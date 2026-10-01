# Finds clang so the project builds without clang on PATH.
# Search order: $LLVM_ROOT/bin, a standalone LLVM install, PATH, Visual Studio's bundled clang.
# The standalone install beats PATH because Visual Studio environments (developer shells,
# IDE toolchains) put Visual Studio's older bundled clang on PATH.

if(DEFINED ENV{LLVM_ROOT})
    find_program(TIDE_CLANG NAMES clang PATHS "$ENV{LLVM_ROOT}/bin" NO_DEFAULT_PATH)
endif()

if(CMAKE_HOST_WIN32)
    find_program(TIDE_CLANG NAMES clang PATHS "$ENV{ProgramFiles}/LLVM/bin" NO_DEFAULT_PATH)
endif()

find_program(TIDE_CLANG NAMES clang)

if(NOT TIDE_CLANG AND CMAKE_HOST_WIN32)
    set(_tide_search_paths "")

    set(_tide_pf86 "ProgramFiles(x86)")
    set(_tide_vswhere "$ENV{${_tide_pf86}}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(EXISTS "${_tide_vswhere}")
        execute_process(
            COMMAND "${_tide_vswhere}" -latest -products *
                    -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang
                    -property installationPath
            OUTPUT_VARIABLE _tide_vs_path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_tide_vs_path)
            if("$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "ARM64")
                list(APPEND _tide_search_paths "${_tide_vs_path}/VC/Tools/Llvm/ARM64/bin")
            endif()
            list(APPEND _tide_search_paths "${_tide_vs_path}/VC/Tools/Llvm/x64/bin")
        endif()
    endif()

    find_program(TIDE_CLANG NAMES clang PATHS ${_tide_search_paths} NO_DEFAULT_PATH)
endif()

if(NOT TIDE_CLANG)
    message(FATAL_ERROR "clang not found. Install LLVM, add it to PATH, or set LLVM_ROOT.")
endif()

set(CMAKE_C_COMPILER "${TIDE_CLANG}")
# tide's built-in clang is C++ (TIDE_EMBED_LLVM); clang++ is next to clang.
get_filename_component(_tide_clang_bin "${TIDE_CLANG}" DIRECTORY)
find_program(TIDE_CLANGXX NAMES clang++ HINTS "${_tide_clang_bin}" NO_DEFAULT_PATH)
if(TIDE_CLANGXX)
    set(CMAKE_CXX_COMPILER "${TIDE_CLANGXX}")
endif()

# CMake's Windows clang setup also requires a resource compiler; llvm-rc ships next to clang.
if(CMAKE_HOST_WIN32)
    get_filename_component(_tide_clang_dir "${TIDE_CLANG}" DIRECTORY)
    find_program(TIDE_LLVM_RC NAMES llvm-rc HINTS "${_tide_clang_dir}")
    if(TIDE_LLVM_RC)
        set(CMAKE_RC_COMPILER "${TIDE_LLVM_RC}")
    endif()
endif()
