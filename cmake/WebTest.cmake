# tide_add_web_test(<name> TARGET <target> [QUERY <query>])
#
# Web builds: adds a test that opens <target>'s page in headless Chrome or Edge
# and passes if the page exits with 0. WebGL runs on the browser's software
# renderer, so no GPU is needed. The page's shell must write stdout into
# <pre id="log"> and the exit code into <body data-exit>, and set
# Tide.checkGL so GL errors fail it, as platform/web/test_shell.html does. The
# test is skipped if no browser is found.

# For web builds' tests, and desktop rooms against a browser (platform/tests/rooms.mjs).
find_program(TIDE_BROWSER
    NAMES chrome google-chrome chromium chromium-browser msedge "Google Chrome"
    PATHS
        "C:/Program Files/Google/Chrome/Application"
        "C:/Program Files (x86)/Microsoft/Edge/Application"
        "/Applications/Google Chrome.app/Contents/MacOS"
    DOC "Chrome or Edge, for web tests")
find_program(TIDE_FIREFOX
    NAMES firefox
    PATHS "C:/Program Files/Mozilla Firefox" "/Applications/Firefox.app/Contents/MacOS"
    DOC "Firefox, for desktop rooms against a second browser")

function(tide_add_web_test name)
    cmake_parse_arguments(ARG "" "TARGET;QUERY" "" ${ARGN})
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DNODE=${TIDE_NODE}"
            "-DBROWSER=${TIDE_BROWSER}"
            "-DPAGE=$<TARGET_FILE_DIR:${ARG_TARGET}>/${ARG_TARGET}.html"
            "-DQUERY=${ARG_QUERY}"
            "-DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/${name}-browser"
            -P "${PROJECT_SOURCE_DIR}/cmake/run_web_test.cmake")
    set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
endfunction()
