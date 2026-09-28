# Windows builds on the MinGW-w64 target (x86_64-w64-windows-gnu) instead of
# Visual Studio's. Games built by purr use this target: its C runtime can ship
# with purr, and Microsoft's can't. The C library is the UCRT, part of Windows
# since 10, so games need no DLLs besides Windows' own.
#
# The headers, import libraries and compiler runtime come from llvm-mingw,
# downloaded once into build/llvm-mingw-<version> and pinned. clang is found like
# native builds find it (clang-toolchain.cmake), and links with lld.

set(PURR_MINGW_VERSION 20260922)
set(PURR_MINGW_TARGET x86_64-w64-windows-gnu)

include("${CMAKE_CURRENT_LIST_DIR}/clang-toolchain.cmake")
set(CMAKE_C_COMPILER_TARGET ${PURR_MINGW_TARGET})
# CMake's MinGW setup expects windres, which LLVM has under its own name.
get_filename_component(_purr_clang_dir "${PURR_CLANG}" DIRECTORY)
find_program(PURR_LLVM_WINDRES NAMES llvm-windres HINTS "${_purr_clang_dir}")
if(PURR_LLVM_WINDRES)
    set(CMAKE_RC_COMPILER "${PURR_LLVM_WINDRES}")
endif()

# Shared by every MinGW build tree, and by CMake's own test projects. The Linux
# build of llvm-mingw is the smallest download; the files taken from it are the
# same in every build.
get_filename_component(_purr_mingw_dir "${CMAKE_CURRENT_LIST_DIR}/../build/llvm-mingw-${PURR_MINGW_VERSION}" ABSOLUTE)
if(NOT EXISTS "${_purr_mingw_dir}/done")
    set(_purr_mingw_name "llvm-mingw-${PURR_MINGW_VERSION}-ucrt-ubuntu-22.04-x86_64")
    set(_purr_mingw_file "${_purr_mingw_dir}/${_purr_mingw_name}.tar.xz")
    message(STATUS "Downloading llvm-mingw ${PURR_MINGW_VERSION}")
    file(DOWNLOAD "https://github.com/mstorsjo/llvm-mingw/releases/download/${PURR_MINGW_VERSION}/${_purr_mingw_name}.tar.xz"
        "${_purr_mingw_file}" EXPECTED_HASH SHA256=bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21
        STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Downloading llvm-mingw failed: ${status}")
    endif()
    # Its x86_64 include folder is a link to the shared one, which Windows
    # can't extract; take the shared one.
    file(ARCHIVE_EXTRACT INPUT "${_purr_mingw_file}" DESTINATION "${_purr_mingw_dir}/extract"
        PATTERNS "${_purr_mingw_name}/generic-w64-mingw32/include/*" "${_purr_mingw_name}/x86_64-w64-mingw32/lib/*"
                 "${_purr_mingw_name}/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a")
    set(_purr_mingw_from "${_purr_mingw_dir}/extract/${_purr_mingw_name}")
    file(REMOVE_RECURSE "${_purr_mingw_dir}/sysroot")
    file(MAKE_DIRECTORY "${_purr_mingw_dir}/sysroot")
    file(RENAME "${_purr_mingw_from}/x86_64-w64-mingw32" "${_purr_mingw_dir}/sysroot/x86_64-w64-mingw32")
    file(RENAME "${_purr_mingw_from}/generic-w64-mingw32/include" "${_purr_mingw_dir}/sysroot/x86_64-w64-mingw32/include")
    file(GLOB _purr_mingw_builtins "${_purr_mingw_from}/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a")
    file(RENAME "${_purr_mingw_builtins}" "${_purr_mingw_dir}/libclang_rt.builtins-x86_64.a")
    file(REMOVE_RECURSE "${_purr_mingw_dir}/extract")
    file(REMOVE "${_purr_mingw_file}")
    file(TOUCH "${_purr_mingw_dir}/done")
endif()

set(PURR_MINGW_SYSROOT "${_purr_mingw_dir}/sysroot")
set(PURR_MINGW_BUILTINS "${_purr_mingw_dir}/libclang_rt.builtins-x86_64.a")
set(CMAKE_SYSROOT "${PURR_MINGW_SYSROOT}")

# The compiler runtime is passed by path, and the C runtime's libraries by name
# (what clang links by default for MinGW, and what CMake adds on Windows): LLVM's
# releases default to GCC's runtime for this target, and don't include ours.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -nodefaultlibs")
set(CMAKE_C_STANDARD_LIBRARIES "-lmingw32 -lmingwex -lmoldname -lmsvcrt -lkernel32 -luser32 -lgdi32 -lwinspool \
-lshell32 -lole32 -loleaut32 -luuid -lcomdlg32 -ladvapi32 \"${PURR_MINGW_BUILTINS}\"")
