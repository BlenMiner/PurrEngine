# Runs tidec for tide_add_game (see Tide.cmake), where its warnings are errors:
# the build fails on any warning but those the game means to have, listed one
# per line in EXPECTED, and on one of those that's missing. Those aren't shown.
#
# cmake -DTIDEC=<tidec> -DEXPECTED=<file> -P run_tidec.cmake -- <tidec's arguments>...

set(args "")
set(after FALSE)
math(EXPR last "${CMAKE_ARGC} - 1")
foreach(i RANGE ${last})
    if(after)
        list(APPEND args "${CMAKE_ARGV${i}}")
    elseif(CMAKE_ARGV${i} STREQUAL "--")
        set(after TRUE)
    endif()
endforeach()

execute_process(
    COMMAND "${TIDEC}" ${args}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE stdout
    ERROR_VARIABLE stderr)
# Its own words, where editors can read where each message points.
string(STRIP "${stdout}${stderr}" said)
if(NOT result EQUAL 0)
    message(NOTICE "${said}")
    message(FATAL_ERROR "tidec failed")
endif()

# Lines, with any ';' in them kept from splitting them further.
string(ASCII 31 semicolon)
function(lines_of text out)
    string(REPLACE "\r" "" text "${text}")
    string(REPLACE ";" "${semicolon}" text "${text}")
    string(REPLACE "\n" ";" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

lines_of("${stderr}" output)
set(warnings "")
foreach(line IN LISTS output)
    if(line MATCHES ": warning: (.*)$")
        list(APPEND warnings "${CMAKE_MATCH_1}")
    endif()
endforeach()
set(expected "")
if(EXISTS "${EXPECTED}")
    file(READ "${EXPECTED}" content)
    lines_of("${content}" expected)
    list(REMOVE_ITEM expected "")
endif()

set(problems "")
foreach(warning IN LISTS warnings)
    set(known FALSE)
    foreach(text IN LISTS expected)
        string(FIND "${warning}" "${text}" at)
        if(NOT at EQUAL -1)
            set(known TRUE)
        endif()
    endforeach()
    if(NOT known)
        string(REPLACE "${semicolon}" ";" warning "${warning}")
        string(APPEND problems "\nwarning: ${warning}")
    endif()
endforeach()
foreach(text IN LISTS expected)
    set(found FALSE)
    foreach(warning IN LISTS warnings)
        string(FIND "${warning}" "${text}" at)
        if(NOT at EQUAL -1)
            set(found TRUE)
        endif()
    endforeach()
    if(NOT found)
        string(REPLACE "${semicolon}" ";" text "${text}")
        string(APPEND problems "\nexpected, but tidec didn't say it: ${text}")
    endif()
endforeach()

if(problems)
    message(NOTICE "${said}")
    message(FATAL_ERROR "Warnings are errors in this build: fix them, or list the ones the game means to have "
                        "after WARNINGS in its tide_add_game.${problems}")
endif()
