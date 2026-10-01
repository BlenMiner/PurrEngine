# Runs `tidec --schedule` on SOURCE and checks that it prints EXPECTED exactly.
execute_process(
    COMMAND "${TIDEC}" "${SOURCE}" --schedule
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "tidec --schedule failed:\n${stderr}")
endif()
file(READ "${EXPECTED}" expected)
string(REPLACE "\r\n" "\n" expected "${expected}")
string(REPLACE "\r\n" "\n" stdout "${stdout}")
if(NOT stdout STREQUAL expected)
    message(FATAL_ERROR "the schedule changed. Expected (${EXPECTED}):\n${expected}\nGot:\n${stdout}")
endif()
