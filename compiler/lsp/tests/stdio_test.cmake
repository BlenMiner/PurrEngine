# Runs purrls over stdin and stdout, the way an editor does, with framed
# messages, and checks its replies and its exit code.
#
# cmake -DPURRLS=<purrls> -DOUT=<scratch dir> -P stdio_test.cmake

# One variable per message: a CMake list would split them at semicolons.
set(message1 [[{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}]])
set(message2 [[{"jsonrpc":"2.0","method":"initialized","params":{}}]])
set(message3 [[{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///a.purr","languageId":"purrlang","version":1,"text":"component Body { float2 position; }\nsystem Main() { Spawn(Body); }\nsystem Move(mut Body body)\n{\n    body.\n}\n"}}}]])
set(message4 [[{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///a.purr"},"position":{"line":4,"character":9}}}]])
set(message5 [[{"jsonrpc":"2.0","id":3,"method":"shutdown"}]])
set(message6 [[{"jsonrpc":"2.0","method":"exit"}]])

set(input "")
foreach(name message1 message2 message3 message4 message5 message6)
    string(LENGTH "${${name}}" length)
    string(APPEND input "Content-Length: ${length}\n\n${${name}}")
endforeach()
# Headers end with \r\n on every platform. file(WRITE) would translate newlines
# on Windows only.
file(CONFIGURE OUTPUT "${OUT}/stdio_input.txt" CONTENT "${input}" @ONLY NEWLINE_STYLE CRLF)

execute_process(
    COMMAND "${PURRLS}"
    INPUT_FILE "${OUT}/stdio_input.txt"
    OUTPUT_VARIABLE output
    RESULT_VARIABLE result
    TIMEOUT 20)

if(NOT result EQUAL 0)
    message(FATAL_ERROR "purrls exited with ${result}\n${output}")
endif()
foreach(expected "\"capabilities\"" "publishDiagnostics" "\"label\":\"position\"" "\"id\":3,\"result\":null")
    string(FIND "${output}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "purrls didn't reply with ${expected}\n${output}")
    endif()
endforeach()
string(FIND "${output}" "invalid JSON" invalid)
if(NOT invalid EQUAL -1)
    message(FATAL_ERROR "purrls couldn't read a message\n${output}")
endif()
