// SCTP for one WebRTC data channel, over DTLS (see rtc.h).
//
// The channel's messages go unordered and are never sent again: once one is
// missing for a while, FORWARD-TSN tells the other side to stop waiting for
// it (RFC 3758). Only the channel's open and ack (RFC 8832) are sent until
// they're acknowledged. Coming in, every DATA is passed on as soon as it's
// whole: messages that came in pieces are put back together first.

#include "rtc.h"

#include <string.h>

enum {
    DATA = 0,
    INIT = 1,
    INIT_ACK = 2,
    SACK = 3,
    HEARTBEAT = 4,
    HEARTBEAT_ACK = 5,
    ABORT = 6,
    SHUTDOWN = 7,
    SHUTDOWN_ACK = 8,
    COOKIE_ECHO = 10,
    COOKIE_ACK = 11,
    RECONFIG = 130,
    FORWARD_TSN = 192,
};
enum { PPID_DCEP = 50, PPID_STRING = 51, PPID_BINARY = 53, PPID_STRING_EMPTY = 56, PPID_BINARY_EMPTY = 57 };
enum { DCEP_ACK = 2, DCEP_OPEN = 3 };
enum { FLAG_END = 1, FLAG_BEGIN = 2, FLAG_UNORDERED = 4 };

#define A_RWND (1u << 20)
#define STREAMS 1024u
#define CONNECT_TIMEOUT 20.0
#define GIVE_UP_AFTER 0.2 // Seconds before a message not acknowledged is given up on
#define RESEND_AFTER 0.5  // The channel's open and ack

// a comes after b, in 32-bit serial arithmetic
static bool after(const uint32_t a, const uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

// ---------------------------------------------------------------------------
// Packets

typedef struct packet {
    uint8_t data[1400];
    size_t size;
    bool overflow;
} packet;

static void begin(const rtc_sctp *s, packet *p, const uint32_t tag)
{
    rtc_put16(p->data, s->port);
    rtc_put16(p->data + 2, s->their_port);
    rtc_put32(p->data + 4, tag);
    rtc_put32(p->data + 8, 0);
    p->size = 12;
    p->overflow = false;
}

static void chunk(packet *p, const uint8_t type, const uint8_t flags, const void *value, const size_t size)
{
    const size_t padded = (4 + size + 3) & ~(size_t)3;
    if (p->overflow || p->size + padded > sizeof p->data) {
        p->overflow = true;
        return;
    }
    uint8_t *c = p->data + p->size;
    c[0] = type;
    c[1] = flags;
    rtc_put16(c + 2, (uint32_t)(4 + size));
    if (size) memcpy(c + 4, value, size);
    memset(c + 4 + size, 0, padded - 4 - size);
    p->size += padded;
}

// The checksum is CRC32c, stored least significant byte first.
static void finish(rtc_sctp *s, packet *p)
{
    if (p->overflow) return;
    const uint32_t crc = rtc_crc32c(p->data, p->size);
    p->data[8] = (uint8_t)crc;
    p->data[9] = (uint8_t)(crc >> 8);
    p->data[10] = (uint8_t)(crc >> 16);
    p->data[11] = (uint8_t)(crc >> 24);
    s->send(s->user, p->data, p->size);
}

static void send_chunk(rtc_sctp *s, const uint8_t type, const uint8_t flags, const void *value, const size_t size)
{
    packet p;
    begin(s, &p, type == INIT ? 0 : s->their_tag);
    chunk(&p, type, flags, value, size);
    finish(s, &p);
}

// INIT and INIT ACK: our tag, window, streams and first TSN, then what we
// support: FORWARD-TSN and stream resets. INIT ACK adds the cookie.
static void send_init(rtc_sctp *s, const uint8_t type, const uint8_t *cookie, const size_t cookie_size)
{
    uint8_t v[128];
    rtc_put32(v, s->my_tag);
    rtc_put32(v + 4, A_RWND);
    rtc_put16(v + 8, STREAMS);
    rtc_put16(v + 10, STREAMS);
    rtc_put32(v + 12, s->my_initial_tsn);
    size_t n = 16;
    static const uint8_t extensions[] = {0x80, 0x08, 0x00, 0x06, FORWARD_TSN, RECONFIG, 0, 0};
    memcpy(v + n, extensions, sizeof extensions);
    n += sizeof extensions;
    static const uint8_t forward_tsn[] = {0xc0, 0x00, 0x00, 0x04};
    memcpy(v + n, forward_tsn, sizeof forward_tsn);
    n += sizeof forward_tsn;
    if (cookie) {
        rtc_put16(v + n, 7); // State Cookie
        rtc_put16(v + n + 2, (uint32_t)(4 + cookie_size));
        memcpy(v + n + 4, cookie, cookie_size);
        n += (4 + cookie_size + 3) & ~(size_t)3;
    }
    send_chunk(s, type, 0, v, n);
}

// ---------------------------------------------------------------------------
// Receiving

static bool has(const rtc_sctp *s, const uint32_t tsn)
{
    return s->received[(tsn % RTC_SCTP_WINDOW) / 8] >> (tsn % 8) & 1u;
}

static void mark(rtc_sctp *s, const uint32_t tsn, const bool on)
{
    uint8_t *byte = &s->received[(tsn % RTC_SCTP_WINDOW) / 8];
    const uint8_t bit = (uint8_t)(1u << (tsn % 8));
    *byte = on ? (uint8_t)(*byte | bit) : (uint8_t)(*byte & ~bit);
}

// Past every TSN that has come, clearing their bits for the ones after.
static void advance(rtc_sctp *s)
{
    while (has(s, s->cumulative + 1)) {
        mark(s, s->cumulative + 1, false);
        s->cumulative++;
    }
    for (int i = 0; i < RTC_SCTP_FRAGMENTS; i++) {
        if (s->fragments[i].used && !after(s->fragments[i].tsn, s->cumulative) && has(s, s->fragments[i].tsn)) {
            s->fragments[i].used = false;
        }
    }
}

static void send_dcep(rtc_sctp *s, const uint8_t *message, size_t size, double now);

static void deliver(rtc_sctp *s, const uint16_t stream, const uint32_t ppid, const uint8_t *data, const size_t size,
                    const double now)
{
    if (ppid == PPID_DCEP && size >= 1) {
        if (data[0] == DCEP_OPEN) {
            // Their channel: ours now, acknowledged on its stream
            s->stream = stream;
            s->channel = true;
            s->ssn = 0;
            static const uint8_t ack = DCEP_ACK;
            send_dcep(s, &ack, 1, now);
        } else if (data[0] == DCEP_ACK && s->opening && stream == s->stream) {
            s->channel = true;
        }
        return;
    }
    if (!s->channel || stream != s->stream) return;
    if (ppid == PPID_BINARY || ppid == PPID_STRING) s->message(s->user, data, size);
}

// A piece of a message: kept until the pieces from BEGIN to END are all here.
static void fragment(rtc_sctp *s, const uint32_t tsn, const uint8_t flags, const uint16_t stream, const uint32_t ppid,
                     const uint8_t *data, const size_t size, const double now)
{
    int slot = -1;
    for (int i = 0; i < RTC_SCTP_FRAGMENTS && slot < 0; i++) {
        if (!s->fragments[i].used) slot = i;
    }
    if (slot < 0 || size > sizeof s->fragments[0].data) return; // It won't come together: the channel is unreliable
    s->fragments[slot].used = true;
    s->fragments[slot].tsn = tsn;
    s->fragments[slot].flags = flags;
    s->fragments[slot].size = size;
    memcpy(s->fragments[slot].data, data, size);

    // A message from a BEGIN to the next END, TSN by TSN
    for (int b = 0; b < RTC_SCTP_FRAGMENTS; b++) {
        if (!s->fragments[b].used || !(s->fragments[b].flags & FLAG_BEGIN)) continue;
        int pieces[RTC_SCTP_FRAGMENTS];
        int count = 0;
        size_t total = 0;
        uint32_t want = s->fragments[b].tsn;
        bool whole = false;
        for (;;) {
            int found = -1;
            for (int i = 0; i < RTC_SCTP_FRAGMENTS; i++) {
                if (s->fragments[i].used && s->fragments[i].tsn == want) found = i;
            }
            if (found < 0 || count == RTC_SCTP_FRAGMENTS) break;
            pieces[count++] = found;
            total += s->fragments[found].size;
            if (s->fragments[found].flags & FLAG_END) {
                whole = true;
                break;
            }
            want++;
        }
        if (!whole || total > 65536) continue;
        uint8_t message[65536];
        size_t at = 0;
        for (int i = 0; i < count; i++) {
            memcpy(message + at, s->fragments[pieces[i]].data, s->fragments[pieces[i]].size);
            at += s->fragments[pieces[i]].size;
            s->fragments[pieces[i]].used = false;
        }
        deliver(s, stream, ppid, message, total, now);
        return;
    }
}

static void on_data(rtc_sctp *s, const uint8_t flags, const uint8_t *v, const size_t size, const double now)
{
    if (size < 12) return;
    const uint32_t tsn = rtc_get32(v);
    const uint16_t stream = rtc_get16(v + 4);
    const uint32_t ppid = rtc_get32(v + 8);
    if (!s->unacknowledged) s->first_unacknowledged = now;
    s->unacknowledged++;
    if (!after(tsn, s->cumulative) || tsn - s->cumulative > RTC_SCTP_WINDOW || has(s, tsn)) return; // Seen, or too far
    mark(s, tsn, true);
    if ((flags & (FLAG_BEGIN | FLAG_END)) == (FLAG_BEGIN | FLAG_END)) {
        deliver(s, stream, ppid, v + 12, size - 12, now);
    } else {
        fragment(s, tsn, flags, stream, ppid, v + 12, size - 12, now);
    }
    advance(s);
}

static void send_sack(rtc_sctp *s)
{
    uint8_t v[12 + 4 * 32];
    rtc_put32(v, s->cumulative);
    rtc_put32(v + 4, A_RWND);
    int blocks = 0;
    uint32_t offset = 2;
    while (offset <= RTC_SCTP_WINDOW && blocks < 32) {
        if (!has(s, s->cumulative + offset)) {
            offset++;
            continue;
        }
        const uint32_t start = offset;
        while (offset <= RTC_SCTP_WINDOW && has(s, s->cumulative + offset)) offset++;
        rtc_put16(v + 12 + 4 * blocks, start);
        rtc_put16(v + 14 + 4 * blocks, offset - 1);
        blocks++;
    }
    rtc_put16(v + 8, (uint32_t)blocks);
    rtc_put16(v + 10, 0);
    send_chunk(s, SACK, 0, v, 12 + 4 * (size_t)blocks);
    s->unacknowledged = 0;
}

static void on_sack(rtc_sctp *s, const uint8_t *v, const size_t size)
{
    if (size < 12) return;
    const uint32_t cumulative = rtc_get32(v);
    const uint32_t blocks = rtc_get16(v + 8);
    if (after(cumulative, s->their_cumulative)) s->their_cumulative = cumulative;
    for (int i = 0; i < s->sent_count; i++) {
        const uint32_t tsn = s->sent[i].tsn;
        bool acked = !after(tsn, cumulative);
        for (uint32_t b = 0; b < blocks && 16 + 4 * b <= size && !acked; b++) {
            const uint32_t start = cumulative + rtc_get16(v + 12 + 4 * b);
            const uint32_t end = cumulative + rtc_get16(v + 14 + 4 * b);
            acked = !after(start, tsn) && !after(tsn, end);
        }
        if (acked) s->sent[i].acked = true;
    }
}

static void on_forward_tsn(rtc_sctp *s, const uint8_t *v, const size_t size)
{
    if (size < 4) return;
    const uint32_t to = rtc_get32(v);
    while (after(to, s->cumulative)) {
        mark(s, s->cumulative + 1, false);
        s->cumulative++;
    }
    for (int i = 0; i < RTC_SCTP_FRAGMENTS; i++) {
        if (s->fragments[i].used && !after(s->fragments[i].tsn, s->cumulative)) s->fragments[i].used = false;
    }
    advance(s);
    s->unacknowledged++;
}

// Stream resets (RFC 6525): each outgoing reset they ask for is done. A reset
// of the channel's stream closes it.
static void on_reconfig(rtc_sctp *s, const uint8_t *v, const size_t size)
{
    size_t at = 0;
    while (at + 4 <= size) {
        const uint16_t type = rtc_get16(v + at);
        const size_t length = rtc_get16(v + at + 2);
        if (length < 4 || at + length > size) return;
        if (type == 13 && length >= 16) { // Outgoing SSN Reset Request
            for (size_t i = 16; i + 2 <= length; i += 2) {
                if (rtc_get16(v + at + i) == s->stream) s->channel = false;
            }
            if (length == 16) s->channel = false; // Every stream
            uint8_t response[12];
            rtc_put16(response, 16); // Re-configuration Response
            rtc_put16(response + 2, 12);
            memcpy(response + 4, v + at + 4, 4); // Its request's number
            rtc_put32(response + 8, 1);          // Done
            send_chunk(s, RECONFIG, 0, response, sizeof response);
        }
        at += (length + 3) & ~(size_t)3;
    }
}

static void opened(rtc_sctp *s, const double now)
{
    if (s->state == RTC_SCTP_OPEN) return;
    s->state = RTC_SCTP_OPEN;
    if (s->opening) {
        // DATA_CHANNEL_OPEN: unordered, never sent again, labelled "purr"
        static const uint8_t open[] = {DCEP_OPEN, 0x81, 0, 0, 0, 0, 0, 0, 0, 4, 0, 0, 'p', 'u', 'r', 'r'};
        send_dcep(s, open, sizeof open, now);
    }
}

void rtc_sctp_receive(rtc_sctp *s, const uint8_t *packet_data, const size_t size, const double now)
{
    if (size < 16 || s->state == RTC_SCTP_FAILED) return;
    uint8_t p[1500];
    if (size > sizeof p) return;
    memcpy(p, packet_data, size);
    const uint32_t checksum = (uint32_t)p[8] | (uint32_t)p[9] << 8 | (uint32_t)p[10] << 16 | (uint32_t)p[11] << 24;
    memset(p + 8, 0, 4);
    if (rtc_crc32c(p, size) != checksum) return;
    const uint32_t tag = rtc_get32(p + 4);

    size_t at = 12;
    while (at + 4 <= size) {
        const uint8_t type = p[at];
        const uint8_t flags = p[at + 1];
        const size_t length = rtc_get16(p + at + 2);
        if (length < 4 || at + length > size) return;
        const uint8_t *v = p + at + 4;
        const size_t n = length - 4;
        at += (length + 3) & ~(size_t)3;

        if (type != DATA && type != SACK) rtc_debug("sctp: their chunk %u (%u bytes)", (unsigned)type, (unsigned)length);
        if (type == INIT) {
            if (tag != 0 || n < 16) return;
            s->their_tag = rtc_get32(v);
            s->cumulative = rtc_get32(v + 12) - 1;
            memset(s->received, 0, sizeof s->received);
            uint8_t cookie[16];
            rtc_put32(cookie, s->my_tag);
            rtc_put32(cookie + 4, s->their_tag);
            memset(cookie + 8, 0x70, 8);
            send_init(s, INIT_ACK, cookie, sizeof cookie);
            if (s->state == RTC_SCTP_CLOSED) s->state = RTC_SCTP_CONNECTING;
            return; // INIT is alone in its packet
        }
        if (tag != s->my_tag) {
            rtc_debug("sctp: a packet for tag %08x, not ours (%08x)", (unsigned)tag, (unsigned)s->my_tag);
            return;
        }
        switch (type) {
        case INIT_ACK:
            if (s->state == RTC_SCTP_CONNECTING && n >= 16) {
                s->their_tag = rtc_get32(v);
                s->cumulative = rtc_get32(v + 12) - 1;
                memset(s->received, 0, sizeof s->received);
                for (size_t q = 16; q + 4 <= n;) {
                    const uint16_t param = rtc_get16(v + q);
                    const size_t param_size = rtc_get16(v + q + 2);
                    if (param_size < 4 || q + param_size > n) break;
                    if (param == 7 && param_size - 4 <= sizeof s->cookie) {
                        s->cookie_size = param_size - 4;
                        memcpy(s->cookie, v + q + 4, s->cookie_size);
                    }
                    q += (param_size + 3) & ~(size_t)3;
                }
                if (s->cookie_size) {
                    send_chunk(s, COOKIE_ECHO, 0, s->cookie, s->cookie_size);
                    s->init_sent = now;
                }
            }
            break;
        case COOKIE_ECHO:
            send_chunk(s, COOKIE_ACK, 0, NULL, 0);
            opened(s, now);
            break;
        case COOKIE_ACK: opened(s, now); break;
        case DATA:
            if (s->state == RTC_SCTP_OPEN) on_data(s, flags, v, n, now);
            break;
        case SACK: on_sack(s, v, n); break;
        case HEARTBEAT: send_chunk(s, HEARTBEAT_ACK, 0, v, n); break;
        case ABORT: s->state = RTC_SCTP_FAILED; return;
        case SHUTDOWN:
            send_chunk(s, SHUTDOWN_ACK, 0, NULL, 0);
            s->state = RTC_SCTP_CLOSED;
            s->channel = false;
            return;
        case FORWARD_TSN: on_forward_tsn(s, v, n); break;
        case RECONFIG: on_reconfig(s, v, n); break;
        default: break; // Unknown chunks are skipped
        }
    }
}

// ---------------------------------------------------------------------------
// Sending

static void remember(rtc_sctp *s, const uint32_t tsn, const bool reliable, const uint8_t *chunk_data, const size_t size,
                     const double now)
{
    if (s->sent_count == 64) {
        // Full: forget the oldest one that isn't sent until acknowledged
        int oldest = -1;
        for (int i = 0; i < s->sent_count && oldest < 0; i++) {
            if (!s->sent[i].reliable) oldest = i;
        }
        if (oldest < 0) return;
        memmove(&s->sent[oldest], &s->sent[oldest + 1], sizeof s->sent[0] * (size_t)(s->sent_count - oldest - 1));
        s->sent_count--;
    }
    s->sent[s->sent_count].tsn = tsn;
    s->sent[s->sent_count].sent = now;
    s->sent[s->sent_count].acked = false;
    s->sent[s->sent_count].reliable = reliable;
    s->sent[s->sent_count].size = reliable ? size : 0;
    if (reliable) memcpy(s->sent[s->sent_count].chunk, chunk_data, size);
    s->sent_count++;
}

// DATA's value: TSN, stream, stream sequence number, protocol and the message.
static size_t data_value(rtc_sctp *s, uint8_t *v, const uint16_t ssn, const uint32_t ppid, const void *message,
                         const size_t size)
{
    rtc_put32(v, s->next_tsn++);
    rtc_put16(v + 4, s->stream);
    rtc_put16(v + 6, ssn);
    rtc_put32(v + 8, ppid);
    memcpy(v + 12, message, size);
    return 12 + size;
}

// The channel's open and ack: ordered, and sent until acknowledged.
static void send_dcep(rtc_sctp *s, const uint8_t *message, const size_t size, const double now)
{
    uint8_t v[64];
    if (size > sizeof v - 12) return;
    const uint32_t tsn = s->next_tsn;
    const size_t n = data_value(s, v, s->ssn++, PPID_DCEP, message, size);
    send_chunk(s, DATA, FLAG_BEGIN | FLAG_END, v, n);
    remember(s, tsn, true, v, n, now);
}

void rtc_sctp_send(rtc_sctp *s, const void *data, const size_t size, const double now)
{
    if (s->state != RTC_SCTP_OPEN || !s->channel || size == 0 || size > 1200) return;
    uint8_t v[12 + 1200];
    const uint32_t tsn = s->next_tsn;
    const size_t n = data_value(s, v, 0, PPID_BINARY, data, size);
    send_chunk(s, DATA, FLAG_BEGIN | FLAG_END | FLAG_UNORDERED, v, n);
    remember(s, tsn, false, NULL, 0, now);
}

void rtc_sctp_open_channel(rtc_sctp *s, const uint16_t stream, const double now)
{
    s->opening = true;
    s->stream = stream;
    s->ssn = 0;
    if (s->state == RTC_SCTP_OPEN) {
        s->state = RTC_SCTP_CONNECTING; // So opened() sends the open
        opened(s, now);
    }
}

void rtc_sctp_start(rtc_sctp *s, const bool initiator, const uint16_t their_port, const rtc_send_fn send,
                    const rtc_message_fn message, void *user, const double now)
{
    memset(s, 0, sizeof *s);
    s->initiator = initiator;
    s->send = send;
    s->message = message;
    s->user = user;
    s->port = 5000;
    s->their_port = their_port ? their_port : 5000;
    s->started = now;
    s->state = RTC_SCTP_CONNECTING;
    rtc_random(&s->my_tag, sizeof s->my_tag);
    if (!s->my_tag) s->my_tag = 1;
    rtc_random(&s->my_initial_tsn, sizeof s->my_initial_tsn);
    s->next_tsn = s->my_initial_tsn;
    s->their_cumulative = s->my_initial_tsn - 1;
    s->forward = s->their_cumulative;
    s->init_sent = now - 10.0; // Due at once, for an initiator
}

void rtc_sctp_update(rtc_sctp *s, const double now)
{
    if (s->state == RTC_SCTP_FAILED || s->state == RTC_SCTP_CLOSED) return;
    if (s->state == RTC_SCTP_CONNECTING) {
        if (now - s->started > CONNECT_TIMEOUT) {
            s->state = RTC_SCTP_FAILED;
            return;
        }
        // INIT (or our COOKIE ECHO) again. A side that waits for the other's
        // INIT sends its own after a second without one.
        const bool due = s->initiator || now - s->started > 1.0;
        if (due && now - s->init_sent >= 0.5 * (1 << (s->init_tries < 4 ? s->init_tries : 4))) {
            if (s->cookie_size) send_chunk(s, COOKIE_ECHO, 0, s->cookie, s->cookie_size);
            else send_init(s, INIT, NULL, 0);
            s->init_sent = now;
            s->init_tries++;
        }
        return;
    }

    if (s->unacknowledged && (s->unacknowledged >= 2 || now - s->first_unacknowledged >= 0.02)) send_sack(s);

    // The open and ack again, until acknowledged
    for (int i = 0; i < s->sent_count; i++) {
        if (s->sent[i].reliable && !s->sent[i].acked && now - s->sent[i].sent >= RESEND_AFTER) {
            send_chunk(s, DATA, FLAG_BEGIN | FLAG_END, s->sent[i].chunk, s->sent[i].size);
            s->sent[i].sent = now;
        }
    }

    // Given up on: every TSN past theirs that was acknowledged, is old, or
    // was forgotten, up to the first that's still worth waiting for
    uint32_t point = after(s->forward, s->their_cumulative) ? s->forward : s->their_cumulative;
    while (after(s->next_tsn, point + 1)) {
        const uint32_t tsn = point + 1;
        int found = -1;
        for (int i = 0; i < s->sent_count && found < 0; i++) {
            if (s->sent[i].tsn == tsn) found = i;
        }
        if (found >= 0 && !s->sent[found].acked
            && (s->sent[found].reliable || now - s->sent[found].sent < GIVE_UP_AFTER)) {
            break;
        }
        point = tsn;
    }
    // What's at or before it is forgotten; what's acknowledged is too
    int kept = 0;
    for (int i = 0; i < s->sent_count; i++) {
        if (after(s->sent[i].tsn, point)) s->sent[kept++] = s->sent[i];
    }
    s->sent_count = kept;
    if (after(point, s->their_cumulative) && (after(point, s->forward) || now - s->forward_sent >= 0.2)) {
        uint8_t v[4];
        rtc_put32(v, point);
        send_chunk(s, FORWARD_TSN, 0, v, 4);
        s->forward = point;
        s->forward_sent = now;
    }
}
