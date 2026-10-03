# tide_add_web_test(<name> TARGET <target> [QUERY <query>] [WEBGPU [EXPECT <text>]])
#
# Web builds: adds a test that opens <target>'s page in headless Chrome or Edge
# and passes if the page exits with 0. It draws with WebGL 2 (backend=webgl in
# the page's address), on the browser's software renderer, so no GPU is
# needed, in virtual time. The page's shell must write stdout into
# <pre id="log"> and the exit code into <body data-exit>, and set
# Tide.checkGL so mistakes in drawing fail it, as platform/web/test_shell.html
# does. The test is skipped if no browser is found.
#
# With WEBGPU, it also adds <name>_webgpu, labelled webgpu: the same page
# drawing with WebGPU (backend=webgpu, which fails rather than falls back to
# WebGL), on the browser's software WebGPU. The browser only answers about its
# GPU in real time, so the page is served (cmake/run_web_served_test.mjs), and
# its shell must send its output and exit code back, as test_shell.html does.
# EXPECT is a line the program has to print.

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
    cmake_parse_arguments(ARG "WEBGPU" "TARGET;QUERY;EXPECT" "" ${ARGN})
    set(page "$<TARGET_FILE_DIR:${ARG_TARGET}>/${ARG_TARGET}.html")
    set(query "backend=webgl")
    set(webgpu_query "backend=webgpu")
    if(ARG_QUERY) # The program's own words first: they're its arguments, in order
        set(query "${ARG_QUERY}&${query}")
        set(webgpu_query "${ARG_QUERY}&${webgpu_query}")
    endif()
    add_test(NAME ${name}
        COMMAND "${CMAKE_COMMAND}"
            "-DNODE=${TIDE_NODE}"
            "-DBROWSER=${TIDE_BROWSER}"
            "-DPAGE=${page}"
            "-DQUERY=${query}"
            "-DPROFILE=${CMAKE_CURRENT_BINARY_DIR}/${name}-browser"
            -P "${PROJECT_SOURCE_DIR}/cmake/run_web_test.cmake")
    set_tests_properties(${name} PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:")
    if(ARG_WEBGPU)
        set(expect "")
        if(ARG_EXPECT)
            set(expect --expect "${ARG_EXPECT}")
        endif()
        add_test(NAME ${name}_webgpu
            COMMAND "${TIDE_NODE}" "${PROJECT_SOURCE_DIR}/cmake/run_web_served_test.mjs" "${TIDE_BROWSER}" "${page}"
                "${CMAKE_CURRENT_BINARY_DIR}/${name}_webgpu-browser" "${webgpu_query}" ${expect})
        set_tests_properties(${name}_webgpu PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED:" LABELS webgpu TIMEOUT 120)
    endif()
endfunction()
