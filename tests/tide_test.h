#pragma once

// Minimal test harness. Define a test in any .c file in tests/:
//
//     #include "tide_test.h"
//
//     TIDE_TEST(entity_create)
//     {
//         TIDE_CHECK(1 + 1 == 2);
//     }
//
// Tests register themselves at startup, so there's no list to maintain.
//     tide_tests          runs every test
//     tide_tests entity   runs tests whose name contains "entity"

typedef void (*tide_test_fn)(void);

typedef struct tide_test_case {
    const char *name;
    const char *file;
    tide_test_fn fn;
} tide_test_case;

void tide_test_register(const tide_test_case *tc);
void tide_test_fail(const char *file, int line, const char *expr);

#define TIDE_TEST(name)                                                        \
    static void tide_test_fn__##name(void);                                    \
    static const tide_test_case tide_test_case__##name = {                     \
        #name, __FILE__, tide_test_fn__##name};                                \
    __attribute__((constructor)) static void tide_test_reg__##name(void)       \
    {                                                                          \
        tide_test_register(&tide_test_case__##name);                           \
    }                                                                          \
    static void tide_test_fn__##name(void)

// Records a failure and keeps going.
#define TIDE_CHECK(expr)                                                       \
    do {                                                                       \
        if (!(expr)) tide_test_fail(__FILE__, __LINE__, #expr);                \
    } while (0)

// Records a failure and ends the test.
#define TIDE_REQUIRE(expr)                                                     \
    do {                                                                       \
        if (!(expr)) {                                                         \
            tide_test_fail(__FILE__, __LINE__, #expr);                         \
            return;                                                            \
        }                                                                      \
    } while (0)
