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
    TIDE_CHECK(list.count == 0 && list.text_used == 0 && list.dropped == 0);
}

TIDE_TEST(draw_full_list_drops_commands)
{
    tide_draw_reset(&list);
    for (uint32_t i = 0; i < TIDE_DRAW_MAX_COMMANDS + 3; i++) {
        tide_draw_line(&list, tide_f2(0.0f, 0.0f), tide_f2(1.0f, 1.0f), TIDE_COLOR_WHITE);
    }
    TIDE_CHECK(list.count == TIDE_DRAW_MAX_COMMANDS);
    TIDE_CHECK(list.dropped == 3);
}

TIDE_TEST(draw_full_text_drops_commands)
{
    static char big[TIDE_DRAW_TEXT_BYTES / 2 + 1];
    memset(big, 'a', sizeof big - 1);
    tide_draw_reset(&list);
    tide_draw_text(&list, big, tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    tide_draw_text(&list, big, tide_f2(0.0f, 0.0f), 1.0f, TIDE_COLOR_WHITE);
    TIDE_CHECK(list.count == 1);
    TIDE_CHECK(list.dropped == 1);
}
