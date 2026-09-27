#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purr/time.h"
#include "purr_test.h"

#define PURR_MAX_TESTS 4096

static const purr_test_case *tests[PURR_MAX_TESTS];
static int test_count;
static int current_failures;

void purr_test_register(const purr_test_case *tc)
{
    if (test_count == PURR_MAX_TESTS) {
        fprintf(stderr, "too many tests, raise PURR_MAX_TESTS\n");
        exit(1);
    }
    tests[test_count++] = tc;
}

void purr_test_fail(const char *file, int line, const char *expr)
{
    printf("    %s:%d: check failed: %s\n", file, line, expr);
    current_failures++;
}

static int compare_tests(const void *a, const void *b)
{
    const purr_test_case *x = *(const purr_test_case *const *)a;
    const purr_test_case *y = *(const purr_test_case *const *)b;
    int by_file = strcmp(x->file, y->file);
    return by_file != 0 ? by_file : strcmp(x->name, y->name);
}

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;

    // Registration order depends on link order; sort so output is stable.
    qsort(tests, (size_t)test_count, sizeof tests[0], compare_tests);

    int ran = 0;
    int failed = 0;
    for (int i = 0; i < test_count; i++) {
        const purr_test_case *tc = tests[i];
        if (filter && !strstr(tc->name, filter)) continue;

        current_failures = 0;
        uint64_t start = purr_time_now_ns();
        tc->fn();
        double ms = (double)(purr_time_now_ns() - start) / 1e6;

        printf("%s %s (%.3f ms)\n", current_failures ? "FAIL" : "ok  ", tc->name, ms);
        ran++;
        if (current_failures) failed++;
    }

    printf("\n%d/%d passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
