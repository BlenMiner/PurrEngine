// Rooms (see tide/platform.h): matches found by a code, through the relay.
// On the web, the page's JavaScript does the work (platform/web/tide.js), on
// the browser's WebRTC. On desktop, this does the same on our own
// (platform/src/rtc). Either way, a room is a transport: players are numbered
// in it, the host 0 to those who join, and they 1 and up to the host.

#if !defined(_WIN32) && !defined(__APPLE__) && !defined(__wasm__)
#define _DEFAULT_SOURCE
#endif

#include "tide/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The room this program is in, whichever machine does the work: `number`
// tells rooms apart, so a transport left over from the last one does nothing.
static uint32_t backend_host(void);
static uint32_t backend_join(const char *code);
static void backend_close(uint32_t number);
static void backend_code(char out[TIDE_ROOM_CODE_LENGTH + 1]);
static bool backend_failed(void);
static void backend_send(uint32_t number, uint32_t to, const void *data, uint32_t size);
static uint32_t backend_receive(uint32_t number, uint32_t *from, void *data, uint32_t capacity);

#if defined(__wasm__)

#include "tide_web.h"

static uint32_t backend_host(void)
{
    return tide_web_room_host();
}

static uint32_t backend_join(const char *code)
{
    return tide_web_room_join(code);
}

static void backend_close(const uint32_t number)
{
    tide_web_room_close(number);
}

static void backend_code(char out[TIDE_ROOM_CODE_LENGTH + 1])
{
    tide_web_room_code(out);
}

static bool backend_failed(void)
{
    return tide_web_room_failed();
}

static void backend_send(const uint32_t number, const uint32_t to, const void *data, const uint32_t size)
{
    tide_web_room_send(number, to, data, size);
}

static uint32_t backend_receive(const uint32_t number, uint32_t *from, void *data, const uint32_t capacity)
{
    return tide_web_room_receive(number, from, data, capacity);
}

#else

#include "rtc/rtc.h"

#define RELAY "wss://relay.tide-engine.dev"
#define PEERS 16

typedef struct room_peer {
    rtc_peer *peer;
    uint32_t number;
    int relay_id; // The relay's number for them, while they're joining; -1 after
} room_peer;

typedef struct native_room {
    uint32_t number;
    bool hosting;
    bool failed;
    bool reachable; // Hosting: the relay knows the room
    bool connected; // Joining: the host is reached
    double opened;
    double join_again; // Joining a room the relay doesn't know yet: when to ask again
    char code[TIDE_ROOM_CODE_LENGTH + 1];
    rtc_ws ws;
    bool ws_live;
    double reconnect_at;
    rtc_ice_config ice;
    room_peer peers[PEERS];
    int peer_count;
    uint32_t next_number;
    int next_read;
} native_room;

static native_room *room;
static uint32_t rooms_opened;

// TIDE_RELAY picks another relay, like one on this machine for tests, which
// can be plain ws://.
static const char *relay_url(void)
{
    const char *env = getenv("TIDE_RELAY");
    return env && env[0] ? env : RELAY;
}

static void new_code(char code[TIDE_ROOM_CODE_LENGTH + 1])
{
    uint8_t r[TIDE_ROOM_CODE_LENGTH];
    rtc_random(r, sizeof r);
    for (int i = 0; i < TIDE_ROOM_CODE_LENGTH; i++) code[i] = TIDE_ROOM_CODE_LETTERS[r[i] & 31];
    code[TIDE_ROOM_CODE_LENGTH] = '\0';
}

static void relay_send(native_room *r, const char *json)
{
    if (r->ws_live && r->ws.state == RTC_WS_OPEN) rtc_ws_send(&r->ws, json, strlen(json));
}

static void relay_close(native_room *r)
{
    if (r->ws_live) rtc_ws_close(&r->ws);
    r->ws_live = false;
}

static void remove_peer(native_room *r, const int i)
{
    rtc_peer_free(r->peers[i].peer);
    r->peers[i] = r->peers[--r->peer_count];
}

// The relay's gone: a host opens its room again once it's back, and players
// already in keep playing; one joining can't go on.
static void relay_lost(native_room *r, const double now)
{
    relay_close(r);
    if (r->hosting) {
        if (r->reachable) fprintf(stderr, "tide: lost the relay; room %s opens again once it's back\n", r->code);
        r->reachable = false;
        r->reconnect_at = now + 3.0;
        for (int i = r->peer_count - 1; i >= 0; i--) {
            if (r->peers[i].peer->state != RTC_PEER_OPEN) remove_peer(r, i);
            else r->peers[i].relay_id = -1;
        }
    } else if (!r->connected && !r->failed && !r->peer_count && now - r->opened < 10.0) {
        // Not introduced yet: the relay may be waking up (it refuses
        // connections for a moment then), so try again
        r->reconnect_at = now + 1.0;
    } else if (!r->connected && !r->failed) {
        r->failed = true;
        fprintf(stderr, "tide: can't reach the relay at %s to join room %s\n", relay_url(), r->code);
    }
}

static void relay_connect(native_room *r, const double now)
{
    relay_close(r);
    r->ws_live = rtc_ws_open(&r->ws, relay_url(), now);
    if (!r->ws_live) relay_lost(r, now);
}

// "stun:host:port" or "turn:host:port?transport=udp": its address.
static bool server_address(const char *url, rtc_addr *out)
{
    const char *host = strchr(url, ':') + 1;
    char name[256];
    size_t n = 0;
    while (host[n] && host[n] != ':' && host[n] != '?' && n < sizeof name - 1) {
        name[n] = host[n];
        n++;
    }
    name[n] = '\0';
    const uint16_t port = host[n] == ':' ? (uint16_t)strtoul(host + n + 1, NULL, 10) : 3478;
    return n > 0 && rtc_resolve(name, port, out);
}

// The relay's ICE servers: the first STUN one, and the first TURN one on UDP.
static void read_ice(native_room *r, const rtc_json servers)
{
    memset(&r->ice, 0, sizeof r->ice);
    const char *at = NULL;
    rtc_json server;
    while (rtc_json_next(servers, &at, &server)) {
        const rtc_json urls = rtc_json_get(server, "urls");
        char url[256], username[256] = "", credential[256] = "";
        rtc_json_string(rtc_json_get(server, "username"), username, sizeof username);
        rtc_json_string(rtc_json_get(server, "credential"), credential, sizeof credential);
        const char *u = NULL;
        rtc_json item = urls;
        const bool list = urls.text && urls.text[0] == '[';
        while (list ? rtc_json_next(urls, &u, &item) : item.text != NULL) {
            if (rtc_json_string(item, url, sizeof url)) {
                const bool udp = !strstr(url, "transport=") || strstr(url, "transport=udp");
                if (!r->ice.has_stun && strncmp(url, "stun:", 5) == 0) {
                    r->ice.has_stun = server_address(url, &r->ice.stun);
                } else if (!r->ice.has_turn && strncmp(url, "turn:", 5) == 0 && udp && username[0]) {
                    r->ice.has_turn = server_address(url, &r->ice.turn);
                    snprintf(r->ice.username, sizeof r->ice.username, "%s", username);
                    snprintf(r->ice.credential, sizeof r->ice.credential, "%s", credential);
                }
            }
            if (!list) break;
        }
    }
}

static void apply_signal(rtc_peer *p, const rtc_json signal)
{
    static char type[16], sdp[8192], candidate[512];
    const rtc_json description = rtc_json_get(signal, "description");
    const rtc_json c = rtc_json_get(signal, "candidate");
    if (description.text && rtc_json_string(rtc_json_get(description, "type"), type, sizeof type)
        && rtc_json_string(rtc_json_get(description, "sdp"), sdp, sizeof sdp)) {
        rtc_peer_description(p, type, sdp);
    }
    if (c.text && rtc_json_string(rtc_json_get(c, "candidate"), candidate, sizeof candidate)) {
        rtc_peer_candidate(p, candidate);
    }
}

static room_peer *add_peer(native_room *r, const bool offerer, const uint32_t number, const int relay_id)
{
    if (r->peer_count == PEERS) return NULL;
    rtc_peer *p = rtc_peer_new(offerer, &r->ice);
    if (!p) return NULL;
    r->peers[r->peer_count] = (room_peer){p, number, relay_id};
    return &r->peers[r->peer_count++];
}

static room_peer *peer_by_relay(native_room *r, const double id)
{
    for (int i = 0; i < r->peer_count; i++) {
        if (r->peers[i].relay_id >= 0 && (double)r->peers[i].relay_id == id) return &r->peers[i];
    }
    return NULL;
}

static void on_relay(native_room *r, const char *text)
{
    const rtc_json m = rtc_json_of(text, strlen(text));
    if (!m.text) return;
    char code[16], json[64];
    double n = 0;
    if (rtc_json_get(m, "relay").text) {
        read_ice(r, rtc_json_get(m, "ice"));
        snprintf(json, sizeof json, "{\"%s\":\"%s\"}", r->hosting ? "host" : "join", r->code);
        relay_send(r, json);
    } else if (rtc_json_get(m, "hosting").text) {
        r->reachable = true;
    } else if (rtc_json_get(m, "taken").text) {
        new_code(r->code);
        snprintf(json, sizeof json, "{\"host\":\"%s\"}", r->code);
        relay_send(r, json);
    } else if (rtc_json_string(rtc_json_get(m, "missing"), code, sizeof code) && !r->hosting
               && rtc_now() - r->opened < 5.0) {
        // A room its host just opened, which the relay may not know yet
        // (the relay can take a few seconds to wake up): ask again soon
        r->join_again = rtc_now() + 0.5;
    } else if (rtc_json_string(rtc_json_get(m, "missing"), code, sizeof code)
               || rtc_json_string(rtc_json_get(m, "full"), code, sizeof code)
               || rtc_json_string(rtc_json_get(m, "closed"), code, sizeof code)) {
        if (!r->connected && !r->failed) {
            r->failed = true;
            fprintf(stderr, "%s%s\n", rtc_json_get(m, "missing").text ? "tide: no room has the code "
                                      : rtc_json_get(m, "full").text  ? "tide: too many players are joining room "
                                                                      : "tide: the host closed room ",
                    code);
        }
    } else if (rtc_json_get(m, "joined").text) {
        if (!r->hosting && !r->peer_count && !add_peer(r, true, 0, 0)) r->failed = true;
    } else if (rtc_json_number(rtc_json_get(m, "peer"), &n)) {
        if (r->hosting) add_peer(r, false, r->next_number++, (int)n);
    } else if (rtc_json_number(rtc_json_get(m, "from"), &n)) {
        room_peer *p = r->hosting ? peer_by_relay(r, n) : NULL;
        if (p) apply_signal(p->peer, rtc_json_get(m, "signal"));
    } else if (rtc_json_get(m, "signal").text) {
        if (!r->hosting && r->peer_count) apply_signal(r->peers[0].peer, rtc_json_get(m, "signal"));
    } else if (rtc_json_number(rtc_json_get(m, "left"), &n)) {
        room_peer *p = r->hosting ? peer_by_relay(r, n) : NULL;
        if (p && p->peer->state != RTC_PEER_OPEN) remove_peer(r, (int)(p - r->peers));
        else if (p) p->relay_id = -1;
    }
}

// Everything the room has to do: the relay's messages, and each peer's.
static void pump(native_room *r)
{
    const double now = rtc_now();
    const bool joining = !r->hosting && !r->connected && !r->failed && r->reconnect_at > 0.0;
    if (!r->ws_live && (r->hosting || joining) && now >= r->reconnect_at) relay_connect(r, now);
    if (r->join_again > 0.0 && now >= r->join_again) {
        r->join_again = 0.0;
        char json[64];
        snprintf(json, sizeof json, "{\"join\":\"%s\"}", r->code);
        relay_send(r, json);
    }
    if (r->ws_live) {
        rtc_ws_update(&r->ws, now);
        static char message[65536];
        while (r->ws_live && rtc_ws_next(&r->ws, message, sizeof message)) on_relay(r, message);
        if (r->ws_live && r->ws.state == RTC_WS_CLOSED) relay_lost(r, now);
    }
    for (int i = r->peer_count - 1; i >= 0; i--) {
        room_peer *p = &r->peers[i];
        rtc_peer_update(p->peer, now);
        static char signal[8192], wrapped[8400];
        while (rtc_peer_next_signal(p->peer, signal, sizeof signal)) {
            if (r->hosting) snprintf(wrapped, sizeof wrapped, "{\"to\":%d,\"signal\":%s}", p->relay_id, signal);
            else snprintf(wrapped, sizeof wrapped, "{\"signal\":%s}", signal);
            relay_send(r, wrapped);
        }
        if (p->peer->state == RTC_PEER_OPEN && !r->hosting && !r->connected) {
            r->connected = true;
            relay_close(r); // Joined: the relay's part is done
        } else if (p->peer->state == RTC_PEER_FAILED) {
            if (!r->hosting && !r->connected && !r->failed) {
                r->failed = true;
                fprintf(stderr, "tide: couldn't connect to the host of room %s\n", r->code);
            }
            remove_peer(r, i);
        }
    }
}

static void backend_close(const uint32_t number)
{
    if (!room || room->number != number) return;
    while (room->peer_count) remove_peer(room, room->peer_count - 1);
    relay_close(room);
    free(room);
    room = NULL;
}

static uint32_t open_room(const bool hosting, const char *code)
{
    if (room) backend_close(room->number);
    room = calloc(1, sizeof *room);
    if (!room) return 0;
    room->number = ++rooms_opened;
    room->hosting = hosting;
    room->reachable = true;
    room->next_number = 1;
    room->opened = rtc_now();
    if (hosting) {
        new_code(room->code);
    } else {
        // Case and spaces don't matter
        size_t n = 0;
        bool valid = true;
        for (const char *c = code; *c && valid; c++) {
            if (*c == ' ') continue;
            const char upper = *c >= 'a' && *c <= 'z' ? (char)(*c - 'a' + 'A') : *c;
            valid = n < TIDE_ROOM_CODE_LENGTH && strchr(TIDE_ROOM_CODE_LETTERS, upper) != NULL;
            if (valid) room->code[n++] = upper;
        }
        if (!valid || n != TIDE_ROOM_CODE_LENGTH) {
            room->failed = true;
            fprintf(stderr, "tide: '%s' isn't a room code: they're 6 letters and digits, like K7QF2M\n", code);
            return room->number;
        }
    }
    relay_connect(room, rtc_now());
    return room->number;
}

static uint32_t backend_host(void)
{
    return open_room(true, NULL);
}

static uint32_t backend_join(const char *code)
{
    return open_room(false, code);
}

static void backend_code(char out[TIDE_ROOM_CODE_LENGTH + 1])
{
    const bool shown = room && !room->failed && room->reachable;
    snprintf(out, TIDE_ROOM_CODE_LENGTH + 1, "%s", shown ? room->code : "");
}

static bool backend_failed(void)
{
    return room && room->failed;
}

static void backend_send(const uint32_t number, const uint32_t to, const void *data, const uint32_t size)
{
    if (!room || room->number != number) return;
    for (int i = 0; i < room->peer_count; i++) {
        if (room->peers[i].number == to) rtc_peer_send(room->peers[i].peer, data, size);
    }
}

static uint32_t backend_receive(const uint32_t number, uint32_t *from, void *data, const uint32_t capacity)
{
    if (!room || room->number != number) return 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        for (int k = 0; k < room->peer_count; k++) {
            const int i = (room->next_read + k) % room->peer_count;
            const size_t n = rtc_peer_receive(room->peers[i].peer, data, capacity);
            if (n) {
                *from = room->peers[i].number;
                room->next_read = (i + 1) % room->peer_count;
                return (uint32_t)n;
            }
        }
        if (attempt == 0) pump(room); // Nothing waiting: see what's come
        if (!room || room->number != number) return 0;
    }
    return 0;
}

#endif

// ---------------------------------------------------------------------------
// Rooms as transports

// To those who join, the host's address also holds the room's code, so they
// know it from another room's host (as sessions do when they join again, see
// tide_session_join).
typedef struct room_transport {
    uint32_t number;
    uint32_t code; // Packed, with TIDE_ROOM_CODE_BIT; 0 in a room this machine hosts
} room_transport;

static void room_send(void *self, const tide_address to, const void *data, const uint32_t size)
{
    const room_transport *r = self;
    if (to.kind == TIDE_ADDRESS_ROOM && to.host == r->code) backend_send(r->number, to.port, data, size);
}

static uint32_t room_receive(void *self, tide_address *from, void *data, const uint32_t capacity)
{
    const room_transport *r = self;
    uint32_t player = 0;
    const uint32_t size = backend_receive(r->number, &player, data, capacity);
    if (size) *from = (tide_address){.kind = TIDE_ADDRESS_ROOM, .host = r->code, .port = (uint16_t)player};
    return size;
}

static void room_close(void *self)
{
    room_transport *r = self;
    backend_close(r->number);
    free(r);
}

// A transport for room `number`: false, closing it, without the memory.
static bool transport(const uint32_t number, const uint32_t code, tide_transport *out)
{
    room_transport *r = number ? malloc(sizeof *r) : NULL;
    if (!r) {
        backend_close(number);
        return false;
    }
    *r = (room_transport){number, code};
    *out = (tide_transport){.self = r, .send = room_send, .receive = room_receive, .close = room_close};
    return true;
}

// The code, packed: false if it isn't one.
static bool pack(const char *code, uint32_t *out)
{
    uint32_t packed = 0;
    int n = 0;
    for (const char *c = code; *c; c++) {
        if (*c == ' ') continue;
        const char upper = *c >= 'a' && *c <= 'z' ? (char)(*c - 'a' + 'A') : *c;
        const char *at = strchr(TIDE_ROOM_CODE_LETTERS, upper);
        if (!at || n == TIDE_ROOM_CODE_LENGTH) return false;
        packed = packed << 5 | (uint32_t)(at - TIDE_ROOM_CODE_LETTERS);
        n++;
    }
    *out = packed | TIDE_ROOM_CODE_BIT;
    return n == TIDE_ROOM_CODE_LENGTH;
}

bool tide_platform_room_host(tide_transport *out)
{
    return transport(backend_host(), 0, out);
}

bool tide_platform_room_join(const char *code, tide_transport *out, tide_address *server)
{
    // A code that isn't one fails in the backend, which says why
    uint32_t packed = 0;
    if (!pack(code, &packed)) packed = TIDE_ROOM_CODE_BIT;
    *server = (tide_address){.kind = TIDE_ADDRESS_ROOM, .host = packed, .port = 0};
    return transport(backend_join(code), packed, out);
}

void tide_platform_room_code(char *out, const size_t size)
{
    char code[TIDE_ROOM_CODE_LENGTH + 1] = {0};
    backend_code(code);
    if (size) snprintf(out, size, "%s", code);
}

bool tide_platform_room_failed(void)
{
    return backend_failed();
}

// ---------------------------------------------------------------------------
// Hosting: on a UDP port and in a room at once, where there's both

typedef struct both {
    tide_transport udp;
    tide_transport room;
    bool room_first; // Taking turns, so neither waits on the other
} both;

static void both_send(void *self, const tide_address to, const void *data, const uint32_t size)
{
    const both *b = self;
    const tide_transport *t = to.kind == TIDE_ADDRESS_ROOM ? &b->room : &b->udp;
    t->send(t->self, to, data, size);
}

static uint32_t both_receive(void *self, tide_address *from, void *data, const uint32_t capacity)
{
    both *b = self;
    b->room_first = !b->room_first;
    const tide_transport *first = b->room_first ? &b->room : &b->udp;
    const tide_transport *second = b->room_first ? &b->udp : &b->room;
    const uint32_t n = first->receive(first->self, from, data, capacity);
    return n ? n : second->receive(second->self, from, data, capacity);
}

static void both_close(void *self)
{
    both *b = self;
    b->udp.close(b->udp.self);
    b->room.close(b->room.self);
    free(b);
}

bool tide_platform_host_open(const uint16_t port, tide_transport *out)
{
    tide_transport udp = {0}, in_room = {0};
    const bool has_udp = tide_platform_udp_open(port, &udp);
    const bool has_room = tide_platform_room_host(&in_room);
    both *b = has_udp && has_room ? malloc(sizeof *b) : NULL;
    if (b) {
        *b = (both){udp, in_room, false};
        *out = (tide_transport){.self = b, .send = both_send, .receive = both_receive, .close = both_close};
        return true;
    }
    if (has_udp && has_room) {
        in_room.close(in_room.self);
        *out = udp;
        return true;
    }
    if (has_udp) *out = udp;
    if (has_room) *out = in_room;
    return has_udp || has_room;
}
