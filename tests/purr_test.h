#pragma once

// Minimal test harness. Define a test in any .c file in tests/:
//
//     #include "purr_test.h"
//
//     PURR_TEST(entity_create)
//     {
//         PURR_CHECK(1 + 1 == 2);
//     }
//
// Tests register themselves at startup, so there's no list to maintain.
//     purr_tests          runs every test
//     purr_tests entity   runs tests whose name contains "entity"

typedef void (*purr_test_fn)(void);

typedef struct purr_test_case {
    const char *name;
    const char *file;
    purr_test_fn fn;
} purr_test_case;

void purr_test_register(const purr_test_case *tc);
void purr_test_fail(const char *file, int line, const char *expr);

#define PURR_TEST(name)                                                        \
    static void purr_test_fn__##name(void);                                    \
    static const purr_test_case purr_test_case__##name = {                     \
        #name, __FILE__, purr_test_fn__##name};                                \
    __attribute__((constructor)) static void purr_test_reg__##name(void)       \
    {                                                                          \
        purr_test_register(&purr_test_case__##name);                           \
    }                                                                          \
    static void purr_test_fn__##name(void)

// Records a failure and keeps going.
#define PURR_CHECK(expr)                                                       \
    do {                                                                       \
        if (!(expr)) purr_test_fail(__FILE__, __LINE__, #expr);                \
    } while (0)

// Records a failure and ends the test.
#define PURR_REQUIRE(expr)                                                     \
    do {                                                                       \
        if (!(expr)) {                                                         \
            purr_test_fail(__FILE__, __LINE__, #expr);                         \
            return;                                                            \
        }                                                                      \
    } while (0)
