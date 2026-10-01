#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide/time.h"
#include "tide_test.h"

#define TIDE_MAX_TESTS 4096

static const tide_test_case *tests[TIDE_MAX_TESTS];
static int test_count;
static int current_failures;

void tide_test_register(const tide_test_case *tc)
{
    if (test_count == TIDE_MAX_TESTS) {
        fprintf(stderr, "too many tests, raise TIDE_MAX_TESTS\n");
        exit(1);
    }
    tests[test_count++] = tc;
}

void tide_test_fail(const char *file, const int line, const char *expr)
{
    printf("    %s:%d: check failed: %s\n", file, line, expr);
    current_failures++;
}

static int compare_tests(const void *a, const void *b)
{
    const tide_test_case *x = *(const tide_test_case *const *)a;
    const tide_test_case *y = *(const tide_test_case *const *)b;
    const int by_file = strcmp(x->file, y->file);
    return by_file != 0 ? by_file : strcmp(x->name, y->name);
}

int main(const int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;

    // Registration order depends on link order; sort so output is stable.
    qsort(tests, (size_t)test_count, sizeof tests[0], compare_tests);

    int ran = 0;
    int failed = 0;
    for (int i = 0; i < test_count; i++) {
        const tide_test_case *tc = tests[i];
        if (filter && !strstr(tc->name, filter)) continue;

        current_failures = 0;
        const uint64_t start = tide_time_now_ns();
        tc->fn();
        const double ms = (double)(tide_time_now_ns() - start) / 1e6;

        printf("%s %s (%.3f ms)\n", current_failures ? "FAIL" : "ok  ", tc->name, ms);
        ran++;
        if (current_failures) failed++;
    }

    printf("\n%d/%d passed\n", ran - failed, ran);
    return failed ? 1 : 0;
}
