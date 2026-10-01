// Fuzzing the WebRTC stack (platform/src/rtc): everything that reads what
// other machines send, fed real messages broken at random. It passes if
// nothing crashes, hangs or, built with sanitizers (TIDE_SANITIZE), touches
// memory it shouldn't. Checksums and signatures are made right again after
// breaking a message, most of the time, so the breakage gets past them.
//
//     tide_platform_rtc_fuzz [rounds] [seed]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc/rtc.h"

static uint64_t state = 0x9e3779b97f4a7c15ull;

static uint32_t next(void)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return (uint32_t)((state * 0x2545f4914f6cdd1dull) >> 32);
}

static uint32_t below(const uint32_t n)
{
    return n ? next() % n : 0;
}

// Breaks `data` (of `size`, with room for `capacity`) a few ways at once: its new size.
static size_t mutate(uint8_t *data, size_t size, const size_t capacity)
{
    const int changes = 1 + (int)below(4);
    for (int k = 0; k < changes; k++) {
        const uint32_t at = size ? below((uint32_t)size) : 0;
        switch (below(9)) {
        case 0: if (size) data[at] ^= (uint8_t)(1u << below(8)); break;
        case 1: if (size) data[at] = (uint8_t)next(); break;
        case 2: { // Interesting values
            static const uint8_t values[] = {0, 1, 0x7f, 0x80, 0xff, 0xfe, 20, 22, 23};
            if (size) data[at] = values[below(sizeof values)];
            break;
        }
        case 3: // A 16-bit field, like a length, at an edge
            if (size >= 2) {
                const uint32_t i = below((uint32_t)size - 1);
                static const uint16_t values[] = {0, 1, 0x7fff, 0x8000, 0xffff, 12, 13, 16};
                rtc_put16(data + i, values[below(8)]);
            }
            break;
        case 4: if (size) size = below((uint32_t)size); break; // Cut short
        case 5: { // Random bytes in
            const size_t n = 1 + below(16);
            if (size + n > capacity) break;
            memmove(data + at + n, data + at, size - at);
            for (size_t i = 0; i < n; i++) data[at + i] = (uint8_t)next();
            size += n;
            break;
        }
        case 6: { // A piece out
            const size_t n = 1 + below(16);
            if (at + n > size) break;
            memmove(data + at, data + at + n, size - at - n);
            size -= n;
            break;
        }
        case 7: { // A piece twice
            const size_t n = 1 + below(32);
            if (at + n > size || size + n > capacity) break;
            memmove(data + at + n, data + at, size - at);
            size += n;
            break;
        }
        default: // Another piece of itself, over it
            if (size >= 8) {
                const size_t from = below((uint32_t)size - 4), n = 1 + below(4);
                if (at + n <= size && from + n <= size) memmove(data + at, data + from, n);
            }
            break;
        }
    }
    return size;
}

typedef struct message {
    uint8_t data[2048];
    size_t size;
} message;

// ---------------------------------------------------------------------------
// Text: the relay's JSON, and SDP

static const char *const JSON_SEEDS[] = {
    ("{\"relay\":1,\"ice\":[{\"urls\":[\"stun:stun.cloudflare.com:3478\"]},{\"urls\":\"turn:t:3478?transport=udp\","
     "\"username\":\"u\",\"credential\":\"c\"}]}"),
    "{\"from\":3,\"signal\":{\"description\":{\"type\":\"offer\",\"sdp\":\"v=0\\r\\na=ice-ufrag:x\\r\\n\"}}}",
    "{\"signal\":{\"candidate\":{\"candidate\":\"candidate:1 1 udp 1 1.2.3.4 5 typ host\",\"sdpMid\":\"0\"}}}",
    "{\"missing\":\"K7QF2M\"}",
    "{\"a\":[[[[{\"b\":\"\\u00e9\\\\\"}]]]],\"c\":-1.5e3,\"d\":true,\"e\":null}",
};

static void fuzz_json(const int rounds)
{
    for (int r = 0; r < rounds; r++) {
        char text[4096];
        const char *seed = JSON_SEEDS[below(sizeof JSON_SEEDS / sizeof JSON_SEEDS[0])];
        size_t n = strlen(seed);
        memcpy(text, seed, n);
        n = mutate((uint8_t *)text, n, sizeof text - 1);
        const rtc_json m = rtc_json_of(text, n);
        static const char *const keys[] = {"relay", "ice", "urls", "from", "signal", "description", "type", "sdp",
                                           "candidate", "missing", "a", "c"};
        char out[512];
        double number;
        for (size_t k = 0; k < sizeof keys / sizeof keys[0]; k++) {
            const rtc_json v = rtc_json_get(m, keys[k]);
            rtc_json_string(v, out, sizeof out);
            rtc_json_number(v, &number);
            const char *at = NULL;
            rtc_json item;
            for (int i = 0; i < 64 && rtc_json_next(v, &at, &item); i++) {
                rtc_json_string(rtc_json_get(item, "urls"), out, sizeof out);
                rtc_json_string(item, out, sizeof out);
            }
        }
    }
}

static const char SDP_SEED[] =
    "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE 0\r\n"
    "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n"
    "a=candidate:1467250027 1 udp 2122260223 192.168.1.5 46243 typ host generation 0\r\n"
    "a=candidate:23456 1 udp 2122194687 7ab3c5e1-2d44.local 51234 typ host generation 0\r\n"
    "a=candidate:842163049 1 udp 1686052607 203.0.113.4 46243 typ srflx raddr 192.168.1.5 rport 46243\r\n"
    "a=ice-ufrag:SZzk\r\na=ice-pwd:CZjoUwvw/U4XnFw95ItXGrfQ\r\na=ice-options:trickle\r\n"
    "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:DC:B8:5F:64:1A:24:C2:43:F0:A1:58:D0:A1:2C:19:"
    "08\r\na=setup:actpass\r\na=mid:0\r\na=sctp-port:5000\r\na=max-message-size:262144\r\n";

static void fuzz_sdp(const int rounds)
{
    for (int r = 0; r < rounds; r++) {
        char text[4096];
        size_t n = sizeof SDP_SEED - 1;
        memcpy(text, SDP_SEED, n);
        n = mutate((uint8_t *)text, n, sizeof text - 1);
        text[n] = '\0';
        rtc_description d;
        rtc_sdp_parse(text, &d);
        rtc_candidate c;
        rtc_candidate_parse(text + below((uint32_t)n + 1), &c);
    }
}

// ---------------------------------------------------------------------------
// STUN, and certificates

static void fuzz_stun(const int rounds)
{
    message seeds[3];
    const uint8_t id[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    rtc_stun_writer w;
    rtc_stun_begin(&w, seeds[0].data, sizeof seeds[0].data, RTC_STUN_BINDING_REQUEST, id);
    rtc_stun_attr(&w, RTC_STUN_USERNAME, "abcd:efgh", 9);
    rtc_stun_attr32(&w, RTC_STUN_PRIORITY, 12345);
    rtc_stun_attr64(&w, RTC_STUN_ICE_CONTROLLING, 42);
    rtc_stun_attr(&w, RTC_STUN_USE_CANDIDATE, NULL, 0);
    rtc_stun_integrity(&w, "pwd", 3);
    rtc_stun_fingerprint(&w);
    seeds[0].size = w.size;
    rtc_stun_begin(&w, seeds[1].data, sizeof seeds[1].data, RTC_STUN_ALLOCATE_SUCCESS, id);
    rtc_stun_attr_addr(&w, RTC_STUN_XOR_RELAYED_ADDRESS, (rtc_addr){0x01020304, 5});
    rtc_stun_attr_addr(&w, RTC_STUN_XOR_MAPPED_ADDRESS, (rtc_addr){0x05060708, 9});
    rtc_stun_attr32(&w, RTC_STUN_LIFETIME, 600);
    rtc_stun_attr(&w, RTC_STUN_REALM, "cloudflare.com", 14);
    rtc_stun_attr(&w, RTC_STUN_NONCE, "abcdef", 6);
    rtc_stun_integrity(&w, "key", 3);
    seeds[1].size = w.size;
    rtc_stun_begin(&w, seeds[2].data, sizeof seeds[2].data, RTC_STUN_DATA_INDICATION, id);
    rtc_stun_attr_addr(&w, RTC_STUN_XOR_PEER_ADDRESS, (rtc_addr){0x0a000001, 1234});
    rtc_stun_attr(&w, RTC_STUN_DATA, "\x16\xfe\xfd hello", 10);
    seeds[2].size = w.size;
    for (int r = 0; r < rounds; r++) {
        message m = seeds[below(3)];
        m.size = mutate(m.data, m.size, sizeof m.data);
        if (m.size >= 4 && below(2)) rtc_put16(m.data + 2, (uint32_t)(m.size >= 20 ? m.size - 20 : 0)); // A fitting length
        rtc_stun s;
        if (rtc_stun_parse(m.data, m.size, &s)) {
            rtc_stun_check(&s, "pwd", 3);
            rtc_addr a;
            rtc_stun_read_addr(s.payload, &a);
        }
    }
}

static void fuzz_der(const int rounds)
{
    rtc_identity id;
    if (!rtc_identity_new(&id)) return;
    uint8_t sig[64] = {0x80, 1, 2}, der[72];
    const size_t sig_size = rtc_sig_to_der(sig, der);
    for (int r = 0; r < rounds; r++) {
        message m;
        uint8_t key[65];
        if (below(2)) {
            memcpy(m.data, id.cert, id.cert_size);
            m.size = mutate(m.data, id.cert_size, sizeof m.data);
            rtc_cert_key(m.data, m.size, key);
        } else {
            memcpy(m.data, der, sig_size);
            m.size = mutate(m.data, sig_size, sizeof m.data);
            rtc_sig_from_der(m.data, m.size, sig);
        }
    }
}

// ---------------------------------------------------------------------------
// DTLS and SCTP: real exchanges recorded, then played to fresh ends broken

typedef struct recording {
    message messages[64];
    int count;
} recording;

static void record(void *user, const void *data, const size_t size)
{
    recording *r = user;
    if (r->count < 64 && size <= sizeof r->messages[0].data) {
        memcpy(r->messages[r->count].data, data, size);
        r->messages[r->count++].size = size;
    }
}

static void ignore(void *user, const void *data, const size_t size)
{
    (void)user;
    (void)data;
    (void)size;
}

static rtc_identity client_id, server_id;
static recording to_server, to_client;

// A whole handshake, recorded both ways, and a little data after it.
static void record_handshake(void)
{
    rtc_identity_new(&client_id);
    rtc_identity_new(&server_id);
    static rtc_dtls client, server;
    rtc_dtls_start(&server, false, &server_id, client_id.fingerprint, record, ignore, &to_client, 0.0);
    rtc_dtls_start(&client, true, &client_id, server_id.fingerprint, record, ignore, &to_server, 0.0);
    int delivered_server = 0, delivered_client = 0;
    for (int step = 0; step < 50; step++) {
        while (delivered_server < to_server.count) {
            const message *m = &to_server.messages[delivered_server++];
            rtc_dtls_receive(&server, m->data, m->size, 0.0);
        }
        while (delivered_client < to_client.count) {
            const message *m = &to_client.messages[delivered_client++];
            rtc_dtls_receive(&client, m->data, m->size, 0.0);
        }
    }
    rtc_dtls_send(&client, "hello there", 11);
    rtc_dtls_send(&server, "general", 7);
    rtc_dtls_close(&client);
}

static void fuzz_dtls(const int rounds)
{
    for (int r = 0; r < rounds; r++) {
        // A fresh end, played what the other side sent, one of those broken
        const bool server = below(2);
        const recording *theirs = server ? &to_server : &to_client;
        static rtc_dtls d;
        recording ours = {0};
        rtc_dtls_start(&d, !server, server ? &server_id : &client_id, server ? client_id.fingerprint : server_id.fingerprint,
                       record, ignore, &ours, 0.0);
        const int broken = (int)below((uint32_t)theirs->count);
        for (int i = 0; i < theirs->count; i++) {
            message m = theirs->messages[i];
            if (i == broken) m.size = mutate(m.data, m.size, sizeof m.data);
            rtc_dtls_receive(&d, m.data, m.size, 0.1 * i);
            rtc_dtls_update(&d, 0.1 * i);
        }
    }
}

typedef struct sctp_side {
    rtc_sctp sctp;
    recording sent;
} sctp_side;

static sctp_side sctp_a, sctp_b;

static void fuzz_sctp(const int rounds)
{
    // Two ends talking: an association, the channel, messages and acks
    rtc_sctp_start(&sctp_a.sctp, true, 5000, record, ignore, &sctp_a.sent, 0.0);
    rtc_sctp_start(&sctp_b.sctp, false, 5000, record, ignore, &sctp_b.sent, 0.0);
    rtc_sctp_open_channel(&sctp_a.sctp, 0, 0.0);
    int a_read = 0, b_read = 0;
    for (int step = 0; step < 40; step++) {
        const double now = 0.05 * step;
        rtc_sctp_update(&sctp_a.sctp, now);
        rtc_sctp_update(&sctp_b.sctp, now);
        while (b_read < sctp_a.sent.count) {
            const message *m = &sctp_a.sent.messages[b_read++];
            rtc_sctp_receive(&sctp_b.sctp, m->data, m->size, now);
        }
        while (a_read < sctp_b.sent.count) {
            const message *m = &sctp_b.sent.messages[a_read++];
            rtc_sctp_receive(&sctp_a.sctp, m->data, m->size, now);
        }
        if (step > 10 && step % 3 == 0) {
            uint8_t big[1200] = {0};
            rtc_sctp_send(&sctp_a.sctp, big, 100 + (size_t)step * 20, now);
            rtc_sctp_send(&sctp_b.sctp, "x", 1, now);
        }
    }
    const rtc_sctp open_a = sctp_a.sctp, open_b = sctp_b.sctp;
    for (int r = 0; r < rounds; r++) {
        // Their packet, broken, into an end that's open, or one just starting
        const bool to_b = below(2);
        const recording *theirs = to_b ? &sctp_a.sent : &sctp_b.sent;
        static rtc_sctp s;
        s = below(4) ? (to_b ? open_b : open_a) : open_a;
        if (s.state != RTC_SCTP_OPEN) rtc_sctp_start(&s, false, 5000, ignore, ignore, NULL, 0.0);
        s.send = ignore;
        s.message = ignore;
        if (!theirs->count) break;
        message m = theirs->messages[below((uint32_t)theirs->count)];
        m.size = mutate(m.data, m.size, sizeof m.data);
        if (m.size >= 12 && below(8)) {
            memset(m.data + 8, 0, 4);
            const uint32_t crc = rtc_crc32c(m.data, m.size);
            m.data[8] = (uint8_t)crc;
            m.data[9] = (uint8_t)(crc >> 8);
            m.data[10] = (uint8_t)(crc >> 16);
            m.data[11] = (uint8_t)(crc >> 24);
        }
        rtc_sctp_receive(&s, m.data, m.size, 5.0);
        rtc_sctp_update(&s, 5.5);
    }
}

// ---------------------------------------------------------------------------
// ICE and TURN, over loopback: broken STUN at an agent's socket, signed with
// its password so it gets past the checks

static void fuzz_ice(const int rounds)
{
    const rtc_socket sender = rtc_udp_open();
    if (sender == RTC_NO_SOCKET) return;
    const rtc_addr from = {0x7f000001u, rtc_socket_port(sender)};
    rtc_ice_config config = {0};
    config.has_turn = true;
    config.turn = from; // What we send looks like the TURN server's too
    snprintf(config.username, sizeof config.username, "user");
    snprintf(config.credential, sizeof config.credential, "pass");
    static rtc_ice ice;
    if (!rtc_ice_start(&ice, false, &config)) return;
    rtc_ice_set_remote(&ice, "them", "their-password-1234567");
    const rtc_candidate them = {from, 1000, RTC_HOST, "1"};
    rtc_ice_add_remote(&ice, &them);
    const rtc_addr to = {0x7f000001u, rtc_socket_port(ice.socket)};
    uint8_t out[2048];
    for (int r = 0; r < rounds; r++) {
        message m;
        rtc_stun_writer w;
        uint8_t id[12];
        for (int i = 0; i < 12; i++) id[i] = (uint8_t)next();
        static const uint16_t types[] = {RTC_STUN_BINDING_REQUEST, RTC_STUN_BINDING_SUCCESS, RTC_STUN_ALLOCATE_SUCCESS,
                                         RTC_STUN_ALLOCATE_ERROR, RTC_STUN_DATA_INDICATION, RTC_STUN_PERMISSION_SUCCESS,
                                         RTC_STUN_REFRESH_SUCCESS};
        if (below(3) == 0) memcpy(id, ice.turn_id, 12); // Answering the TURN request in flight
        rtc_stun_begin(&w, m.data, sizeof m.data, types[below(7)], id);
        char username[64];
        snprintf(username, sizeof username, "%s:them", ice.ufrag);
        rtc_stun_attr(&w, RTC_STUN_USERNAME, username, strlen(username));
        rtc_stun_attr_addr(&w, RTC_STUN_XOR_RELAYED_ADDRESS, (rtc_addr){next(), (uint16_t)next()});
        rtc_stun_attr_addr(&w, RTC_STUN_XOR_PEER_ADDRESS, from);
        rtc_stun_attr(&w, RTC_STUN_DATA, "\x14\xfe\xfd", 3);
        rtc_stun_attr(&w, RTC_STUN_REALM, "realm", 5);
        rtc_stun_attr(&w, RTC_STUN_NONCE, "nonce", 5);
        uint8_t code[4] = {0, 0, 4, 1}; // 401
        rtc_stun_attr(&w, RTC_STUN_ERROR_CODE, code, 4);
        if (below(2)) rtc_stun_attr(&w, RTC_STUN_USE_CANDIDATE, NULL, 0);
        m.size = mutate(m.data, w.size, sizeof m.data - 40);
        if (m.size >= 20 && below(4)) {
            // Signed again, as whoever it claims to be would have
            rtc_put16(m.data + 2, (uint32_t)((m.size - 20) & ~3u));
            m.size = 20 + ((m.size - 20) & ~(size_t)3);
            w = (rtc_stun_writer){m.data, m.size, sizeof m.data, false};
            if (below(2)) rtc_stun_integrity(&w, ice.pwd, strlen(ice.pwd));
            else rtc_stun_integrity(&w, ice.turn_key, 16);
            rtc_stun_fingerprint(&w);
            m.size = w.size;
        }
        rtc_udp_send(sender, to, m.data, m.size);
        const double now = 0.01 * r;
        while (rtc_ice_receive(&ice, now, out, sizeof out)) {}
        rtc_ice_update(&ice, now);
    }
    rtc_ice_close(&ice);
    rtc_socket_close(sender);
}

// ---------------------------------------------------------------------------
// WebSocket frames, as the relay would send them

static void fuzz_ws(const int rounds)
{
    rtc_ws w;
    memset(&w, 0, sizeof w);
    w.socket = RTC_NO_SOCKET;
    w.upgraded = true;
    w.state = RTC_WS_OPEN;
    w.in = malloc(256 * 1024);
    w.out = malloc(256 * 1024);
    if (!w.in || !w.out) return;
    for (int r = 0; r < rounds; r++) {
        message m;
        const char *text = JSON_SEEDS[below(sizeof JSON_SEEDS / sizeof JSON_SEEDS[0])];
        const size_t n = strlen(text);
        m.data[0] = below(4) ? 0x81 : (uint8_t)(0x80 | below(16));
        size_t at = 2;
        if (n < 126) {
            m.data[1] = (uint8_t)n;
        } else {
            m.data[1] = 126;
            rtc_put16(m.data + 2, (uint32_t)n);
            at = 4;
        }
        memcpy(m.data + at, text, n);
        m.size = mutate(m.data, at + n, sizeof m.data);
        w.in_size = m.size;
        memcpy(w.in, m.data, m.size);
        w.state = RTC_WS_OPEN;
        w.out_size = 0;
        char got[4096];
        for (int i = 0; i < 8 && rtc_ws_next(&w, got, sizeof got); i++) {}
    }
    free(w.in);
    free(w.out);
}

int main(const int argc, char **argv)
{
    const int rounds = argc > 1 ? atoi(argv[1]) : 2000;
    state ^= argc > 2 ? strtoull(argv[2], NULL, 10) : 1u;
    const double start = rtc_now();
    fuzz_json(rounds);
    fuzz_sdp(rounds);
    fuzz_stun(rounds);
    fuzz_der(rounds);
    fuzz_ws(rounds);
    record_handshake();
    fuzz_dtls(rounds / 4 + 1); // Each plays a whole handshake: slower
    fuzz_sctp(rounds);
    fuzz_ice(rounds);
    printf("%d rounds of each, in %.1f seconds: nothing broke\n", rounds, rtc_now() - start);
    return 0;
}
