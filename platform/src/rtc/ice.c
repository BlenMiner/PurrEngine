// ICE (RFC 8445), with a STUN server for our public address and a TURN server
// (RFC 8656) for when no direct pair works; see rtc.h.
//
// Pairs are checked one at a time, the most promising first. The side that
// made the offer is controlling: it puts USE-CANDIDATE on its checks, and the
// first pair to answer is the one (aggressive nomination). The other side uses
// the pair the controlling one picked, once its own check there answered.
// Once there's a pair, checks go on every few seconds, so both sides know the
// other still wants the packets (consent, RFC 7675).

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WAITING, IN_PROGRESS, SUCCEEDED, FAILED };
enum { TURN_NONE, TURN_ALLOCATING, TURN_ALLOCATED, TURN_FAILED };

#define CHECK_EVERY 0.02    // Seconds between new checks (Ta)
#define CHECK_TRIES 7       // Before a pair fails
#define CONNECT_TIMEOUT 30.0
#define CONSENT_EVERY 2.0
#define CONSENT_TIMEOUT 20.0
#define SERVER_TRIES 6      // STUN and TURN requests, half a second apart
#define PERMISSION_LIFE 240.0 // They last 300 seconds; renewed before

static const uint32_t TYPE_PREFERENCE[] = {126, 100, 110, 0}; // Host, server-reflexive, peer-reflexive, relayed

// An address as text, for debugging: four at a time.
static const char *addr_text(const rtc_addr a)
{
    static char texts[4][24];
    static int next;
    char *t = texts[next++ % 4];
    snprintf(t, sizeof texts[0], "%u.%u.%u.%u:%u", (unsigned)(a.ip >> 24), (unsigned)(a.ip >> 16 & 255),
             (unsigned)(a.ip >> 8 & 255), (unsigned)(a.ip & 255), (unsigned)a.port);
    return t;
}

static uint32_t candidate_priority(const int type)
{
    return TYPE_PREFERENCE[type] << 24 | 65535u << 8 | 255u;
}

static bool random_id(uint8_t id[12])
{
    return rtc_random(id, 12);
}

// Letters and digits for ufrag and pwd.
static void random_text(char *out, const size_t n)
{
    static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+/";
    uint8_t r[32];
    rtc_random(r, n);
    for (size_t i = 0; i < n; i++) out[i] = letters[r[i] & 63];
    out[n] = '\0';
}

static void add_local(rtc_ice *ice, const int type, const rtc_addr a)
{
    for (int i = 0; i < ice->local_count; i++) {
        if (rtc_addr_equal(ice->locals[i].addr, a)) return; // No NAT: our public address is our own
    }
    if (ice->local_count == 4 || (ice->relay_only && type != RTC_RELAY)) return;
    rtc_candidate *c = &ice->locals[ice->local_count++];
    memset(c, 0, sizeof *c);
    c->addr = a;
    c->type = (uint8_t)type;
    c->priority = candidate_priority(type);
    snprintf(c->foundation, sizeof c->foundation, "%d", type + 1);
}

bool rtc_ice_start(rtc_ice *ice, const bool controlling, const rtc_ice_config *config)
{
    memset(ice, 0, sizeof *ice);
    ice->selected = -1;
    ice->controlling = controlling;
    ice->relay_only = getenv("TIDE_RTC_RELAY_ONLY") != NULL;
    ice->config = *config;
    if (!rtc_random(&ice->tiebreaker, sizeof ice->tiebreaker)) return false;
    random_text(ice->ufrag, 8);
    random_text(ice->pwd, 24);
    ice->socket = rtc_udp_open();
    if (ice->socket == RTC_NO_SOCKET) return false;
    uint32_t ip;
    if (rtc_local_ip(&ip)) add_local(ice, RTC_HOST, (rtc_addr){ip, rtc_socket_port(ice->socket)});
    ice->started = rtc_now();
    ice->turn_state = config->has_turn ? TURN_ALLOCATING : TURN_NONE;
    if (config->has_stun) random_id(ice->stun_id);
    return true;
}

void rtc_ice_close(rtc_ice *ice)
{
    rtc_socket_close(ice->socket);
    ice->socket = RTC_NO_SOCKET;
    ice->state = RTC_ICE_FAILED;
}

bool rtc_ice_next_local(rtc_ice *ice, rtc_candidate *c)
{
    if (ice->locals_told == ice->local_count) return false;
    *c = ice->locals[ice->locals_told++];
    return true;
}

// ---------------------------------------------------------------------------
// Pairs

static uint64_t pair_priority(const rtc_ice *ice, const uint32_t local, const uint32_t remote)
{
    const uint64_t g = ice->controlling ? local : remote;
    const uint64_t d = ice->controlling ? remote : local;
    return ((g < d ? g : d) << 32) + 2 * (g > d ? g : d) + (g > d ? 1u : 0u);
}

static int find_pair(const rtc_ice *ice, const rtc_addr remote, const bool relayed)
{
    for (int i = 0; i < ice->pair_count; i++) {
        if (ice->pairs[i].relayed == relayed && rtc_addr_equal(ice->pairs[i].remote, remote)) return i;
    }
    return -1;
}

static int add_pair(rtc_ice *ice, const rtc_addr remote, const uint32_t remote_priority, const bool relayed)
{
    const int found = find_pair(ice, remote, relayed);
    if (found >= 0 || ice->pair_count == RTC_MAX_PAIRS) return found;
    rtc_pair *p = &ice->pairs[ice->pair_count];
    memset(p, 0, sizeof *p);
    p->relayed = relayed;
    p->remote = remote;
    p->remote_priority = remote_priority;
    p->priority = pair_priority(ice, candidate_priority(relayed ? RTC_RELAY : RTC_HOST), remote_priority);
    p->state = WAITING;
    return ice->pair_count++;
}

static void add_permission(rtc_ice *ice, const uint32_t ip)
{
    for (int i = 0; i < ice->permission_count; i++) {
        if (ice->permissions[i].ip == ip) return;
    }
    if (ice->permission_count == RTC_MAX_PERMISSIONS) return;
    memset(&ice->permissions[ice->permission_count], 0, sizeof ice->permissions[0]);
    ice->permissions[ice->permission_count++].ip = ip;
}

static bool permitted(const rtc_ice *ice, const uint32_t ip)
{
    for (int i = 0; i < ice->permission_count; i++) {
        if (ice->permissions[i].ip == ip) return ice->permissions[i].granted;
    }
    return false;
}

void rtc_ice_add_remote(rtc_ice *ice, const rtc_candidate *c)
{
    for (int i = 0; i < ice->remote_count; i++) {
        if (rtc_addr_equal(ice->remotes[i].addr, c->addr)) return;
    }
    rtc_debug("ice: their candidate %s (type %d)", addr_text(c->addr), c->type);
    if (ice->remote_count < RTC_MAX_CANDIDATES) ice->remotes[ice->remote_count++] = *c;
    if (!ice->relay_only) add_pair(ice, c->addr, c->priority, false);
    if (ice->turn_state == TURN_ALLOCATED) {
        add_pair(ice, c->addr, c->priority, true);
        add_permission(ice, c->addr.ip);
    }
}

void rtc_ice_set_remote(rtc_ice *ice, const char *ufrag, const char *pwd)
{
    snprintf(ice->remote_ufrag, sizeof ice->remote_ufrag, "%s", ufrag);
    snprintf(ice->remote_pwd, sizeof ice->remote_pwd, "%s", pwd);
    ice->has_remote = true;
}

// ---------------------------------------------------------------------------
// Sending: straight from our socket, or through the TURN server

static void send_turn_indication(rtc_ice *ice, const rtc_addr peer, const void *data, const size_t size)
{
    uint8_t buffer[1600], id[12];
    random_id(id);
    rtc_stun_writer w;
    rtc_stun_begin(&w, buffer, sizeof buffer, RTC_STUN_SEND_INDICATION, id);
    rtc_stun_attr_addr(&w, RTC_STUN_XOR_PEER_ADDRESS, peer);
    rtc_stun_attr(&w, RTC_STUN_DATA, data, size);
    if (!w.overflow) rtc_udp_send(ice->socket, ice->config.turn, buffer, w.size);
}

static void send_on(rtc_ice *ice, const bool relayed, const rtc_addr to, const void *data, const size_t size)
{
    if (relayed) send_turn_indication(ice, to, data, size);
    else rtc_udp_send(ice->socket, to, data, size);
}

void rtc_ice_send(rtc_ice *ice, const void *data, const size_t size)
{
    if (ice->selected < 0 || ice->state != RTC_ICE_CONNECTED) return;
    const rtc_pair *p = &ice->pairs[ice->selected];
    send_on(ice, p->relayed, p->remote, data, size);
}

static void send_check(rtc_ice *ice, rtc_pair *p, const double now)
{
    if (!p->tries) random_id(p->id);
    char username[200];
    snprintf(username, sizeof username, "%s:%s", ice->remote_ufrag, ice->ufrag);
    uint8_t buffer[256];
    rtc_stun_writer w;
    rtc_stun_begin(&w, buffer, sizeof buffer, RTC_STUN_BINDING_REQUEST, p->id);
    rtc_stun_attr(&w, RTC_STUN_USERNAME, username, strlen(username));
    rtc_stun_attr32(&w, RTC_STUN_PRIORITY, candidate_priority(RTC_PRFLX));
    rtc_stun_attr64(&w, ice->controlling ? RTC_STUN_ICE_CONTROLLING : RTC_STUN_ICE_CONTROLLED, ice->tiebreaker);
    if (ice->controlling) rtc_stun_attr(&w, RTC_STUN_USE_CANDIDATE, NULL, 0);
    rtc_stun_integrity(&w, ice->remote_pwd, strlen(ice->remote_pwd));
    rtc_stun_fingerprint(&w);
    if (w.overflow) return;
    if (p->tries == 0) rtc_debug("ice: checking %s%s", addr_text(p->remote), p->relayed ? " through TURN" : "");
    send_on(ice, p->relayed, p->remote, buffer, w.size);
    p->sent = now;
    p->tries++;
}

// A TURN request, signed once the server has told us its realm.
static void send_turn_request(rtc_ice *ice, const uint16_t type, const uint8_t id[12], const rtc_addr *peer,
                              const bool lifetime)
{
    uint8_t buffer[512];
    rtc_stun_writer w;
    rtc_stun_begin(&w, buffer, sizeof buffer, type, id);
    if (type == RTC_STUN_ALLOCATE_REQUEST) rtc_stun_attr32(&w, RTC_STUN_REQUESTED_TRANSPORT, 17u << 24); // UDP
    if (lifetime) rtc_stun_attr32(&w, RTC_STUN_LIFETIME, 600);
    if (peer) rtc_stun_attr_addr(&w, RTC_STUN_XOR_PEER_ADDRESS, *peer);
    if (ice->realm[0]) {
        rtc_stun_attr(&w, RTC_STUN_USERNAME, ice->config.username, strlen(ice->config.username));
        rtc_stun_attr(&w, RTC_STUN_REALM, ice->realm, strlen(ice->realm));
        rtc_stun_attr(&w, RTC_STUN_NONCE, ice->nonce, strlen(ice->nonce));
        rtc_stun_integrity(&w, ice->turn_key, 16);
    }
    rtc_stun_fingerprint(&w);
    if (!w.overflow) rtc_udp_send(ice->socket, ice->config.turn, buffer, w.size);
}

// ---------------------------------------------------------------------------

void rtc_ice_update(rtc_ice *ice, const double now)
{
    if (ice->state == RTC_ICE_FAILED) return;

    // Our public address, from the STUN server
    if (ice->config.has_stun && ice->stun_tries < SERVER_TRIES && now - ice->stun_sent >= 0.5) {
        uint8_t buffer[64];
        rtc_stun_writer w;
        rtc_stun_begin(&w, buffer, sizeof buffer, RTC_STUN_BINDING_REQUEST, ice->stun_id);
        rtc_stun_fingerprint(&w);
        rtc_udp_send(ice->socket, ice->config.stun, buffer, w.size);
        ice->stun_sent = now;
        ice->stun_tries++;
    }

    // TURN: the allocation, its refreshes, and permissions for their addresses
    if (ice->turn_state == TURN_ALLOCATING && now - ice->turn_sent >= 0.5) {
        if (ice->turn_tries == SERVER_TRIES) {
            ice->turn_state = TURN_FAILED;
        } else {
            if (!ice->turn_tries) random_id(ice->turn_id);
            send_turn_request(ice, RTC_STUN_ALLOCATE_REQUEST, ice->turn_id, NULL, true);
            ice->turn_sent = now;
            ice->turn_tries++;
        }
    }
    if (ice->turn_state == TURN_ALLOCATED) {
        if (now >= ice->refresh_at) {
            random_id(ice->turn_id);
            send_turn_request(ice, RTC_STUN_REFRESH_REQUEST, ice->turn_id, NULL, true);
            ice->refresh_at = now + 5.0; // Again soon, unless it answers
        }
        for (int i = 0; i < ice->permission_count; i++) {
            const double due = ice->permissions[i].granted ? ice->permissions[i].granted_at + PERMISSION_LIFE : 0.0;
            if (now >= due && now - ice->permissions[i].sent >= 0.5) {
                random_id(ice->permissions[i].id);
                const rtc_addr peer = {ice->permissions[i].ip, 0};
                send_turn_request(ice, RTC_STUN_PERMISSION_REQUEST, ice->permissions[i].id, &peer, false);
                ice->permissions[i].sent = now;
            }
        }
    }

    if (!ice->has_remote) return;

    // Connected: checks every few seconds keep it, and it ends if they stop answering
    if (ice->selected >= 0) {
        if (now >= ice->next_consent) {
            rtc_pair *p = &ice->pairs[ice->selected];
            p->tries = 0;
            send_check(ice, p, now);
            ice->next_consent = now + CONSENT_EVERY;
        }
        if (now - ice->last_heard > CONSENT_TIMEOUT) ice->state = RTC_ICE_FAILED;
        return;
    }
    if (now - ice->started > CONNECT_TIMEOUT) {
        ice->state = RTC_ICE_FAILED;
        return;
    }

    // Checks: the ones in flight again after a while, then the best waiting one
    rtc_pair *next = NULL;
    for (int i = 0; i < ice->pair_count; i++) {
        rtc_pair *p = &ice->pairs[i];
        if (p->relayed && !permitted(ice, p->remote.ip)) continue;
        if (p->state == IN_PROGRESS && now - p->sent >= 0.1 * (double)(1 << (p->tries < 5 ? p->tries : 5))) {
            if (p->tries >= CHECK_TRIES) p->state = FAILED;
            else send_check(ice, p, now);
        }
        if (p->state == WAITING && (!next || p->priority > next->priority)) next = p;
    }
    if (next && now >= ice->next_check) {
        next->state = IN_PROGRESS;
        next->tries = 0;
        send_check(ice, next, now);
        ice->next_check = now + CHECK_EVERY;
    }
}

// The pair becomes the one in use, if it's better than what's used now.
static void select_pair(rtc_ice *ice, const int index, const double now)
{
    if (ice->selected >= 0 && ice->pairs[ice->selected].priority >= ice->pairs[index].priority) return;
    rtc_debug("ice: using the pair to %s%s", addr_text(ice->pairs[index].remote),
              ice->pairs[index].relayed ? " through TURN" : "");
    ice->selected = index;
    ice->state = RTC_ICE_CONNECTED;
    ice->last_heard = now;
    ice->next_consent = now + CONSENT_EVERY;
}

// A check from them: answered, and it tells us about pairs, and which they picked.
static void on_request(rtc_ice *ice, const rtc_stun *m, const rtc_addr from, const bool relayed, const double now)
{
    if (ice->relay_only && !relayed) return;
    // USERNAME is "ours:theirs", signed with our password
    const size_t ours = strlen(ice->ufrag);
    if (m->username.size <= ours || memcmp(m->username.data, ice->ufrag, ours) != 0 || m->username.data[ours] != ':'
        || !rtc_stun_check(m, ice->pwd, strlen(ice->pwd))) {
        rtc_debug("ice: a check from %s refused (username %.*s)", addr_text(from), (int)m->username.size,
                  (const char *)m->username.data);
        return;
    }
    rtc_debug("ice: a check from %s%s%s", addr_text(from), relayed ? " through TURN" : "",
              m->use_candidate ? ", which it picks" : "");
    uint8_t buffer[128];
    rtc_stun_writer w;
    rtc_stun_begin(&w, buffer, sizeof buffer, RTC_STUN_BINDING_SUCCESS, m->id);
    rtc_stun_attr_addr(&w, RTC_STUN_XOR_MAPPED_ADDRESS, from);
    rtc_stun_integrity(&w, ice->pwd, strlen(ice->pwd));
    rtc_stun_fingerprint(&w);
    send_on(ice, relayed, from, buffer, w.size);

    int index = find_pair(ice, from, relayed);
    if (index < 0) {
        // A new address for them (peer-reflexive): check it too
        index = add_pair(ice, from, m->priority ? m->priority : candidate_priority(RTC_PRFLX), relayed);
        if (index < 0) return;
    }
    rtc_pair *p = &ice->pairs[index];
    if (index == ice->selected) ice->last_heard = now;
    if (m->use_candidate && !ice->controlling) {
        p->nominated = true;
        if (p->state == SUCCEEDED) select_pair(ice, index, now);
    }
    if (p->state == FAILED || (p->state == WAITING && ice->has_remote)) {
        p->state = IN_PROGRESS; // Triggered: check back at once
        p->tries = 0;
        send_check(ice, p, now);
    }
}

static void on_response(rtc_ice *ice, const rtc_stun *m, const double now)
{
    // Our public address, from the STUN server
    if (memcmp(m->id, ice->stun_id, 12) == 0) {
        if (m->type == RTC_STUN_BINDING_SUCCESS && m->has_mapped) add_local(ice, RTC_SRFLX, m->mapped);
        ice->stun_tries = SERVER_TRIES;
        return;
    }
    for (int i = 0; i < ice->pair_count; i++) {
        rtc_pair *p = &ice->pairs[i];
        if (p->state == WAITING || memcmp(m->id, p->id, 12) != 0) continue;
        if (!rtc_stun_check(m, ice->remote_pwd, strlen(ice->remote_pwd))) {
            rtc_debug("ice: an answer from %s didn't check out", addr_text(p->remote));
            return;
        }
        rtc_debug("ice: %s answered: %s", addr_text(p->remote), m->type == RTC_STUN_BINDING_SUCCESS ? "yes" : "no");
        if (m->type != RTC_STUN_BINDING_SUCCESS) {
            p->state = FAILED;
            return;
        }
        if (i == ice->selected) {
            ice->last_heard = now; // Consent
            return;
        }
        p->state = SUCCEEDED;
        if (ice->controlling || p->nominated) select_pair(ice, i, now);
        return;
    }
}

static void on_turn(rtc_ice *ice, const rtc_stun *m, const double now)
{
    const bool allocate = memcmp(m->id, ice->turn_id, 12) == 0;
    if ((m->type == RTC_STUN_ALLOCATE_ERROR || m->type == RTC_STUN_REFRESH_ERROR) && allocate) {
        // 401: sign it with the realm and nonce it gave. 438: a new nonce.
        // Both come with them, or they're no answer.
        if ((m->error == 401 || m->error == 438) && m->realm.data && m->nonce.data && m->realm.size < sizeof ice->realm
            && m->nonce.size < sizeof ice->nonce) {
            const bool first = !ice->realm[0];
            memcpy(ice->realm, m->realm.data, m->realm.size);
            ice->realm[m->realm.size] = '\0';
            memcpy(ice->nonce, m->nonce.data, m->nonce.size);
            ice->nonce[m->nonce.size] = '\0';
            rtc_turn_key(ice->config.username, ice->realm, ice->config.credential, ice->turn_key);
            if (first || m->error == 438) {
                random_id(ice->turn_id);
                send_turn_request(ice, m->type == RTC_STUN_ALLOCATE_ERROR ? RTC_STUN_ALLOCATE_REQUEST
                                                                          : RTC_STUN_REFRESH_REQUEST,
                                  ice->turn_id, NULL, true);
                ice->turn_sent = now;
                return;
            }
        }
        if (m->type == RTC_STUN_ALLOCATE_ERROR) ice->turn_state = TURN_FAILED;
        return;
    }
    if (m->type == RTC_STUN_ALLOCATE_SUCCESS && allocate && ice->turn_state == TURN_ALLOCATING
        && rtc_stun_check(m, ice->turn_key, 16) && m->has_relayed) {
        ice->turn_state = TURN_ALLOCATED;
        ice->relayed = m->relayed;
        ice->lifetime = m->lifetime ? m->lifetime : 600;
        ice->refresh_at = now + ice->lifetime / 2.0;
        if (m->has_mapped) add_local(ice, RTC_SRFLX, m->mapped);
        add_local(ice, RTC_RELAY, m->relayed);
        for (int i = 0; i < ice->remote_count; i++) {
            add_pair(ice, ice->remotes[i].addr, ice->remotes[i].priority, true);
            add_permission(ice, ice->remotes[i].addr.ip);
        }
        return;
    }
    if (m->type == RTC_STUN_REFRESH_SUCCESS && allocate) {
        ice->refresh_at = now + (m->lifetime ? m->lifetime : 600) / 2.0;
        return;
    }
    for (int i = 0; i < ice->permission_count; i++) {
        if (memcmp(m->id, ice->permissions[i].id, 12) != 0) continue;
        if (m->type == RTC_STUN_PERMISSION_SUCCESS) {
            ice->permissions[i].granted = true;
            ice->permissions[i].granted_at = now;
        } else if (m->error == 438 && m->nonce.data && m->nonce.size < sizeof ice->nonce) {
            memcpy(ice->nonce, m->nonce.data, m->nonce.size);
            ice->nonce[m->nonce.size] = '\0';
            ice->permissions[i].sent = 0.0; // Again, with the new nonce
        }
        return;
    }
}

// A datagram from them (or relayed from them): STUN is handled here, and
// anything else is for the layer above, if it's from a pair we know.
static size_t on_datagram(rtc_ice *ice, const rtc_addr from, const bool relayed, const uint8_t *data,
                          const size_t size, const double now, uint8_t *out, const size_t capacity)
{
    rtc_stun m;
    if (rtc_stun_is(data, size)) {
        if (!rtc_stun_parse(data, size, &m)) return 0;
        if (m.type == RTC_STUN_BINDING_REQUEST) on_request(ice, &m, from, relayed, now);
        else if (m.type == RTC_STUN_BINDING_SUCCESS || m.type == RTC_STUN_BINDING_ERROR) on_response(ice, &m, now);
        return 0;
    }
    const int pair = find_pair(ice, from, relayed);
    if (pair < 0 || size > capacity) return 0;
    if (pair == ice->selected) ice->last_heard = now;
    memcpy(out, data, size);
    return size;
}

size_t rtc_ice_receive(rtc_ice *ice, const double now, uint8_t *out, const size_t capacity)
{
    if (ice->socket == RTC_NO_SOCKET) return 0;
    uint8_t buffer[2048];
    for (;;) {
        rtc_addr from;
        const size_t size = rtc_udp_receive(ice->socket, &from, buffer, sizeof buffer);
        if (!size) return 0;
        size_t got = 0;
        if (ice->config.has_turn && rtc_addr_equal(from, ice->config.turn) && rtc_stun_is(buffer, size)) {
            rtc_stun m;
            if (!rtc_stun_parse(buffer, size, &m)) continue;
            if (m.type == RTC_STUN_DATA_INDICATION && m.has_peer && m.payload.data) {
                got = on_datagram(ice, m.peer, true, m.payload.data, m.payload.size, now, out, capacity);
            } else if (memcmp(m.id, ice->stun_id, 12) == 0) {
                on_response(ice, &m, now); // The STUN server can be the TURN server
            } else {
                on_turn(ice, &m, now);
            }
        } else {
            got = on_datagram(ice, from, false, buffer, size, now, out, capacity);
        }
        if (got) return got;
    }
}
