#include "game.h"

static purr_world world;

int main(void)
{
    purr_world_init(&world, 1.0f);
    for (int i = 0; i < 6; i++) {
        purr_world_tick(&world);
        purr_world_print(&world);
    }
    return 0;
}
