# Android builds: the engine, its tests and games for Android, on arm64
# (phones and tablets) or x86_64 (the emulator, and some Chromebooks).
#
# clang is found like native builds find it (clang-toolchain.cmake), and links
# with lld. Android's C library (bionic), its headers and the compiler runtime
# come from Android's NDK, which Google's license keeps out of this repo and
# the package: the build finds the one installed, from ANDROID_NDK_HOME or
# ANDROID_NDK_ROOT, else the newest in the SDK (ANDROID_HOME, ANDROID_SDK_ROOT,
# or where Android Studio puts it).
#
# Tests run on a device or emulator through adb (cmake/run_android.mjs).

set(TIDE_ANDROID_API 26 CACHE STRING "The oldest Android version builds run on, as an API level (26: Android 8.0)")
set(TIDE_ANDROID_ABI x86_64 CACHE STRING "arm64-v8a (devices) or x86_64 (the emulator)")
if(TIDE_ANDROID_ABI STREQUAL "arm64-v8a")
    set(_tide_android_arch aarch64)
elseif(TIDE_ANDROID_ABI STREQUAL "x86_64")
    set(_tide_android_arch x86_64)
else()
    message(FATAL_ERROR "TIDE_ANDROID_ABI is arm64-v8a or x86_64, not ${TIDE_ANDROID_ABI}")
endif()

set(CMAKE_SYSTEM_NAME Linux) # CMake's own Android support would take the NDK's clang
set(CMAKE_SYSTEM_PROCESSOR ${_tide_android_arch})
include("${CMAKE_CURRENT_LIST_DIR}/clang-toolchain.cmake")
set(CMAKE_C_COMPILER_TARGET ${_tide_android_arch}-linux-android${TIDE_ANDROID_API})

# The NDK: the newest under a folder of them, by version.
function(_tide_newest_ndk folder out)
    file(GLOB found LIST_DIRECTORIES true "${folder}/*")
    set(best "")
    set(best_version "0")
    foreach(dir IN LISTS found)
        get_filename_component(version "${dir}" NAME)
        if(EXISTS "${dir}/toolchains/llvm/prebuilt" AND version VERSION_GREATER best_version)
            set(best "${dir}")
            set(best_version "${version}")
        endif()
    endforeach()
    set(${out} "${best}" PARENT_SCOPE)
endfunction()

if(NOT TIDE_ANDROID_NDK)
    foreach(var ANDROID_NDK_HOME ANDROID_NDK_ROOT)
        if(NOT TIDE_ANDROID_NDK AND DEFINED ENV{${var}} AND EXISTS "$ENV{${var}}/toolchains/llvm/prebuilt")
            set(TIDE_ANDROID_NDK "$ENV{${var}}")
        endif()
    endforeach()
    set(_tide_sdks "$ENV{ANDROID_HOME}" "$ENV{ANDROID_SDK_ROOT}")
    if(CMAKE_HOST_WIN32)
        list(APPEND _tide_sdks "$ENV{LOCALAPPDATA}/Android/Sdk")
    elseif(CMAKE_HOST_APPLE)
        list(APPEND _tide_sdks "$ENV{HOME}/Library/Android/sdk")
    else()
        list(APPEND _tide_sdks "$ENV{HOME}/Android/Sdk")
    endif()
    foreach(sdk IN LISTS _tide_sdks)
        if(NOT TIDE_ANDROID_NDK AND sdk AND EXISTS "${sdk}/ndk")
            _tide_newest_ndk("${sdk}/ndk" TIDE_ANDROID_NDK)
        endif()
    endforeach()
    if(NOT TIDE_ANDROID_NDK)
        message(FATAL_ERROR "Android's NDK not found. Install it (Android Studio's SDK Manager, or "
                            "https://developer.android.com/ndk/downloads), or set ANDROID_NDK_HOME.")
    endif()
    file(TO_CMAKE_PATH "${TIDE_ANDROID_NDK}" TIDE_ANDROID_NDK)
    set(TIDE_ANDROID_NDK "${TIDE_ANDROID_NDK}" CACHE PATH "Android's NDK, which Android builds take bionic from")
endif()

file(GLOB _tide_ndk_host LIST_DIRECTORIES true "${TIDE_ANDROID_NDK}/toolchains/llvm/prebuilt/*")
list(GET _tide_ndk_host 0 _tide_ndk_host)
set(CMAKE_SYSROOT "${_tide_ndk_host}/sysroot")
file(GLOB TIDE_ANDROID_BUILTINS "${_tide_ndk_host}/lib/clang/*/lib/linux/libclang_rt.builtins-${_tide_android_arch}-android.a")
list(GET TIDE_ANDROID_BUILTINS 0 TIDE_ANDROID_BUILTINS)

# The compiler runtime is passed by path, and the C library's libraries by
# name: clang looks for the runtime in its own install, which LLVM's releases
# don't fill for Android. Pages of 16 KiB, which Android 15's devices can have.
set(_tide_android_link "-fuse-ld=lld -nodefaultlibs -Wl,-z,max-page-size=16384")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_tide_android_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_tide_android_link}")
set(CMAKE_C_STANDARD_LIBRARIES "-lc -lm -ldl \"${TIDE_ANDROID_BUILTINS}\"")

find_program(TIDE_NODE NAMES node)
if(NOT TIDE_NODE)
    message(FATAL_ERROR "Node not found. Android tests run on a device through Node and adb: install Node (https://nodejs.org).")
endif()
set(CMAKE_CROSSCOMPILING_EMULATOR "${TIDE_NODE};--no-warnings;${CMAKE_CURRENT_LIST_DIR}/run_android.mjs")
