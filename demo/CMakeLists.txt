# A small game on the platform layer: `demo` on desktop, `demo.html` on the web.
# It has its own host (main.c) for the smoke test.
tide_add_game(demo HOST main.c WINDOW)
target_link_libraries(demo PRIVATE tide_platform)
tide_web_page(demo SHELL shell.html)

if(TIDE_WEB)
    tide_add_web_test(demo_smoke_web TARGET demo QUERY smoke)
elseif(NOT TIDE_ANDROID)
    # Opens a hidden window, so it needs a desktop session with working OpenGL.
    add_test(NAME demo_smoke COMMAND demo --smoke)
    set_tests_properties(demo_smoke PROPERTIES LABELS gpu)
endif()
