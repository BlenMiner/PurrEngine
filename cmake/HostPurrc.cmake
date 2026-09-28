# Web builds generate their games' C with a native purrc: a WebAssembly one
# couldn't run during the build. This builds purrc from the same sources for
# the machine doing the build, in <build>/host, before anything needs it.

include(ExternalProject)
set(_purr_host_dir "${CMAKE_BINARY_DIR}/host")
if(CMAKE_HOST_WIN32)
    set(PURRC_PATH "${_purr_host_dir}/bin/purrc.exe")
else()
    set(PURRC_PATH "${_purr_host_dir}/bin/purrc")
endif()

ExternalProject_Add(purrc_host
    SOURCE_DIR "${PROJECT_SOURCE_DIR}"
    BINARY_DIR "${_purr_host_dir}"
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DPURR_TOOLS_ONLY=ON
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${_purr_host_dir}" --target purrc
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${PURRC_PATH}")

set(PURRC_COMMAND "${PURRC_PATH}")
set(PURRC_DEPENDS purrc_host "${PURRC_PATH}")
