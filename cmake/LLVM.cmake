# purr's built-in clang and lld (compiler/cli/llvm/cc.cpp): purr links LLVM's
# static libraries, so users need no compiler of their own.
#
# The libraries come from LLVM's own release for this machine, downloaded once
# into build/llvm-<version> and pinned; unpacking it needs the zstd program.
# PURR_LLVM_ROOT can name an unpacked LLVM instead. Defines the purr_cc library,
# and installs clang's own headers into the package, where purr's clang looks
# for them (lib/clang/<major>, next to bin/).

set(PURR_LLVM_VERSION 23.1.2)
string(REGEX MATCH "^[0-9]+" _purr_llvm_major "${PURR_LLVM_VERSION}")
set(PURR_LLVM_ROOT "" CACHE PATH "An unpacked LLVM ${PURR_LLVM_VERSION} with its static libraries (default: downloaded)")

if(NOT PURR_LLVM_ROOT)
    if(CMAKE_HOST_WIN32)
        set(_purr_llvm_name "clang+llvm-${PURR_LLVM_VERSION}-x86_64-pc-windows-msvc")
        set(_purr_llvm_hash ceaee048142fece144752c6f6431cb0905a7a6160f78ab8cf5cf0b6216f99418)
    elseif(CMAKE_HOST_APPLE)
        set(_purr_llvm_name "LLVM-${PURR_LLVM_VERSION}-macOS-ARM64")
        set(_purr_llvm_hash 3da0e91b5dfe3a5ec795ad2be79b3f5e6f28c8b23edcd3847fad7742b25e0507)
    else()
        set(_purr_llvm_name "LLVM-${PURR_LLVM_VERSION}-Linux-X64")
        set(_purr_llvm_hash 6382de1c1a210ce5a5cc49d18bc8444d137742e7cbf9b19f4ae602bb1ab52534)
    endif()
    get_filename_component(_purr_llvm_dir "${PROJECT_SOURCE_DIR}/build/llvm-${PURR_LLVM_VERSION}" ABSOLUTE)
    set(_purr_llvm_root "${_purr_llvm_dir}/${_purr_llvm_name}")
    if(NOT EXISTS "${_purr_llvm_root}/done")
        find_program(PURR_ZSTD NAMES zstd)
        if(NOT PURR_ZSTD)
            message(FATAL_ERROR "Unpacking LLVM needs the zstd program (its archives use frames CMake can't read): "
                                "install zstd, or set PURR_LLVM_ROOT to an unpacked LLVM ${PURR_LLVM_VERSION}")
        endif()
        set(_purr_llvm_file "${_purr_llvm_dir}/${_purr_llvm_name}.tar.zst")
        message(STATUS "Downloading LLVM ${PURR_LLVM_VERSION} (about 1 GB, once)")
        file(DOWNLOAD "https://github.com/llvm/llvm-project/releases/download/llvmorg-${PURR_LLVM_VERSION}/${_purr_llvm_name}.tar.zst"
            "${_purr_llvm_file}" EXPECTED_HASH SHA256=${_purr_llvm_hash} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Downloading LLVM failed: ${status}")
        endif()
        # purr needs the headers, the static libraries and clang's own headers:
        # a small part of the archive, which unpacks to several gigabytes.
        file(REMOVE_RECURSE "${_purr_llvm_root}")
        if(CMAKE_HOST_WIN32)
            set(_purr_llvm_tar "${_purr_llvm_dir}/${_purr_llvm_name}.tar")
            execute_process(COMMAND "${PURR_ZSTD}" -d -q -f --long=31 -o "${_purr_llvm_tar}" "${_purr_llvm_file}"
                RESULT_VARIABLE result)
            if(result EQUAL 0)
                file(ARCHIVE_EXTRACT INPUT "${_purr_llvm_tar}" DESTINATION "${_purr_llvm_dir}"
                    PATTERNS "${_purr_llvm_name}/include/*" "${_purr_llvm_name}/lib/*${CMAKE_STATIC_LIBRARY_SUFFIX}"
                             "${_purr_llvm_name}/lib/clang/*/include/*")
            endif()
            file(REMOVE "${_purr_llvm_tar}")
        else()
            # Straight from zstd into tar: the Linux archive unpacks to 12 GB.
            find_program(PURR_TAR NAMES tar REQUIRED)
            execute_process(COMMAND "${PURR_ZSTD}" -d -q -c --long=31 "${_purr_llvm_file}"
                COMMAND "${PURR_TAR}" -x -f - -C "${_purr_llvm_dir}" --exclude=*.so* --exclude=*.dylib
                    "${_purr_llvm_name}/include" "${_purr_llvm_name}/lib"
                RESULT_VARIABLE result)
        endif()
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Unpacking ${_purr_llvm_file} failed")
        endif()
        file(REMOVE "${_purr_llvm_file}")
        # Only LLVM's, clang's and lld's libraries, and clang's headers.
        file(GLOB _purr_llvm_unneeded LIST_DIRECTORIES true "${_purr_llvm_root}/lib/*" "${_purr_llvm_root}/lib/clang/*/*")
        list(FILTER _purr_llvm_unneeded EXCLUDE REGEX
            "/lib/((lib)?(LLVM|clang|lld)[^/]*\\${CMAKE_STATIC_LIBRARY_SUFFIX}|clang|clang/[^/]+/include)$")
        file(REMOVE_RECURSE ${_purr_llvm_unneeded})
        file(TOUCH "${_purr_llvm_root}/done")
    endif()
    set(PURR_LLVM_ROOT "${_purr_llvm_root}")
endif()

if(NOT EXISTS "${PURR_LLVM_ROOT}/include/clang/Driver/Driver.h")
    message(FATAL_ERROR "PURR_EMBED_LLVM needs LLVM ${PURR_LLVM_VERSION}'s headers and static libraries: "
                        "set PURR_LLVM_ROOT to an unpacked LLVM (not found in '${PURR_LLVM_ROOT}')")
endif()

enable_language(CXX)

add_library(purr_cc STATIC "${PROJECT_SOURCE_DIR}/compiler/cli/llvm/cc.cpp")
target_include_directories(purr_cc SYSTEM PRIVATE "${PURR_LLVM_ROOT}/include")
target_include_directories(purr_cc PUBLIC "${PROJECT_SOURCE_DIR}/compiler/cli/llvm")
target_compile_features(purr_cc PRIVATE cxx_std_17)
# As LLVM is built.
target_compile_options(purr_cc PRIVATE -fno-rtti -fno-exceptions)

# Every static library of LLVM, clang and lld; the linker takes only what's used.
# Not LLVM-C, libclang and the like: those import LLVM's DLLs.
file(GLOB _purr_llvm_libs "${PURR_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVM*${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${PURR_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}clang*${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${PURR_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}lld*${CMAKE_STATIC_LIBRARY_SUFFIX}")
list(FILTER _purr_llvm_libs EXCLUDE REGEX "/(lib)?(LLVM-C|LLVM|clang|clang-cpp|LTO|Remarks)\\.[a-z]+$")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GNU ld reads each library once, and LLVM's depend on each other in circles.
    list(JOIN _purr_llvm_libs "," _purr_llvm_group)
    target_link_libraries(purr_cc PUBLIC "$<LINK_GROUP:RESCAN,${_purr_llvm_group}>")
    # purr runs on systems with an older C++ library than the one it's built with.
    target_link_options(purr_cc INTERFACE -static-libstdc++ -static-libgcc)
else()
    target_link_libraries(purr_cc PUBLIC ${_purr_llvm_libs})
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(purr_cc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()

# LLVM's releases are built with zlib and zstd, and on Windows with libxml2
# (for lld's manifests), without including them: build them for purr, only as
# the static libraries. A function, so these settings stay local.
include(FetchContent)
FetchContent_Declare(purr_zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(purr_zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR build/cmake)
FetchContent_Declare(purr_libxml2
    URL https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz
    URL_HASH SHA256=98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
function(purr_add_llvm_dependencies)
    set(BUILD_SHARED_LIBS OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_INSTALL OFF)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_LEGACY_SUPPORT OFF)
    set(ZSTD_MULTITHREAD_SUPPORT OFF)
    FetchContent_MakeAvailable(purr_zlib purr_zstd)
    target_compile_options(zlibstatic PRIVATE -w)
    target_compile_options(libzstd_static PRIVATE -w)
    target_link_libraries(purr_cc PUBLIC zlibstatic libzstd_static)
    if(WIN32)
        foreach(feature CATALOG DEBUG HTML ICONV MODULES PROGRAMS TESTS)
            set(LIBXML2_WITH_${feature} OFF)
        endforeach()
        FetchContent_MakeAvailable(purr_libxml2)
        target_compile_options(LibXml2 PRIVATE -w)
        target_link_libraries(purr_cc PUBLIC LibXml2 ntdll version)
    endif()
endfunction()
purr_add_llvm_dependencies()

install(DIRECTORY "${PURR_LLVM_ROOT}/lib/clang/${_purr_llvm_major}/include"
    DESTINATION "lib/clang/${_purr_llvm_major}" COMPONENT purr)
