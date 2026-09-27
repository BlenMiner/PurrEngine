# Runs purrc on SOURCE and checks that it fails with the message written as
# `// expect: <text>` on the first line. SOURCE is a .purr file, or a folder of
# them compiled together as one program (the expectation is on the first line
# of one of its files).
# Files are read as one string: file(STRINGS) would split the line on ';'. The
# leading [^/]* skips a byte-order mark.
if(IS_DIRECTORY "${SOURCE}")
    file(GLOB files "${SOURCE}/*.purr")
else()
    set(files "${SOURCE}")
endif()

set(expected "")
foreach(file IN LISTS files)
    file(READ "${file}" content)
    if(content MATCHES "^[^/]*// expect: ([^\r\n]+)")
        set(expected "${CMAKE_MATCH_1}")
        break()
    endif()
endforeach()
if(expected STREQUAL "")
    message(FATAL_ERROR "${SOURCE}: the first line must be `// expect: <message>`")
endif()

file(MAKE_DIRECTORY "${OUT}")
# EMULATOR runs purrc when it isn't a native program (Node, for web builds).
execute_process(
    COMMAND ${EMULATOR} "${PURRC}" ${files} -o "${OUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)

if(result EQUAL 0)
    message(FATAL_ERROR "expected purrc to fail with: ${expected}")
endif()

string(FIND "${stderr}" "${expected}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "expected an error containing: ${expected}\npurrc said:\n${stderr}")
endif()
