# Web builds generate their games' C with a native tidec: a WebAssembly one
# couldn't run during the build. This builds tidec from the same sources for
# the machine doing the build, in <build>/host, before anything needs it.

include(ExternalProject)
set(_tide_host_dir "${CMAKE_BINARY_DIR}/host")
if(CMAKE_HOST_WIN32)
    set(TIDEC_PATH "${_tide_host_dir}/bin/tidec.exe")
else()
    set(TIDEC_PATH "${_tide_host_dir}/bin/tidec")
endif()

ExternalProject_Add(tidec_host
    SOURCE_DIR "${PROJECT_SOURCE_DIR}"
    BINARY_DIR "${_tide_host_dir}"
    CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DTIDE_TOOLS_ONLY=ON
    BUILD_COMMAND "${CMAKE_COMMAND}" --build "${_tide_host_dir}" --target tidec
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON
    BUILD_BYPRODUCTS "${TIDEC_PATH}")

set(TIDEC_COMMAND "${TIDEC_PATH}")
set(TIDEC_DEPENDS tidec_host "${TIDEC_PATH}")
