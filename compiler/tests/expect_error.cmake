# Runs purrc on SOURCE and checks that it fails with the message written on the
# file's first line as `// expect: <text>`.
# Read as one string: file(STRINGS) would split the line on ';'. The leading
# [^/]* skips a byte-order mark.
file(READ "${SOURCE}" content)
if(NOT content MATCHES "^[^/]*// expect: ([^\r\n]+)")
    message(FATAL_ERROR "${SOURCE}: first line must be `// expect: <message>`")
endif()
set(expected "${CMAKE_MATCH_1}")

file(MAKE_DIRECTORY "${OUT}")
execute_process(
    COMMAND "${PURRC}" "${SOURCE}" -o "${OUT}"
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
