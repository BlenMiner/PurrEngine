# purr_add_game(<target> SOURCE <file.purr> [NAME <name>])
#
# Transpiles a PurrLang program with purrc and compiles the generated C into
# <target>, which can then `#include "<name>.h"`. NAME defaults to the source
# file's name. The generated files land in the build tree and are regenerated
# whenever the .purr file or purrc changes.
function(purr_add_game target)
    cmake_parse_arguments(ARG "" "SOURCE;NAME" "" ${ARGN})
    if(NOT ARG_SOURCE)
        message(FATAL_ERROR "purr_add_game(${target}): SOURCE is required")
    endif()

    get_filename_component(source "${ARG_SOURCE}" ABSOLUTE)
    if(ARG_NAME)
        set(name "${ARG_NAME}")
    else()
        get_filename_component(name "${source}" NAME_WE)
    endif()

    set(out_dir "${CMAKE_CURRENT_BINARY_DIR}/purr/${target}")
    set(out_c "${out_dir}/${name}.c")
    set(out_h "${out_dir}/${name}.h")

    add_custom_command(
        OUTPUT "${out_c}" "${out_h}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${out_dir}"
        COMMAND purrc "${source}" -o "${out_dir}" --name "${name}"
        DEPENDS purrc "${source}"
        COMMENT "purrc ${ARG_SOURCE}"
        VERBATIM)

    target_sources(${target} PRIVATE "${out_c}" "${out_h}")
    target_include_directories(${target} PRIVATE "${out_dir}")
    target_link_libraries(${target} PRIVATE purr)
endfunction()
