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

set(PURR_WASI_SDK_VERSION 24)
set(PURR_WASI_TARGET wasm32-wasip1)

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)
include("${CMAKE_CURRENT_LIST_DIR}/clang-toolchain.cmake")
set(CMAKE_C_COMPILER_TARGET ${PURR_WASI_TARGET})

# Shared by every web build tree, and by CMake's own test projects.
get_filename_component(_purr_wasi_dir "${CMAKE_CURRENT_LIST_DIR}/../build/wasi-sdk-${PURR_WASI_SDK_VERSION}" ABSOLUTE)
set(_purr_wasi_url "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${PURR_WASI_SDK_VERSION}")
function(_purr_wasi_fetch file hash)
    if(EXISTS "${_purr_wasi_dir}/${file}.done")
        return()
    endif()
    message(STATUS "Downloading ${file} (wasi-sdk ${PURR_WASI_SDK_VERSION})")
    file(DOWNLOAD "${_purr_wasi_url}/${file}" "${_purr_wasi_dir}/${file}" EXPECTED_HASH SHA256=${hash} STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
        message(FATAL_ERROR "Downloading ${file} failed: ${status}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${_purr_wasi_dir}/${file}" DESTINATION "${_purr_wasi_dir}")
    file(REMOVE "${_purr_wasi_dir}/${file}")
    file(TOUCH "${_purr_wasi_dir}/${file}.done")
endfunction()
_purr_wasi_fetch(wasi-sysroot-${PURR_WASI_SDK_VERSION}.0.tar.gz 35172f7d2799485b15a46b1d87f50a585d915ec662080f005d99153a50888f08)
_purr_wasi_fetch(libclang_rt.builtins-wasm32-wasi-${PURR_WASI_SDK_VERSION}.0.tar.gz
    7e33c0df758b90469b1de3ca158e2d0a7f71934d5884525ba6a372de0b3b0ec7)

set(PURR_WASI_SYSROOT "${_purr_wasi_dir}/wasi-sysroot-${PURR_WASI_SDK_VERSION}.0")
set(PURR_WASI_BUILTINS "${_purr_wasi_dir}/libclang_rt.builtins-wasm32-wasi-${PURR_WASI_SDK_VERSION}.0/libclang_rt.builtins-wasm32.a")
# This wasi-sdk keeps its libraries under the target's older name, wasm32-wasi,
# which newer clang calls deprecated; give them the current one.
if(NOT EXISTS "${PURR_WASI_SYSROOT}/lib/${PURR_WASI_TARGET}")
    file(COPY "${PURR_WASI_SYSROOT}/lib/wasm32-wasi/" DESTINATION "${PURR_WASI_SYSROOT}/lib/${PURR_WASI_TARGET}")
endif()
set(CMAKE_SYSROOT "${PURR_WASI_SYSROOT}")

# The compiler runtime is passed by path: clang looks for it in its own install,
# which LLVM's releases don't fill for WASI.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nodefaultlibs")
set(CMAKE_C_STANDARD_LIBRARIES "-lc \"${PURR_WASI_BUILTINS}\"")
set(CMAKE_EXECUTABLE_SUFFIX ".wasm")

find_program(PURR_NODE NAMES node)
if(NOT PURR_NODE)
    message(FATAL_ERROR "Node not found. Web tests run under Node's WASI: install Node (https://nodejs.org).")
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR "${PURR_NODE};--no-warnings;${CMAKE_CURRENT_LIST_DIR}/run_wasi.mjs")
