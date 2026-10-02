#include <string.h>

#include "tide/draw.h"
#include "tide_test.h"

static tide_draw_list list;

TIDE_TEST(draw_records_in_order)
{
    tide_draw_reset(&list);
    tide_draw_clear(&list, TIDE_COLOR_BLACK);
    tide_draw_circle(&list, tide_f2(1.0f, 2.0f), 3.0f, TIDE_COLOR_RED);
    tide_draw_text(&list, "one", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_text(&list, "two", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    TIDE_REQUIRE(list.count == 4);
    TIDE_CHECK(list.commands[0].kind == TIDE_DRAW_CLEAR);
    TIDE_CHECK(list.commands[1].kind == TIDE_DRAW_CIRCLE && list.commands[1].b.x == 3.0f);
    TIDE_CHECK(strcmp(list.text + list.commands[2].text, "one") == 0);
    TIDE_CHECK(strcmp(list.text + list.commands[3].text, "two") == 0);

    tide_draw_reset(&list);
    TIDE_CHECK(list.count == 0 && list.text_used == 0);
}

// It grows as it needs: nothing is dropped, however much a frame draws.
TIDE_TEST(draw_list_grows)
{
    tide_draw_list grown = {0};
    for (uint32_t i = 0; i < 100000u; i++) {
        tide_draw_rect(&grown, tide_f2((float)i, 0.0f), tide_f2(1.0f, 1.0f), TIDE_COLOR_WHITE);
    }
    static char big[100000];
    memset(big, 'a', sizeof big - 1);
    tide_draw_text(&grown, big, tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_text(&grown, "end", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    TIDE_REQUIRE(grown.count == 100002u);
    TIDE_CHECK(grown.commands[99999].a.x == 99999.0f);
    TIDE_CHECK(strlen(grown.text + grown.commands[100000].text) == sizeof big - 1);
    TIDE_CHECK(strcmp(grown.text + grown.commands[100001].text, "end") == 0);

    // Appended, as the GUI's are over the world's, text and all
    tide_draw_list world = {0};
    tide_draw_text(&world, "world", tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_append(&world, &grown);
    TIDE_REQUIRE(world.count == 100003u);
    TIDE_CHECK(strcmp(world.text + world.commands[0].text, "world") == 0);
    TIDE_CHECK(strcmp(world.text + world.commands[100002].text, "end") == 0);
    tide_draw_free(&world);
    tide_draw_free(&grown);
    TIDE_CHECK(grown.count == 0 && grown.commands == NULL);
}
