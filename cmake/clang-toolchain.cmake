# Finds clang so the project builds without clang on PATH.
# Search order: $LLVM_ROOT/bin, a standalone LLVM install, PATH, Visual Studio's bundled clang.
# The standalone install beats PATH because Visual Studio environments (developer shells,
# IDE toolchains) put Visual Studio's older bundled clang on PATH.

if(DEFINED ENV{LLVM_ROOT})
    find_program(PURR_CLANG NAMES clang PATHS "$ENV{LLVM_ROOT}/bin" NO_DEFAULT_PATH)
endif()

if(CMAKE_HOST_WIN32)
    find_program(PURR_CLANG NAMES clang PATHS "$ENV{ProgramFiles}/LLVM/bin" NO_DEFAULT_PATH)
endif()

find_program(PURR_CLANG NAMES clang)

if(NOT PURR_CLANG AND CMAKE_HOST_WIN32)
    set(_purr_search_paths "")

    set(_purr_pf86 "ProgramFiles(x86)")
    set(_purr_vswhere "$ENV{${_purr_pf86}}/Microsoft Visual Studio/Installer/vswhere.exe")
    if(EXISTS "${_purr_vswhere}")
        execute_process(
            COMMAND "${_purr_vswhere}" -latest -products *
                    -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang
                    -property installationPath
            OUTPUT_VARIABLE _purr_vs_path
            OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(_purr_vs_path)
            if("$ENV{PROCESSOR_ARCHITECTURE}" STREQUAL "ARM64")
                list(APPEND _purr_search_paths "${_purr_vs_path}/VC/Tools/Llvm/ARM64/bin")
            endif()
            list(APPEND _purr_search_paths "${_purr_vs_path}/VC/Tools/Llvm/x64/bin")
        endif()
    endif()

    find_program(PURR_CLANG NAMES clang PATHS ${_purr_search_paths} NO_DEFAULT_PATH)
endif()

if(NOT PURR_CLANG)
    message(FATAL_ERROR "clang not found. Install LLVM, add it to PATH, or set LLVM_ROOT.")
endif()

set(CMAKE_C_COMPILER "${PURR_CLANG}")

# CMake's Windows clang setup also requires a resource compiler; llvm-rc ships next to clang.
if(CMAKE_HOST_WIN32)
    get_filename_component(_purr_clang_dir "${PURR_CLANG}" DIRECTORY)
    find_program(PURR_LLVM_RC NAMES llvm-rc HINTS "${_purr_clang_dir}")
    if(PURR_LLVM_RC)
        set(CMAKE_RC_COMPILER "${PURR_LLVM_RC}")
    endif()
endif()
