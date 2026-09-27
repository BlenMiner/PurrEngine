# Finds Emscripten and hands over to its own CMake toolchain file, for web builds.
# Search order: $EMSDK (set by emsdk_env), then common emsdk install locations.

set(_purr_emsdk_candidates "$ENV{EMSDK}" "D:/Tools/emsdk" "$ENV{USERPROFILE}/emsdk" "$ENV{HOME}/emsdk" "C:/emsdk")
foreach(_purr_dir IN LISTS _purr_emsdk_candidates)
    if(_purr_dir AND EXISTS "${_purr_dir}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
        set(PURR_EMSDK "${_purr_dir}")
        break()
    endif()
endforeach()

if(NOT PURR_EMSDK)
    message(FATAL_ERROR "Emscripten not found. Install emsdk (https://emscripten.org) and set EMSDK to its folder.")
endif()

include("${PURR_EMSDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake")
