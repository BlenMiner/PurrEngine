# Opens a web build's page in a headless browser and fails unless the page exits
# with 0. See WebTest.cmake.
#
# cmake -DBROWSER=<Chrome or Edge> -DPAGE=<page.html> [-DQUERY=<query>] -DPROFILE=<scratch dir> -P run_web_test.cmake

if(NOT EXISTS "${BROWSER}")
    message("SKIPPED: no Chrome or Edge found. Set PURR_BROWSER to run web tests.")
    return()
endif()

if(PAGE MATCHES "^/")
    set(url "file://${PAGE}")
else()
    set(url "file:///${PAGE}")
endif()
if(QUERY)
    string(APPEND url "?${QUERY}")
endif()

file(REMOVE_RECURSE "${PROFILE}")
execute_process(
    COMMAND "${BROWSER}"
        --headless
        --no-first-run
        --no-default-browser-check
        --disable-extensions
        "--user-data-dir=${PROFILE}"
        --use-angle=swiftshader
        --enable-unsafe-swiftshader
        --virtual-time-budget=60000
        --dump-dom "${url}"
    OUTPUT_VARIABLE dom
    ERROR_VARIABLE browser_errors
    TIMEOUT 120)

string(REGEX MATCH "<pre id=\"log\">([^<]*)</pre>" _ "${dom}")
message("${CMAKE_MATCH_1}")

string(REGEX MATCH "data-exit=\"([^\"]*)\"" _ "${dom}")
if(NOT CMAKE_MATCH_1 STREQUAL "0")
    message(FATAL_ERROR "The page didn't exit with 0 (got '${CMAKE_MATCH_1}').\n${browser_errors}")
endif()
