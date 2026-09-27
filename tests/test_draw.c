#include <string.h>

#include "purr/draw.h"
#include "purr_test.h"

static purr_draw_list list;

PURR_TEST(draw_records_in_order)
{
    purr_draw_reset(&list);
    purr_draw_clear(&list, PURR_COLOR_BLACK);
    purr_draw_circle(&list, purr_f2(1.0f, 2.0f), 3.0f, PURR_COLOR_RED);
    purr_draw_text(&list, "one", purr_f2(0.0f, 0.0f), 1.0f, PURR_COLOR_WHITE);
    purr_draw_text(&list, "two", purr_f2(0.0f, 0.0f), 1.0f, PURR_COLOR_WHITE);
    PURR_REQUIRE(list.count == 4);
    PURR_CHECK(list.commands[0].kind == PURR_DRAW_CLEAR);
    PURR_CHECK(list.commands[1].kind == PURR_DRAW_CIRCLE && list.commands[1].b.x == 3.0f);
    PURR_CHECK(strcmp(list.text + list.commands[2].text, "one") == 0);
    PURR_CHECK(strcmp(list.text + list.commands[3].text, "two") == 0);

    purr_draw_reset(&list);
    PURR_CHECK(list.count == 0 && list.text_used == 0 && list.dropped == 0);
}

PURR_TEST(draw_full_list_drops_commands)
{
    purr_draw_reset(&list);
    for (uint32_t i = 0; i < PURR_DRAW_MAX_COMMANDS + 3; i++) {
        purr_draw_line(&list, purr_f2(0.0f, 0.0f), purr_f2(1.0f, 1.0f), PURR_COLOR_WHITE);
    }
    PURR_CHECK(list.count == PURR_DRAW_MAX_COMMANDS);
    PURR_CHECK(list.dropped == 3);
}

PURR_TEST(draw_full_text_drops_commands)
{
    static char big[PURR_DRAW_TEXT_BYTES / 2 + 1];
    memset(big, 'a', sizeof big - 1);
    purr_draw_reset(&list);
    purr_draw_text(&list, big, purr_f2(0.0f, 0.0f), 1.0f, PURR_COLOR_WHITE);
    purr_draw_text(&list, big, purr_f2(0.0f, 0.0f), 1.0f, PURR_COLOR_WHITE);
    PURR_CHECK(list.count == 1);
    PURR_CHECK(list.dropped == 1);
}
