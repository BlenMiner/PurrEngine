#include <stdio.h>
#include <stdlib.h>

#include "game.h"

// Main's Spawned handler starts the chain as the world starts: the program
// stops there, saying why (see compiler/tests/CMakeLists.txt).
int main(void)
{
#ifdef _WIN32
    // Stopping without the debug C runtime's dialog, which would wait for a click
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    static tide_world world;
    tide_world_init(&world, 1.0f);
    printf("the chain ended\n");
    return 1;
}
