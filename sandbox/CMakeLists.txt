add_executable(sandbox main.c)
target_link_libraries(sandbox PRIVATE purr)
purr_add_game(sandbox SOURCE game.purr)
