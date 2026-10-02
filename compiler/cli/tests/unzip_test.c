// tide's own unzip (unzip.c), which unpacks Google's zips: DEFLATE's three
// kinds of block, damage found, and an archive's entries picked and placed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sys.h"
#include "tide_test.h"
#include "unzip.h"
#include "unzip_vectors.h"

uint32_t rtc_crc32(const void *data, size_t size);

TIDE_TEST(unzip_inflates_every_kind_of_block)
{
    char out[64];
    const char hello[] = "hello, hello, hello world";
    TIDE_CHECK(unzip_inflate(fixed_stream, sizeof fixed_stream, (uint8_t *)out, sizeof hello - 1));
    TIDE_CHECK(memcmp(out, hello, sizeof hello - 1) == 0);
    const char stored[] = "stored as it is";
    TIDE_CHECK(unzip_inflate(stored_stream, sizeof stored_stream, (uint8_t *)out, sizeof stored - 1));
    TIDE_CHECK(memcmp(out, stored, sizeof stored - 1) == 0);

    uint8_t *text = malloc(6215);
    TIDE_REQUIRE(text);
    TIDE_CHECK(unzip_inflate(dynamic_stream, sizeof dynamic_stream, text, 6215));
    TIDE_CHECK(rtc_crc32(text, 6215) == 29955095u); // What Python's zlib.crc32 said of what it packed

    // Wrong sizes and damage are refused, never read past
    TIDE_CHECK(!unzip_inflate(dynamic_stream, sizeof dynamic_stream, text, 6214));
    TIDE_CHECK(!unzip_inflate(dynamic_stream, sizeof dynamic_stream / 2, text, 6215));
    uint8_t broken[sizeof dynamic_stream];
    memcpy(broken, dynamic_stream, sizeof broken);
    broken[0] |= 6; // A block of the kind that doesn't exist
    TIDE_CHECK(!unzip_inflate(broken, sizeof broken, text, 6215));
    free(text);
}

// Takes pkg/'s files, without the folder.
static bool in_pkg(void *user, const char *name, char *to, const size_t size)
{
    (void)user;
    if (strncmp(name, "pkg/", 4) != 0) return false;
    snprintf(to, size, "%s", name + 4);
    return true;
}

TIDE_TEST(unzip_extracts_the_entries_asked_for)
{
    const char *archive = "unzip_test.zip";
    FILE *f = fopen(archive, "wb");
    TIDE_REQUIRE(f && fwrite(zip_archive, 1, sizeof zip_archive, f) == sizeof zip_archive);
    fclose(f);
    sys_remove_tree("unzip_test_out");
    TIDE_REQUIRE(unzip(archive, "unzip_test_out", in_pkg, NULL));

    size_t size = 0;
    char *a = sys_read_file("unzip_test_out/a.txt", &size); // Deflated
    TIDE_CHECK(a && size == 600 && memcmp(a, "apple apple ", 12) == 0);
    char *b = sys_read_file("unzip_test_out/sub/b.txt", &size); // Stored, in a folder
    TIDE_CHECK(b && size == 6 && memcmp(b, "banana", 6) == 0);
    TIDE_CHECK(!sys_exists("unzip_test_out/c.txt") && !sys_exists("unzip_test_out/other"));
    free(a);
    free(b);

    // A damaged entry: its data, after the headers
    uint8_t damaged[sizeof zip_archive];
    memcpy(damaged, zip_archive, sizeof damaged);
    damaged[30 + 9 + 2] ^= 0x55;
    f = fopen(archive, "wb");
    TIDE_REQUIRE(f && fwrite(damaged, 1, sizeof damaged, f) == sizeof damaged);
    fclose(f);
    TIDE_CHECK(!unzip(archive, "unzip_test_out", in_pkg, NULL));
    sys_remove_tree("unzip_test_out");
    sys_remove(archive);
}
