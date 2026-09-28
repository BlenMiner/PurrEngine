// purr's built-in C compiler: clang and lld, linked into purr (see
// cmake/LLVM.cmake), so users need no compiler of their own.
//
// `purr cc <clang arguments>` works like clang: its driver runs in this
// process, compiles in it (as clang does by default), and links with lld in it
// too, instead of running a linker program. clang's own main isn't in a
// library, so this is a small version of it (clang/tools/driver in LLVM).

#include "cc.h"

#include <memory>
#include <string>
#include <vector>

#include "clang/Basic/DiagnosticFrontend.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Frontend/Utils.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "lld/Common/Driver.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

// The targets purr builds for: the machine's own, and the web.
extern "C" {
#define PURR_TARGET(name)                                                                                              \
    void LLVMInitialize##name##TargetInfo();                                                                           \
    void LLVMInitialize##name##Target();                                                                               \
    void LLVMInitialize##name##TargetMC();                                                                             \
    void LLVMInitialize##name##AsmPrinter();                                                                           \
    void LLVMInitialize##name##AsmParser();
#if defined(__x86_64__) || defined(_M_X64)
PURR_TARGET(X86)
#define PURR_NATIVE_TARGET X86
#elif defined(__aarch64__) || defined(_M_ARM64)
PURR_TARGET(AArch64)
#define PURR_NATIVE_TARGET AArch64
#endif
PURR_TARGET(WebAssembly)
}

#define PURR_INIT_TARGET_(name)                                                                                        \
    LLVMInitialize##name##TargetInfo();                                                                                \
    LLVMInitialize##name##Target();                                                                                    \
    LLVMInitialize##name##TargetMC();                                                                                  \
    LLVMInitialize##name##AsmPrinter();                                                                                \
    LLVMInitialize##name##AsmParser();
#define PURR_INIT_TARGET(name) PURR_INIT_TARGET_(name)

static void init_targets()
{
    static bool done;
    if (done) return;
    done = true;
    PURR_INIT_TARGET(PURR_NATIVE_TARGET)
    PURR_INIT_TARGET(WebAssembly)
}

// The linkers games need: the web's, and the machine's own.
LLD_HAS_DRIVER(wasm)
#if defined(_WIN32)
LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(mingw)
static const lld::DriverDef linkers[] = {{lld::Wasm, &lld::wasm::link}, {lld::WinLink, &lld::coff::link},
                                         {lld::MinGW, &lld::mingw::link}};
#elif defined(__APPLE__)
LLD_HAS_DRIVER(macho)
static const lld::DriverDef linkers[] = {{lld::Wasm, &lld::wasm::link}, {lld::Darwin, &lld::macho::link}};
#else
LLD_HAS_DRIVER(elf)
static const lld::DriverDef linkers[] = {{lld::Wasm, &lld::wasm::link}, {lld::Gnu, &lld::elf::link}};
#endif

// ---------------------------------------------------------------------------
// Compiling: clang -cc1, as clang/tools/driver/cc1_main.cpp does it

static void backend_error(void *user, const char *message, bool)
{
    auto &diags = *static_cast<clang::DiagnosticsEngine *>(user);
    diags.Report(clang::diag::err_fe_error_backend) << message;
    llvm::sys::RunInterruptHandlers(); // Removes unfinished output files
    llvm::sys::Process::Exit(1);
}

// `args` starts with -cc1, and `self` is purr's path.
static int cc1(llvm::ArrayRef<const char *> args, const char *self)
{
    init_targets();
    auto pch = std::make_shared<clang::PCHContainerOperations>();
    pch->registerWriter(std::make_unique<clang::ObjectFilePCHContainerWriter>());
    pch->registerReader(std::make_unique<clang::ObjectFilePCHContainerReader>());

    // Diagnostics about the arguments wait until the real diagnostics exist.
    clang::DiagnosticOptions diag_opts;
    auto *buffer = new clang::TextDiagnosticBuffer;
    clang::DiagnosticsEngine diags(clang::DiagnosticIDs::create(), diag_opts, buffer);
    auto invocation = std::make_shared<clang::CompilerInvocation>();
    bool ok = clang::CompilerInvocation::CreateFromArgs(*invocation, args, diags, self);

    auto clang = std::make_unique<clang::CompilerInstance>(std::move(invocation), std::move(pch));
    clang->createVirtualFileSystem(llvm::vfs::getRealFileSystem(), buffer);
    clang->createDiagnostics();
    llvm::install_fatal_error_handler(backend_error, &clang->getDiagnostics());
    buffer->FlushDiagnostics(clang->getDiagnostics());
    if (ok) ok = clang::ExecuteCompilerInvocation(clang.get());
    llvm::remove_fatal_error_handler();
    return ok ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Linking: the driver's link command, run by lld in this process

static int link(const llvm::Triple &triple, const llvm::opt::ArgStringList &args)
{
    // lld picks its flavor from the program's name, as the driver would have
    // run it; ld.lld also links MinGW, which the driver asks for with -m.
    const char *name = triple.isWasm()                        ? "wasm-ld"
                       : triple.isOSDarwin()                  ? "ld64.lld"
                       : triple.isWindowsMSVCEnvironment()    ? "lld-link"
                                                              : "ld.lld";
    std::vector<const char *> argv{name};
    argv.insert(argv.end(), args.begin(), args.end());
    const lld::Result result = lld::lldMain(argv, llvm::outs(), llvm::errs(), linkers);
    return result.retCode;
}

// ---------------------------------------------------------------------------
// The driver, as clang/tools/driver/driver.cpp runs it

static int driver(llvm::SmallVectorImpl<const char *> &args)
{
    init_targets();
    const std::string self = args[0];
    auto vfs = llvm::vfs::getRealFileSystem();

    std::unique_ptr<clang::DiagnosticOptions> diag_opts = clang::CreateAndPopulateDiagOpts(args);
    auto *printer = new clang::TextDiagnosticPrinter(llvm::errs(), *diag_opts);
    printer->setPrefix("purr");
    clang::DiagnosticsEngine diags(clang::DiagnosticIDs::create(), *diag_opts, printer);
    clang::ProcessWarningOptions(diags, *diag_opts, *vfs, /*ReportDiags=*/false);

    clang::driver::Driver driver(self, llvm::sys::getDefaultTargetTriple(), diags, "purr", vfs);
    // Compiles run in this process: clang runs -cc1 itself.
    auto run_cc1 = [&](llvm::SmallVectorImpl<const char *> &argv) {
        llvm::cl::ResetAllOptionOccurrences();
        return cc1(llvm::ArrayRef<const char *>(argv).slice(1), self.c_str());
    };
    driver.CC1Main = run_cc1;
    llvm::CrashRecoveryContext::Enable();

    // The driver looks for a linker program; point it at purr, which never runs
    // as one: link commands go to lld here. Only when linking for a target that
    // asks for it (wasm's doesn't), or clang warns that the argument is unused.
    bool links = true;
    for (size_t i = 1; i < args.size(); i++) {
        const llvm::StringRef a(args[i]);
        if (a == "-c" || a == "-S" || a == "-E" || a == "-fsyntax-only" || a == "-M" || a == "-MM") links = false;
        llvm::StringRef target;
        if ((a == "-target" || a == "--target") && i + 1 < args.size()) target = args[i + 1];
        else if (a.starts_with("--target=")) target = a.substr(sizeof "--target=" - 1);
        if (target.starts_with("wasm")) links = false;
    }
    const std::string ld_path = "--ld-path=" + self;
    if (links) args.push_back(ld_path.c_str());

    std::unique_ptr<clang::driver::Compilation> compilation(driver.BuildCompilation(args));
    if (!compilation || compilation->containsError()) return 1;
    const llvm::Triple &triple = compilation->getDefaultToolChain().getTriple();
    for (const clang::driver::Command &job : compilation->getJobs()) {
        int code;
        const llvm::opt::ArgStringList &job_args = job.getArguments();
        if (!job_args.empty() && llvm::StringRef(job_args[0]) == "-cc1") {
            std::string error;
            bool failed = false;
            code = job.Execute({}, &error, &failed);
            if (failed) llvm::errs() << "purr: error: " << error << "\n";
        } else {
            code = link(triple, job_args);
        }
        if (code != 0) return code;
    }
    return 0;
}

int purr_cc(const int argc, const char **argv)
{
    // clang's paths are relative to purr's: its headers are in ../lib/clang.
    llvm::SmallVector<const char *, 64> args(argv, argv + argc);
    const std::string self = llvm::sys::fs::getMainExecutable(argv[0], reinterpret_cast<void *>(&purr_cc));
    args[0] = self.c_str();
    if (args.size() > 1 && llvm::StringRef(args[1]) == "cc") args.erase(args.begin() + 1);
    if (args.size() > 1 && llvm::StringRef(args[1]) == "-cc1") {
        return cc1(llvm::ArrayRef<const char *>(args).slice(1), self.c_str());
    }
    return driver(args);
}
