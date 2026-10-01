# Web builds: WebAssembly through clang's own wasm target, with wasi-libc as the
# C library. No Emscripten: the browser side is our own JavaScript
# (platform/web), and tests run under Node's WASI (cmake/run_wasi.mjs).
#
# The C library and compiler runtime come from wasi-sdk, downloaded once into
# build/wasi-sdk-<version> and pinned. clang is found like native builds find
# it (clang-toolchain.cmake); it needs its WebAssembly target and wasm-ld, which
# LLVM's releases include (Linux distributions ship wasm-ld in `lld`).
#
# wasi-sdk 24 is built with LLVM 18, so any clang from 18 on can link its C
# library; newer releases need a newer wasm-ld than many systems have.
#
# The target has threads (wasi-threads): memory the page shares between its
# workers. Pages that can't share memory (not cross-origin isolated) run the
# same program on one thread (platform/web/tide.js). Newer clang builds for
# threads from the `-threads` target alone, but older clang (18 and 20 among
# them) needs -pthread when compiling (CMakeLists.txt) and --shared-memory
# when linking (TideFlags.cmake): without them, programs build without
# threads and nothing says so.

set(TIDE_WASI_SDK_VERSION 24)
set(TIDE_WASI_TARGET wasm32-wasip1-threads)

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
# Apple's clang has no WebAssembly target; Homebrew's LLVM does (with lld for wasm-ld).
if(CMAKE_HOST_APPLE AND NOT DEFINED ENV{LLVM_ROOT})
    find_program(TIDE_CLANG NAMES clang PATHS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin NO_DEFAULT_PATH)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/clang-toolchain.cmake")
set(CMAKE_C_COMPILER_TARGET ${TIDE_WASI_TARGET})

# Shared by every web build tree, and by CMake's own test projects.
get_filename_component(_tide_wasi_dir "${CMAKE_CURRENT_LIST_DIR}/../build/wasi-sdk-${TIDE_WASI_SDK_VERSION}" ABSOLUTE)
set(_tide_wasi_url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${TIDE_WASI_SDK_VERSION}")
function(_tide_wasi_fetch file hash)
    if(EXISTS "${_tide_wasi_dir}/${file}.done")
        return()
    endif()
    message(STATUS "Downloading ${file} (wasi-sdk ${TIDE_WASI_SDK_VERSION})")
    file(DOWNLOAD "${_tide_wasi_url}/${file}" "${_tide_wasi_dir}/${file}" EXPECTED_HASH SHA256=${hash} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Downloading ${file} failed: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_tide_wasi_dir}/${file}" DESTINATION "${_tide_wasi_dir}")
    file(REMOVE "${_tide_wasi_dir}/${file}")
    file(TOUCH "${_tide_wasi_dir}/${file}.done")
endfunction()
_tide_wasi_fetch(wasi-sysroot-${TIDE_WASI_SDK_VERSION}.0.tar.gz 35172f7d2799485b15a46b1d87f50a585d915ec662080f005d99153a50888f08)
_tide_wasi_fetch(libclang_rt.builtins-wasm32-wasi-${TIDE_WASI_SDK_VERSION}.0.tar.gz
    7e33c0df758b90469b1de3ca158e2d0a7f71934d5884525ba6a372de0b3b0ec7)

set(TIDE_WASI_SYSROOT "${_tide_wasi_dir}/wasi-sysroot-${TIDE_WASI_SDK_VERSION}.0")
set(TIDE_WASI_BUILTINS "${_tide_wasi_dir}/libclang_rt.builtins-wasm32-wasi-${TIDE_WASI_SDK_VERSION}.0/libclang_rt.builtins-wasm32.a")
# Older wasi-sdk releases keep their libraries under the target's older name,
# wasm32-wasi-threads, which newer clang calls deprecated; give them the current one.
if(NOT EXISTS "${TIDE_WASI_SYSROOT}/lib/${TIDE_WASI_TARGET}")
    file(COPY "${TIDE_WASI_SYSROOT}/lib/wasm32-wasi-threads/" DESTINATION "${TIDE_WASI_SYSROOT}/lib/${TIDE_WASI_TARGET}")
endif()
set(CMAKE_SYSROOT "${TIDE_WASI_SYSROOT}")

# The compiler runtime is passed by path: clang looks for it in its own install,
# which LLVM's releases don't fill for WASI.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nodefaultlibs")
set(CMAKE_C_STANDARD_LIBRARIES "-lc \"${TIDE_WASI_BUILTINS}\"")
set(CMAKE_EXECUTABLE_SUFFIX ".wasm")

find_program(TIDE_NODE NAMES node)
if(NOT TIDE_NODE)
    message(FATAL_ERROR "Node not found. Web tests run under Node's WASI: install Node (https://nodejs.org).")
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR "${TIDE_NODE};--no-warnings;${CMAKE_CURRENT_LIST_DIR}/run_wasi.mjs")
