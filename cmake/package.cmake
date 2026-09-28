# Makes the purr package for this machine from the package build trees:
#
#   cmake --preset package && cmake --build --preset package
#   cmake --preset web-package && cmake --build --preset web-package
#   cmake -DNAME=purr-windows-x64 -P cmake/package.cmake
#
# NAME is the archive's name: .zip for Windows, .tar.gz otherwise. It lands in
# build/dist, next to the unpacked package (build/dist/purr) and a SHA256SUMS
# line. NATIVE and WEB override the build trees (default build/package and
# build/web-package); without the web tree, the package can't build web games.

cmake_minimum_required(VERSION 3.25)
get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
if(NOT NAME)
    message(FATAL_ERROR "Pass -DNAME=<archive name>, like purr-windows-x64")
endif()
if(NOT NATIVE)
    set(NATIVE "${source}/build/package")
endif()
if(NOT WEB)
    set(WEB "${source}/build/web-package")
endif()
set(dist "${source}/build/dist")
set(stage "${dist}/purr")

file(REMOVE_RECURSE "${stage}")
foreach(tree IN ITEMS "${NATIVE}" "${WEB}")
    if(NOT EXISTS "${tree}/CMakeCache.txt")
        if(tree STREQUAL NATIVE)
            message(FATAL_ERROR "No native package build in ${tree}: run `cmake --preset package` and build it")
        endif()
        message(WARNING "No web package build in ${tree}: this package won't build web games")
        continue()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" --install "${tree}" --prefix "${stage}" --component purr
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Installing ${tree} failed")
    endif()
endforeach()

if(NAME MATCHES "windows")
    set(archive "${dist}/${NAME}.zip")
    set(format --format=zip)
else()
    set(archive "${dist}/${NAME}.tar.gz")
    set(format "")
endif()
file(REMOVE "${archive}")
file(GLOB entries RELATIVE "${stage}" "${stage}/*")
if(format)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" ${format} ${entries}
        WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${archive}" ${entries}
        WORKING_DIRECTORY "${stage}" RESULT_VARIABLE result)
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Making ${archive} failed")
endif()

get_filename_component(file "${archive}" NAME)
file(SHA256 "${archive}" hash)
file(WRITE "${dist}/${NAME}.sha256" "${hash}  ${file}\n")
message(STATUS "Packaged ${archive}")
