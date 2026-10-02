#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "tide_test.h"

// Deltas (tide/delta.h): a world as what changed from a base, page by page.
// Whatever the base, a delta unpacked is the world it was made from, exactly,
// and a broken one or the wrong base is turned down.

static tide_world world;
static tide_world base;
static tide_world other;
static tide_world made;

// Whether two worlds hold the same: the same bytes (tide_world_pack, which
// leaves out where their pages are), and the same hash.
static bool same(const tide_world *a, const tide_world *b)
{
    const uint32_t size = tide_world_pack(a, NULL, 0);
    if (tide_world_pack(b, NULL, 0) != size) return false;
    uint8_t *x = malloc(size);
    uint8_t *y = malloc(size);
    tide_world_pack(a, x, size);
    tide_world_pack(b, y, size);
    const bool equal = memcmp(x, y, size) == 0;
    free(x);
    free(y);
    return equal && tide_world_hash(a) == tide_world_hash(b);
}

static void ticks(tide_world *w, const int n)
{
    for (int i = 0; i < n; i++) tide_world_tick(w);
}

static void start(void)
{
    tide_world_init(&world, 1.0f / 60.0f);
    tide_world_player_joined(&world, tide_player_from_index(0));
}

static void finish(void)
{
    tide_world_free(&world);
    tide_world_free(&base);
    tide_world_free(&other);
    tide_world_free(&made);
}

static uint32_t whole_size(const tide_world *w)
{
    uint32_t size;
    free(tide_world_pack_delta(w, NULL, NULL, 0, &size));
    return size;
}

// The Main scene is slot 0, the ground the next 3000, then the movers.
// Moves mover `i` up by `dy`, as a world's own code wouldn't.
static void nudge(tide_world *w, const uint32_t i, const float dy)
{
    const tide_entity e = {3001u + i, 1};
    Mover m = tide_get_Mover(w, e);
    m.position.y += dy;
    tide_set_Mover(w, e, m);
}

TIDE_TEST(delta_from_nothing_is_the_whole_world)
{
    start();
    ticks(&world, 5);
    uint32_t size;
    uint8_t *delta = tide_world_pack_delta(&world, NULL, NULL, 0, &size);
    TIDE_CHECK(tide_world_unpack_delta(&made, NULL, delta, size));
    TIDE_CHECK(same(&made, &world));
    TIDE_CHECK(size < tide_world_pack(&world, NULL, 0)); // Runs of zeros pack small
    // ...and goes on the same
    ticks(&world, 30);
    ticks(&made, 30);
    TIDE_CHECK(same(&made, &world));
    free(delta);
    finish();
}

TIDE_TEST(delta_from_a_base_has_what_changed)
{
    start();
    ticks(&world, 5);
    tide_world_copy(&base, &world);
    ticks(&world, 1);
    uint32_t size;
    uint8_t *delta = tide_world_pack_delta(&world, &base, NULL, 0, &size);
    TIDE_CHECK(size * 50u < whole_size(&world)); // A tick changes little of it
    TIDE_CHECK(tide_world_unpack_delta(&made, &base, delta, size));
    TIDE_CHECK(same(&made, &world));
    // What didn't change, it shares with the base: the first ground entities' slots
    TIDE_CHECK(made.entities.page[0] == base.entities.page[0]);
    free(delta);
    finish();
}

TIDE_TEST(delta_from_a_base_far_behind)
{
    start();
    tide_world_copy(&base, &world);
    ticks(&world, 300); // Sparks came and went, movers moved
    tide_world_player_joined(&world, tide_player_from_index(1));
    tide_world_player_left(&world, tide_player_from_index(0));
    ticks(&world, 7);
    uint32_t size;
    uint8_t *delta = tide_world_pack_delta(&world, &base, NULL, 0, &size);
    TIDE_CHECK(size * 10u < whole_size(&world));
    TIDE_CHECK(tide_world_unpack_delta(&made, &base, delta, size));
    TIDE_CHECK(same(&made, &world));
    ticks(&world, 20);
    ticks(&made, 20);
    TIDE_CHECK(same(&made, &world));
    TIDE_CHECK(made.Field.pulses == world.Field.pulses && made.Field.pulses > 40); // Its task went on with it
    free(delta);
    finish();
}

TIDE_TEST(delta_of_what_a_base_lacks)
{
    start();
    ticks(&world, 5);
    tide_world_copy(&base, &world);
    ticks(&world, 40);
    uint32_t hashes_size;
    uint8_t *hashes = tide_world_hash_pages(&world, &hashes_size);
    uint8_t lacks[512];
    const uint32_t lacks_size = tide_world_need_pages(&base, hashes, hashes_size, lacks, sizeof lacks);
    uint32_t size;
    uint8_t *delta = tide_world_pack_delta(&world, NULL, lacks, lacks_size, &size);
    TIDE_CHECK(size * 10u < whole_size(&world));
    TIDE_CHECK(tide_world_unpack_delta(&made, &base, delta, size));
    TIDE_CHECK(same(&made, &world));
    free(delta);

    // Saying less than it lacks, when the list doesn't fit, it gets more
    const uint32_t cut = tide_world_need_pages(&base, hashes, hashes_size, lacks, 2);
    TIDE_CHECK(cut <= 2u);
    uint32_t more;
    delta = tide_world_pack_delta(&world, NULL, lacks, cut, &more);
    TIDE_CHECK(more > size);
    TIDE_CHECK(tide_world_unpack_delta(&made, &base, delta, more));
    TIDE_CHECK(same(&made, &world));
    free(delta);

    // A base that's the world lacks nothing
    tide_world_copy(&other, &world);
    const uint32_t none = tide_world_need_pages(&other, hashes, hashes_size, lacks, sizeof lacks);
    uint32_t least;
    delta = tide_world_pack_delta(&world, NULL, lacks, none, &least);
    TIDE_CHECK(least < 300u);
    TIDE_CHECK(tide_world_unpack_delta(&made, &other, delta, least));
    TIDE_CHECK(same(&made, &world));
    free(delta);
    free(hashes);
    finish();
}

TIDE_TEST(delta_needs_the_base_it_was_made_from)
{
    start();
    ticks(&world, 5);
    tide_world_copy(&base, &world);
    ticks(&world, 25);
    tide_world_copy(&other, &world);
    ticks(&world, 30);
    uint32_t size;
    uint8_t *delta = tide_world_pack_delta(&world, &base, NULL, 0, &size);
    TIDE_CHECK(!tide_world_unpack_delta(&made, &other, delta, size));
    TIDE_CHECK(tide_world_entity_count(&made) == 0); // Left zeroed
    TIDE_CHECK(!tide_world_unpack_delta(&made, NULL, delta, size));
    free(delta);

    // What one base lacks, built on another, where it has something else
    uint32_t hashes_size;
    uint8_t *hashes = tide_world_hash_pages(&world, &hashes_size);
    uint8_t lacks[512];
    const uint32_t lacks_size = tide_world_need_pages(&base, hashes, hashes_size, lacks, sizeof lacks);
    delta = tide_world_pack_delta(&world, NULL, lacks, lacks_size, &size);
    Ground ground = tide_get_Ground(&other, (tide_entity){1, 1});
    ground.height += 1.0f;
    tide_set_Ground(&other, (tide_entity){1, 1}, ground);
    TIDE_CHECK(!tide_world_unpack_delta(&made, &other, delta, size));
    TIDE_CHECK(tide_world_unpack_delta(&made, &base, delta, size));
    TIDE_CHECK(same(&made, &world));
    free(delta);
    free(hashes);
    finish();
}

// splitmix64
static uint64_t next_random(uint64_t *state)
{
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Bytes from another machine: cut short or changed, a delta is turned down,
// or makes the world it was made from, and nothing breaks.
static void break_bytes(const uint8_t *delta, const uint32_t size, const tide_world *from, const int rounds,
                        uint64_t seed)
{
    for (uint32_t cut = 0; cut < size; cut += 1u + cut / 8u) TIDE_CHECK(!tide_world_unpack_delta(&made, from, delta, cut));
    uint8_t *broken = malloc(size);
    for (int i = 0; i < rounds; i++) {
        memcpy(broken, delta, size);
        const uint64_t r = next_random(&seed);
        broken[r % size] ^= (uint8_t)(1u + (r >> 32) % 255u);
        if (tide_world_unpack_delta(&made, from, broken, size)) TIDE_CHECK(same(&made, &world));
    }
    free(broken);
}

TIDE_TEST(delta_turns_down_broken_bytes)
{
    start();
    ticks(&world, 5);
    tide_world_copy(&base, &world);
    ticks(&world, 20);
    uint32_t size;
    uint8_t *delta = tide_world_pack_delta(&world, &base, NULL, 0, &size);
    break_bytes(delta, size, &base, 3000, 1);
    free(delta);
    delta = tide_world_pack_delta(&world, NULL, NULL, 0, &size);
    break_bytes(delta, size, NULL, 200, 2);
    free(delta);

    // Broken hashes: what it says it lacks may be wrong, and the world it
    // builds then is turned down
    uint32_t hashes_size;
    uint8_t *hashes = tide_world_hash_pages(&world, &hashes_size);
    uint64_t seed = 3;
    for (int i = 0; i < 1000; i++) {
        const uint64_t r = next_random(&seed);
        const uint32_t at = (uint32_t)(r % hashes_size);
        const uint8_t was = hashes[at];
        hashes[at] ^= (uint8_t)(1u + (r >> 32) % 255u);
        uint8_t lacks[512];
        const uint32_t n = tide_world_need_pages(&base, hashes, (uint32_t)(r >> 40) % 4u ? hashes_size : at, lacks, sizeof lacks);
        hashes[at] = was;
        delta = tide_world_pack_delta(&world, NULL, lacks, n, &size);
        if (tide_world_unpack_delta(&made, &base, delta, size)) TIDE_CHECK(same(&made, &world));
        free(delta);
    }
    free(hashes);
    finish();
}

// ---------------------------------------------------------------------------
// Sessions send worlds as deltas. A player gets what its world lacks: its own,
// when it went wrong; its last one, coming back to a match that changed
// hands; or, joining, the match as it started, which it starts itself.

static tide_loopback *net;
static tide_server *server;
static tide_client *client;
static double now;

static void sample(void *user, const uint32_t tick, void *input)
{
    (void)user;
    *(Controls *)input = (Controls){.push = tide_f2(tick % 50u == 0 ? 1.0f : 0.0f, 0.0f)};
}

static void serve(const tide_start *start, const tide_net_conditions conditions)
{
    now = 0.0;
    net = tide_loopback_create(5);
    tide_loopback_set_conditions(net, conditions);
    const tide_server_desc desc = {.game = &tide_game_api, .tick_rate = 60, .start = start,
                                   .transports = {tide_loopback_endpoint(net, 1)}};
    server = tide_server_create(&desc, now);
    client = NULL;
}

static void join(void)
{
    client = tide_client_create(&(tide_client_desc){.game = &tide_game_api,
                                                    .transport = tide_loopback_endpoint(net, 2),
                                                    .server = tide_loopback_address(1),
                                                    .sample = sample,
                                                    .lead = 2},
                                now);
}

static void run(const double until)
{
    for (; now < until; now += 1.0 / 60.0) {
        tide_loopback_set_time(net, now);
        if (client) tide_client_update(client, now);
        tide_server_update(server, now);
    }
}

static void stop(void)
{
    tide_client_destroy(client);
    tide_server_destroy(server);
    tide_loopback_destroy(net);
}

TIDE_TEST(delta_a_player_whose_world_went_wrong_gets_what_it_lacks)
{
    serve(NULL, (tide_net_conditions){.latency = 0.03, .jitter = 0.01, .loss = 0.05});
    join();
    run(2.0);
    const tide_client_status joined = tide_client_status_of(client);
    TIDE_REQUIRE(joined.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(joined.world_bytes > 0);

    // Something went wrong on this machine: a mover is elsewhere
    nudge((tide_world *)tide_client_world(client), 3, 3.0f);
    run(5.0);
    const tide_client_status after = tide_client_status_of(client);
    TIDE_CHECK(after.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(after.resyncs == 1); // Once, and right since: every tick's hash matched
    TIDE_CHECK((after.world_bytes - joined.world_bytes) * 10u < whole_size(tide_server_world(server)));
    TIDE_CHECK(after.verified_tick + 30u > tide_server_tick(server));
    stop();
}

// Starting a match calls no C here, so a machine joining one starts it too.
TIDE_TEST(delta_a_player_joining_starts_the_match_itself)
{
    TIDE_CHECK(tide_game_api.pure_start);
    const tide_start start = {.scene = -1}; // Main, as Session.Start says it
    serve(&start, (tide_net_conditions){.latency = 0.03, .jitter = 0.01, .loss = 0.05});
    run(3.0); // Sparks came and went, movers moved
    join();
    run(6.0);
    const tide_client_status s = tide_client_status_of(client);
    TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
    TIDE_CHECK(s.resyncs == 0);
    TIDE_CHECK(s.world_bytes > 0 && s.world_bytes * 10u < whole_size(tide_server_world(server)));    TIDE_CHECK(s.verified_tick + 30u > tide_server_tick(server));
    stop();
}

TIDE_TEST(delta_players_coming_back_to_a_match_that_changed_hands_get_what_they_lack)
{
    now = 0.0;
    net = tide_loopback_create(6);
    tide_loopback_set_conditions(net, (tide_net_conditions){.latency = 0.02});
    tide_session *machines[3];
    for (int i = 0; i < 3; i++) machines[i] = tide_session_create(&(tide_session_desc){.game = &tide_game_api, .sample = sample});
    tide_session_start(machines[0], NULL, now);
    tide_session_open(machines[0], tide_loopback_endpoint(net, 1));
    tide_session_set_room(machines[0], "ABCDEF", "key1");
    tide_session_join(machines[1], tide_loopback_endpoint(net, 2), tide_loopback_address(1), now);
    tide_session_join(machines[2], tide_loopback_endpoint(net, 3), tide_loopback_address(1), now);
    const double until = now + 2.0;
    for (; now < until; now += 1.0 / 60.0) {
        tide_loopback_set_time(net, now);
        for (int i = 0; i < 3; i++) tide_session_update(machines[i], now);
    }
    TIDE_REQUIRE(tide_session_status_of(machines[2]).client.state == TIDE_SESSION_CONNECTED);
    const uint64_t whole = whole_size(tide_session_server_world(machines[0]));

    tide_session_leave(machines[0]);
    for (const double gone = now + 0.2; now < gone; now += 1.0 / 60.0) {
        tide_loopback_set_time(net, now);
        for (int i = 1; i < 3; i++) tide_session_update(machines[i], now);
    }
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    char key[TIDE_ROOM_KEY_LENGTH + 1];
    TIDE_REQUIRE(tide_session_migrating(machines[1], code, key));
    TIDE_REQUIRE(tide_session_migrating(machines[2], code, key));
    tide_session_take_over(machines[1], tide_loopback_endpoint(net, 4), now);
    tide_session_join(machines[2], tide_loopback_endpoint(net, 5), tide_loopback_address(4), now);
    for (const double back = now + 2.0; now < back; now += 1.0 / 60.0) {
        tide_loopback_set_time(net, now);
        for (int i = 1; i < 3; i++) tide_session_update(machines[i], now);
    }
    for (int i = 1; i < 3; i++) {
        const tide_client_status s = tide_session_status_of(machines[i]).client;
        TIDE_CHECK(s.state == TIDE_SESSION_CONNECTED);
        TIDE_CHECK(s.resyncs == 0);
        TIDE_CHECK(s.world_bytes > 0 && s.world_bytes * 10u < whole); // Its world had most of it
    }
    for (int i = 0; i < 3; i++) tide_session_destroy(machines[i]);
    tide_loopback_destroy(net);
}
