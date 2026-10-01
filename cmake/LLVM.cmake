# tide's built-in clang and lld (compiler/cli/llvm/cc.cpp): tide links LLVM's
# static libraries, so users need no compiler of their own.
#
# The libraries come from LLVM's own release for this machine, downloaded once
# into build/llvm-<version> and pinned; unpacking it needs the zstd program.
# TIDE_LLVM_ROOT can name an unpacked LLVM instead. Defines the tide_cc library,
# and installs clang's own headers into the package, where tide's clang looks
# for them (lib/clang/<major>, next to bin/).

set(TIDE_LLVM_VERSION 23.1.2)
string(REGEX MATCH "^[0-9]+" _tide_llvm_major "${TIDE_LLVM_VERSION}")
set(TIDE_LLVM_ROOT "" CACHE PATH "An unpacked LLVM ${TIDE_LLVM_VERSION} with its static libraries (default: downloaded)")

if(NOT TIDE_LLVM_ROOT)
    if(CMAKE_HOST_WIN32)
        set(_tide_llvm_name "clang+llvm-${TIDE_LLVM_VERSION}-x86_64-pc-windows-msvc")
        set(_tide_llvm_hash ceaee048142fece144752c6f6431cb0905a7a6160f78ab8cf5cf0b6216f99418)
    elseif(CMAKE_HOST_APPLE)
        set(_tide_llvm_name "LLVM-${TIDE_LLVM_VERSION}-macOS-ARM64")
        set(_tide_llvm_hash 3da0e91b5dfe3a5ec795ad2be79b3f5e6f28c8b23edcd3847fad7742b25e0507)
    else()
        set(_tide_llvm_name "LLVM-${TIDE_LLVM_VERSION}-Linux-X64")
        set(_tide_llvm_hash 6382de1c1a210ce5a5cc49d18bc8444d137742e7cbf9b19f4ae602bb1ab52534)
    endif()
    get_filename_component(_tide_llvm_dir "${PROJECT_SOURCE_DIR}/build/llvm-${TIDE_LLVM_VERSION}" ABSOLUTE)
    set(_tide_llvm_root "${_tide_llvm_dir}/${_tide_llvm_name}")
    if(NOT EXISTS "${_tide_llvm_root}/done")
        find_program(TIDE_ZSTD NAMES zstd)
        if(NOT TIDE_ZSTD)
            message(FATAL_ERROR "Unpacking LLVM needs the zstd program (its archives use frames CMake can't read): "
                                "install zstd, or set TIDE_LLVM_ROOT to an unpacked LLVM ${TIDE_LLVM_VERSION}")
        endif()
        set(_tide_llvm_file "${_tide_llvm_dir}/${_tide_llvm_name}.tar.zst")
        message(STATUS "Downloading LLVM ${TIDE_LLVM_VERSION} (about 1 GB, once)")
        file(DOWNLOAD "https://github.com/llvm/llvm-project/releases/download/llvmorg-${TIDE_LLVM_VERSION}/${_tide_llvm_name}.tar.zst"
            "${_tide_llvm_file}" EXPECTED_HASH SHA256=${_tide_llvm_hash} STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            message(FATAL_ERROR "Downloading LLVM failed: ${status}")
        endif()
        # tide needs the headers, the static libraries and clang's own headers:
        # a small part of the archive, which unpacks to several gigabytes.
        file(REMOVE_RECURSE "${_tide_llvm_root}")
        if(CMAKE_HOST_WIN32)
            set(_tide_llvm_tar "${_tide_llvm_dir}/${_tide_llvm_name}.tar")
            execute_process(COMMAND "${TIDE_ZSTD}" -d -q -f --long=31 -o "${_tide_llvm_tar}" "${_tide_llvm_file}"
                RESULT_VARIABLE result)
            if(result EQUAL 0)
                file(ARCHIVE_EXTRACT INPUT "${_tide_llvm_tar}" DESTINATION "${_tide_llvm_dir}"
                    PATTERNS "${_tide_llvm_name}/include/*" "${_tide_llvm_name}/lib/*${CMAKE_STATIC_LIBRARY_SUFFIX}"
                             "${_tide_llvm_name}/lib/clang/*/include/*")
            endif()
            file(REMOVE "${_tide_llvm_tar}")
        else()
            # Straight from zstd into tar: the Linux archive unpacks to 12 GB. On
            # macOS, LLVM's lld too, which links tide (see below).
            find_program(TIDE_TAR NAMES tar REQUIRED)
            if(CMAKE_HOST_APPLE)
                set(_tide_llvm_lld "${_tide_llvm_name}/bin/lld" "${_tide_llvm_name}/bin/ld64.lld")
            endif()
            execute_process(COMMAND "${TIDE_ZSTD}" -d -q -c --long=31 "${_tide_llvm_file}"
                COMMAND "${TIDE_TAR}" -x -f - -C "${_tide_llvm_dir}" --exclude=*.so* --exclude=*.dylib
                    "${_tide_llvm_name}/include" "${_tide_llvm_name}/lib" ${_tide_llvm_lld}
                RESULT_VARIABLE result)
        endif()
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Unpacking ${_tide_llvm_file} failed")
        endif()
        file(REMOVE "${_tide_llvm_file}")
        # Only the libraries of LLVM, clang, lld and Polly (which LLVM's Linux
        # release links into clang), and clang's headers.
        file(GLOB _tide_llvm_unneeded LIST_DIRECTORIES true "${_tide_llvm_root}/lib/*" "${_tide_llvm_root}/lib/clang/*/*")
        list(FILTER _tide_llvm_unneeded EXCLUDE REGEX
            "/lib/((lib)?(LLVM|clang|lld|Polly)[^/]*\\${CMAKE_STATIC_LIBRARY_SUFFIX}|clang|clang/[^/]+/include)$")
        file(REMOVE_RECURSE ${_tide_llvm_unneeded})
        file(TOUCH "${_tide_llvm_root}/done")
    endif()
    set(TIDE_LLVM_ROOT "${_tide_llvm_root}")
endif()

if(NOT EXISTS "${TIDE_LLVM_ROOT}/include/clang/Driver/Driver.h")
    message(FATAL_ERROR "TIDE_EMBED_LLVM needs LLVM ${TIDE_LLVM_VERSION}'s headers and static libraries: "
                        "set TIDE_LLVM_ROOT to an unpacked LLVM (not found in '${TIDE_LLVM_ROOT}')")
endif()

enable_language(CXX)

add_library(tide_cc STATIC "${PROJECT_SOURCE_DIR}/compiler/cli/llvm/cc.cpp")
target_include_directories(tide_cc SYSTEM PRIVATE "${TIDE_LLVM_ROOT}/include")
target_include_directories(tide_cc PUBLIC "${PROJECT_SOURCE_DIR}/compiler/cli/llvm")
target_compile_features(tide_cc PRIVATE cxx_std_17)
# As LLVM is built.
target_compile_options(tide_cc PRIVATE -fno-rtti -fno-exceptions)

# Every static library of LLVM, clang, lld and Polly; the linker takes only
# what's used. Not LLVM-C, libclang and the like: those import LLVM's DLLs.
file(GLOB _tide_llvm_libs "${TIDE_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}LLVM*${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${TIDE_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}clang*${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${TIDE_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}lld*${CMAKE_STATIC_LIBRARY_SUFFIX}"
    "${TIDE_LLVM_ROOT}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}Polly*${CMAKE_STATIC_LIBRARY_SUFFIX}")
list(FILTER _tide_llvm_libs EXCLUDE REGEX "/(lib)?(LLVM-C|LLVM|clang|clang-cpp|LTO|Remarks)\\.[a-z]+$")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # GNU ld reads each library once, and LLVM's depend on each other in circles.
    list(JOIN _tide_llvm_libs "," _tide_llvm_group)
    target_link_libraries(tide_cc PUBLIC "$<LINK_GROUP:RESCAN,${_tide_llvm_group}>")
    # tide runs on systems with an older C++ library than the one it's built with.
    target_link_options(tide_cc INTERFACE -static-libstdc++ -static-libgcc)
else()
    target_link_libraries(tide_cc PUBLIC ${_tide_llvm_libs})
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(tide_cc PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
endif()
if(APPLE)
    # LLVM's macOS release holds its libraries as LLVM bitcode (it's built with
    # ThinLTO), which Apple's linker can't read. Its own lld can, and caches
    # the code it makes from them.
    if(NOT EXISTS "${TIDE_LLVM_ROOT}/bin/ld64.lld")
        message(FATAL_ERROR "LLVM's lld is missing from ${TIDE_LLVM_ROOT}/bin: macOS links tide with it")
    endif()
    target_link_options(tide_cc INTERFACE -fuse-ld=lld "--ld-path=${TIDE_LLVM_ROOT}/bin/ld64.lld"
        "LINKER:-cache_path_lto,${CMAKE_BINARY_DIR}/lto-cache")
endif()

# LLVM's releases are built with zlib and zstd, and on Windows with libxml2
# (for lld's manifests), without including them: build them for tide, only as
# the static libraries. A function, so these settings stay local.
include(FetchContent)
FetchContent_Declare(tide_zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(tide_zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR build/cmake)
FetchContent_Declare(tide_libxml2
    URL https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz
    URL_HASH SHA256=98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
function(tide_add_llvm_dependencies)
    set(BUILD_SHARED_LIBS OFF)
    set(ZLIB_BUILD_SHARED OFF)
    set(ZLIB_BUILD_TESTING OFF)
    set(ZLIB_INSTALL OFF)
    set(ZSTD_BUILD_SHARED OFF)
    set(ZSTD_BUILD_PROGRAMS OFF)
    set(ZSTD_BUILD_TESTS OFF)
    set(ZSTD_LEGACY_SUPPORT OFF)
    set(ZSTD_MULTITHREAD_SUPPORT OFF)
    FetchContent_MakeAvailable(tide_zlib tide_zstd)
    target_compile_options(zlibstatic PRIVATE -w)
    target_compile_options(libzstd_static PRIVATE -w)
    target_link_libraries(tide_cc PUBLIC zlibstatic libzstd_static)
    if(WIN32)
        foreach(feature CATALOG DEBUG HTML ICONV MODULES PROGRAMS TESTS)
            set(LIBXML2_WITH_${feature} OFF)
        endforeach()
        FetchContent_MakeAvailable(tide_libxml2)
        target_compile_options(LibXml2 PRIVATE -w)
        target_link_libraries(tide_cc PUBLIC LibXml2 ntdll version)
    endif()
endfunction()
tide_add_llvm_dependencies()

install(DIRECTORY "${TIDE_LLVM_ROOT}/lib/clang/${_tide_llvm_major}/include"
    DESTINATION "lib/clang/${_tide_llvm_major}" COMPONENT tide)
