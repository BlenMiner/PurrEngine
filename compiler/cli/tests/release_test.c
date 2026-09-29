// Versions and which release purr upgrade picks, without the network.

#include <string.h>

#include "json.h"
#include "purr_test.h"
#include "release.h"

PURR_TEST(cli_versions_compare_as_semantic_release_makes_them)
{
    PURR_CHECK(purr_version_compare("0.2.0", "0.2.0") == 0);
    PURR_CHECK(purr_version_compare("0.1.0", "0.2.0") < 0);
    PURR_CHECK(purr_version_compare("0.10.0", "0.9.9") > 0);
    PURR_CHECK(purr_version_compare("1.0.0", "0.99.99") > 0);
    // A nightly comes before its release, and after the release before it.
    PURR_CHECK(purr_version_compare("0.2.0-nightly.6", "0.2.0") < 0);
    PURR_CHECK(purr_version_compare("0.2.0-nightly.6", "0.1.0") > 0);
    PURR_CHECK(purr_version_compare("0.2.0-nightly.5", "0.2.0-nightly.6") < 0);
    PURR_CHECK(purr_version_compare("0.2.0-nightly.10", "0.2.0-nightly.9") > 0); // By number, not as text
    PURR_CHECK(purr_version_compare("0.2.0-nightly.6", "0.2.0-nightly.6") == 0);
    // Semantic versioning's other rules.
    PURR_CHECK(purr_version_compare("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    PURR_CHECK(purr_version_compare("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0); // Numbers before words
    PURR_CHECK(purr_version_compare("1.0.0-beta", "1.0.0-alpha") > 0);
    PURR_CHECK(purr_version_compare("1.0.0+build.5", "1.0.0") == 0);
    // Anything else is older than every version.
    PURR_CHECK(purr_version_compare("banana", "0.0.1") < 0);
    PURR_CHECK(purr_version_compare("0.0.1", "1.2") > 0);
    PURR_CHECK(purr_version_compare(NULL, "0.0.1") < 0);
    PURR_CHECK(!purr_version_valid("1.2"));
    PURR_CHECK(!purr_version_valid("1.2.3-"));
    PURR_CHECK(!purr_version_valid("1.2.3 "));
    PURR_CHECK(purr_version_valid("0.0.0-dev"));
}

// GitHub lists releases newest first, by when they were published.
static const char RELEASES[] = "["
                               "{\"tag_name\": \"v0.1.1\", \"prerelease\": false, \"draft\": false},"
                               "{\"tag_name\": \"v0.3.0-nightly.1\", \"prerelease\": true, \"draft\": true},"
                               "{\"tag_name\": \"v0.2.0-nightly.10\", \"prerelease\": true, \"draft\": false},"
                               "{\"tag_name\": \"v0.2.0-nightly.9\", \"prerelease\": true, \"draft\": false},"
                               "{\"tag_name\": \"not-a-version\", \"prerelease\": false, \"draft\": false},"
                               "{\"tag_name\": \"v0.1.0\", \"prerelease\": false, \"draft\": false}"
                               "]";

static const char *picked(const json *releases, const json *latest, const char *channel)
{
    return purr_release_version(purr_release_pick(releases, latest, channel));
}

PURR_TEST(cli_upgrades_pick_the_highest_version_not_the_newest_release)
{
    const json *releases = json_parse(RELEASES, strlen(RELEASES));
    PURR_REQUIRE(releases != NULL);
    // A stable patch published after a newer nightly: nightly keeps the nightly.
    PURR_CHECK(strcmp(picked(releases, NULL, "nightly"), "0.2.0-nightly.10") == 0);
    PURR_CHECK(strcmp(picked(releases, NULL, "stable"), "0.1.1") == 0);
    // A stable release newer than every nightly is nightly's too.
    const char newer[] = "{\"tag_name\": \"v0.2.0\", \"prerelease\": false, \"draft\": false}";
    const json *latest = json_parse(newer, strlen(newer));
    PURR_CHECK(strcmp(picked(releases, latest, "nightly"), "0.2.0") == 0);
    PURR_CHECK(strcmp(picked(releases, latest, "stable"), "0.2.0") == 0);
    // Stable finds its latest release even when nightly ones pushed it out of the list.
    const char nightlies[] = "[{\"tag_name\": \"v0.3.0-nightly.1\", \"prerelease\": true, \"draft\": false}]";
    const json *only_nightlies = json_parse(nightlies, strlen(nightlies));
    PURR_CHECK(picked(only_nightlies, NULL, "stable") == NULL);
    PURR_CHECK(strcmp(picked(only_nightlies, latest, "stable"), "0.2.0") == 0);
    PURR_CHECK(strcmp(picked(only_nightlies, latest, "nightly"), "0.3.0-nightly.1") == 0);
    json_release();
}

PURR_TEST(cli_upgrades_find_an_exact_version)
{
    const json *releases = json_parse(RELEASES, strlen(RELEASES));
    PURR_REQUIRE(releases != NULL);
    PURR_CHECK(purr_release_find(releases, "0.2.0-nightly.9") == releases->items[3]);
    PURR_CHECK(purr_release_find(releases, "v0.2.0-nightly.9") == releases->items[3]);
    PURR_CHECK(purr_release_find(releases, "0.3.0-nightly.1") == NULL); // A draft
    PURR_CHECK(purr_release_find(releases, "0.4.0") == NULL);
    json_release();
}
