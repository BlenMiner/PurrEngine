# A small game on the platform layer: `demo` on desktop, `demo.html` on the web.
# It has its own host (main.c) for the smoke test.
purr_add_game(demo HOST main.c)
target_link_libraries(demo PRIVATE purr_platform)
purr_web_page(demo SHELL shell.html)

if(EMSCRIPTEN)
    purr_add_web_test(demo_smoke_web TARGET demo QUERY smoke)
else()
    # Opens a hidden window, so it needs a desktop session with working OpenGL.
    add_test(NAME demo_smoke COMMAND demo --smoke)
endif()
