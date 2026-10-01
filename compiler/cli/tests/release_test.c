// Versions and which release tide upgrade picks, without the network.

#include <string.h>

#include "json.h"
#include "tide_test.h"
#include "release.h"

TIDE_TEST(cli_versions_compare_as_semantic_release_makes_them)
{
    TIDE_CHECK(tide_version_compare("0.2.0", "0.2.0") == 0);
    TIDE_CHECK(tide_version_compare("0.1.0", "0.2.0") < 0);
    TIDE_CHECK(tide_version_compare("0.10.0", "0.9.9") > 0);
    TIDE_CHECK(tide_version_compare("1.0.0", "0.99.99") > 0);
    // A nightly comes before its release, and after the release before it.
    TIDE_CHECK(tide_version_compare("0.2.0-nightly.6", "0.2.0") < 0);
    TIDE_CHECK(tide_version_compare("0.2.0-nightly.6", "0.1.0") > 0);
    TIDE_CHECK(tide_version_compare("0.2.0-nightly.5", "0.2.0-nightly.6") < 0);
    TIDE_CHECK(tide_version_compare("0.2.0-nightly.10", "0.2.0-nightly.9") > 0); // By number, not as text
    TIDE_CHECK(tide_version_compare("0.2.0-nightly.6", "0.2.0-nightly.6") == 0);
    // Semantic versioning's other rules.
    TIDE_CHECK(tide_version_compare("1.0.0-alpha", "1.0.0-alpha.1") < 0);
    TIDE_CHECK(tide_version_compare("1.0.0-alpha.1", "1.0.0-alpha.beta") < 0); // Numbers before words
    TIDE_CHECK(tide_version_compare("1.0.0-beta", "1.0.0-alpha") > 0);
    TIDE_CHECK(tide_version_compare("1.0.0+build.5", "1.0.0") == 0);
    // Anything else is older than every version.
    TIDE_CHECK(tide_version_compare("banana", "0.0.1") < 0);
    TIDE_CHECK(tide_version_compare("0.0.1", "1.2") > 0);
    TIDE_CHECK(tide_version_compare(NULL, "0.0.1") < 0);
    TIDE_CHECK(!tide_version_valid("1.2"));
    TIDE_CHECK(!tide_version_valid("1.2.3-"));
    TIDE_CHECK(!tide_version_valid("1.2.3 "));
    TIDE_CHECK(tide_version_valid("0.0.0-dev"));
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
    return tide_release_version(tide_release_pick(releases, latest, channel));
}

TIDE_TEST(cli_upgrades_pick_the_highest_version_not_the_newest_release)
{
    const json *releases = json_parse(RELEASES, strlen(RELEASES));
    TIDE_REQUIRE(releases != NULL);
    // A stable patch published after a newer nightly: nightly keeps the nightly.
    TIDE_CHECK(strcmp(picked(releases, NULL, "nightly"), "0.2.0-nightly.10") == 0);
    TIDE_CHECK(strcmp(picked(releases, NULL, "stable"), "0.1.1") == 0);
    // A stable release newer than every nightly is nightly's too.
    const char newer[] = "{\"tag_name\": \"v0.2.0\", \"prerelease\": false, \"draft\": false}";
    const json *latest = json_parse(newer, strlen(newer));
    TIDE_CHECK(strcmp(picked(releases, latest, "nightly"), "0.2.0") == 0);
    TIDE_CHECK(strcmp(picked(releases, latest, "stable"), "0.2.0") == 0);
    // Stable finds its latest release even when nightly ones pushed it out of the list.
    const char nightlies[] = "[{\"tag_name\": \"v0.3.0-nightly.1\", \"prerelease\": true, \"draft\": false}]";
    const json *only_nightlies = json_parse(nightlies, strlen(nightlies));
    TIDE_CHECK(picked(only_nightlies, NULL, "stable") == NULL);
    TIDE_CHECK(strcmp(picked(only_nightlies, latest, "stable"), "0.2.0") == 0);
    TIDE_CHECK(strcmp(picked(only_nightlies, latest, "nightly"), "0.3.0-nightly.1") == 0);
    json_release();
}

TIDE_TEST(cli_upgrades_find_an_exact_version)
{
    const json *releases = json_parse(RELEASES, strlen(RELEASES));
    TIDE_REQUIRE(releases != NULL);
    TIDE_CHECK(tide_release_find(releases, "0.2.0-nightly.9") == releases->items[3]);
    TIDE_CHECK(tide_release_find(releases, "v0.2.0-nightly.9") == releases->items[3]);
    TIDE_CHECK(tide_release_find(releases, "0.3.0-nightly.1") == NULL); // A draft
    TIDE_CHECK(tide_release_find(releases, "0.4.0") == NULL);
    json_release();
}
