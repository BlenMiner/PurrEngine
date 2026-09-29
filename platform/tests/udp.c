// Sessions over real UDP sockets on this machine: a server and two players,
// with a game written by hand in C (purr/session.h only needs its functions).

#include <stdio.h>
#include <string.h>

#include "purr/platform.h"
#include "purr/session.h"
#include "purr_test.h"

typedef struct world {
    uint32_t tick;
    uint32_t joined;
    int32_t inputs[PURR_MAX_PLAYERS + 1];
    int32_t sum; // Every input, added up every tick
} world;

static void start(void *w, const float dt, const void *s)
{
    (void)dt;
    (void)s;
    memset(w, 0, sizeof(world));
}

static void copy(void *to, const void *from)
{
    memcpy(to, from, sizeof(world));
}

static uint64_t hash(const void *w)
{
    return purr_hash(w, sizeof(world));
}

static void tick(void *w)
{
    world *x = w;
    for (uint32_t i = 0; i <= PURR_MAX_PLAYERS; i++) x->sum += x->inputs[i];
    x->tick++;
}

static void joined(void *w, const purr_player_id player)
{
    (void)player;
    ((world *)w)->joined++;
}

static void left(void *w, const purr_player_id player)
{
    (void)w;
    (void)player;
}

static void set_input(void *w, const purr_player_id player, const void *input)
{
    const int32_t index = purr_player_index(player);
    if (index >= 0) memcpy(&((world *)w)->inputs[index], input, 4);
}

static void set_server_input(void *w, const void *input)
{
    memcpy(&((world *)w)->inputs[PURR_MAX_PLAYERS], input, 4);
}

static uint32_t write_input(const void *input, uint8_t *out, const uint32_t capacity)
{
    if (capacity < 4) return 0;
    memcpy(out, input, 4);
    return 4;
}

static bool read_input(const uint8_t *data, const uint32_t size, void *input)
{
    if (size < 4) return false;
    memcpy(input, data, 4);
    return true;
}

static const purr_game game = {
    .hash = 42,
    .world_size = sizeof(world),
    .input_size = 4,
    .max_input_bytes = 4,
    .start = start,
    .tick = tick,
    .copy_world = copy,
    .hash_world = hash,
    .player_joined = joined,
    .player_left = left,
    .set_input = set_input,
    .set_server_input = set_server_input,
    .write_input = write_input,
    .read_input = read_input,
};

static void sample(void *user, const uint32_t t, void *input)
{
    const int32_t value = (int32_t)(t / 10u) * *(const int32_t *)user;
    memcpy(input, &value, 4);
}

PURR_TEST(udp_players_play_together)
{
    // A free port: the first that opens
    purr_transport network;
    uint16_t port = 47000;
    while (!purr_platform_udp_open_local(port, &network) && port < 47100) port++;
    PURR_REQUIRE(port < 47100);
    double now = 0.0;
    purr_server *server = purr_server_create(&(purr_server_desc){.game = &game, .tick_rate = 60, .transports = {network}}, now);
    PURR_REQUIRE(server != NULL);

    purr_address address;
    char text[32];
    snprintf(text, sizeof text, "localhost:%u", (unsigned)port);
    PURR_REQUIRE(purr_platform_resolve(text, 1, &address));
    PURR_CHECK(address.host == 0x7F000001u && address.port == port);

    int32_t factors[2] = {1, 3};
    purr_client *clients[2];
    for (int i = 0; i < 2; i++) {
        purr_transport t;
        PURR_REQUIRE(purr_platform_udp_open_local(0, &t));
        clients[i] = purr_client_create(&(purr_client_desc){.game = &game, .transport = t, .server = address,
                                                            .sample = sample, .user = &factors[i], .lead = 2},
                                        now);
    }
    for (int frame = 0; frame < 240; frame++) {
        now += 1.0 / 60.0;
        for (int i = 0; i < 2; i++) purr_client_update(clients[i], now);
        purr_server_update(server, now);
    }
    const world *w = purr_server_world(server);
    PURR_CHECK(w->joined == 2);
    PURR_CHECK(w->sum != 0);
    for (int i = 0; i < 2; i++) {
        const purr_client_status s = purr_client_status_of(clients[i]);
        PURR_CHECK(s.state == PURR_SESSION_CONNECTED);
        PURR_CHECK(s.resyncs == 0);
        PURR_CHECK(s.verified_tick + 10u > purr_server_tick(server));
        purr_client_destroy(clients[i]);
    }
    purr_server_destroy(server);
}

PURR_TEST(udp_addresses)
{
    purr_address a;
    PURR_CHECK(purr_platform_resolve("192.168.1.5", 7777, &a) && a.host == 0xC0A80105u && a.port == 7777);
    PURR_CHECK(purr_platform_resolve("10.0.0.1:1234", 7777, &a) && a.host == 0x0A000001u && a.port == 1234);
    PURR_CHECK(!purr_platform_resolve("10.0.0.1:99999", 7777, &a));
    PURR_CHECK(!purr_platform_resolve("", 7777, &a));
}
