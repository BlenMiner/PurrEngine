// DTLS 1.2 for WebRTC, both sides of it (see rtc.h): the handshake of RFC
// 5246 and 6347 with an ECDHE-ECDSA key exchange on P-256, the extended
// master secret of RFC 7627, and records sealed with ChaCha20-Poly1305 (RFC
// 7905). Nothing else is offered or accepted.
//
// The client's flights: ClientHello (again, with a cookie, if the server
// wants one), then Certificate, ClientKeyExchange, CertificateVerify,
// ChangeCipherSpec and Finished. The server's: ServerHello, Certificate,
// ServerKeyExchange, CertificateRequest and ServerHelloDone, then
// ChangeCipherSpec and Finished. A flight goes again until the other side's
// next one comes; the last one, whenever theirs comes again.

#include "rtc.h"

#include <string.h>

#define VERSION 0xfefdu // DTLS 1.2
#define CIPHER 0xcca9u  // TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256
#define ECDSA_SHA256 0x0403u
#define SECP256R1 23u
#define HANDSHAKE_TIMEOUT 30.0
#define DATAGRAM 1400 // Our flights go in datagrams no bigger

enum { CHANGE_CIPHER_SPEC = 20, ALERT = 21, HANDSHAKE = 22, APPLICATION_DATA = 23 };
enum {
    CLIENT_HELLO = 1,
    SERVER_HELLO = 2,
    HELLO_VERIFY_REQUEST = 3,
    CERTIFICATE = 11,
    SERVER_KEY_EXCHANGE = 12,
    CERTIFICATE_REQUEST = 13,
    SERVER_HELLO_DONE = 14,
    CERTIFICATE_VERIFY = 15,
    CLIENT_KEY_EXCHANGE = 16,
    FINISHED = 20,
};
enum { EXT_GROUPS = 10, EXT_POINT_FORMATS = 11, EXT_SIGNATURES = 13, EXT_EXTENDED_MASTER = 23, EXT_RENEGOTIATION = 0xff01 };

// Bytes being written
typedef struct out {
    uint8_t *p;
    size_t size;
    size_t capacity;
    bool overflow;
} out;

static void put(out *o, const void *data, const size_t n)
{
    if (o->overflow || o->size + n > o->capacity) {
        o->overflow = true;
        return;
    }
    memcpy(o->p + o->size, data, n);
    o->size += n;
}

static void put8(out *o, const uint32_t v)
{
    const uint8_t b = (uint8_t)v;
    put(o, &b, 1);
}

static void put16(out *o, const uint32_t v)
{
    uint8_t b[2];
    rtc_put16(b, v);
    put(o, b, 2);
}

static void put24(out *o, const uint32_t v)
{
    uint8_t b[3];
    rtc_put24(b, v);
    put(o, b, 3);
}

// Bytes being read: a reader that runs short says so, once, and reads zeros.
typedef struct in {
    const uint8_t *p;
    size_t size;
    size_t at;
    bool short_read;
} in;

static const uint8_t *take(in *r, const size_t n)
{
    if (r->short_read || r->size - r->at < n) {
        r->short_read = true;
        return NULL;
    }
    const uint8_t *p = r->p + r->at;
    r->at += n;
    return p;
}

static uint32_t take8(in *r)
{
    const uint8_t *p = take(r, 1);
    return p ? p[0] : 0;
}

static uint32_t take16(in *r)
{
    const uint8_t *p = take(r, 2);
    return p ? rtc_get16(p) : 0;
}

static uint32_t take24(in *r)
{
    const uint8_t *p = take(r, 3);
    return p ? rtc_get24(p) : 0;
}

static void fail(rtc_dtls *d)
{
    d->state = RTC_DTLS_FAILED;
    d->flight_count = 0;
}

// ---------------------------------------------------------------------------
// Records

// ChaCha20-Poly1305's nonce for a record: its epoch and sequence number, XOR
// the IV (RFC 7905, 2).
static void record_nonce(const uint8_t iv[12], const uint16_t epoch, const uint64_t seq, uint8_t nonce[12])
{
    const uint64_t n = (uint64_t)epoch << 48 | seq;
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) nonce[4 + i] ^= (uint8_t)(n >> (56 - 8 * i));
}

// The AEAD's additional data: epoch and sequence number, type, version and
// the plaintext's length.
static void record_ad(const uint8_t *header, const size_t size, uint8_t ad[13])
{
    memcpy(ad, header + 3, 8);
    memcpy(ad + 8, header, 3);
    rtc_put16(ad + 11, (uint32_t)size);
}

static void put_record(rtc_dtls *d, out *o, const uint8_t type, const uint16_t epoch, const uint8_t *body,
                       const size_t size)
{
    const size_t length = epoch ? size + 16 : size;
    if (o->overflow || o->size + 13 + length > o->capacity) {
        o->overflow = true;
        return;
    }
    uint8_t *r = o->p + o->size;
    const uint64_t seq = d->record_seq[epoch]++;
    r[0] = type;
    rtc_put16(r + 1, VERSION);
    rtc_put16(r + 3, epoch);
    rtc_put16(r + 5, (uint32_t)(seq >> 32));
    rtc_put32(r + 7, (uint32_t)seq);
    rtc_put16(r + 11, (uint32_t)length);
    if (epoch) {
        uint8_t nonce[12], ad[13];
        record_nonce(d->write_iv, epoch, seq, nonce);
        record_ad(r, size, ad);
        rtc_seal(d->write_key, nonce, ad, 13, body, size, r + 13);
    } else {
        memcpy(r + 13, body, size);
    }
    o->size += 13 + length;
}

// The flight, in as few datagrams as it fits. Each time with new record
// numbers, as DTLS wants for anything sent again.
static void send_flight(rtc_dtls *d, const double now)
{
    uint8_t buffer[DATAGRAM];
    out o = {buffer, 0, sizeof buffer, false};
    for (int i = 0; i < d->flight_count; i++) {
        const size_t before = o.size;
        put_record(d, &o, d->flight[i].type, d->flight[i].epoch, d->flight[i].body, d->flight[i].size);
        if (o.overflow && before > 0) {
            d->send(d->user, buffer, before);
            o = (out){buffer, 0, sizeof buffer, false};
            d->record_seq[d->flight[i].epoch]--; // That record didn't go
            put_record(d, &o, d->flight[i].type, d->flight[i].epoch, d->flight[i].body, d->flight[i].size);
        }
    }
    if (o.size && !o.overflow) d->send(d->user, buffer, o.size);
    d->flight_sent = now;
}

static void start_flight(rtc_dtls *d)
{
    d->flight_count = 0;
    d->flight_timeout = 0.5;
    d->flight_done = false;
}

static void add_to_flight(rtc_dtls *d, const uint8_t type, const uint16_t epoch, const uint8_t *body, const size_t size)
{
    if (d->flight_count == RTC_DTLS_FLIGHT || size > sizeof d->flight[0].body) {
        fail(d);
        return;
    }
    d->flight[d->flight_count].type = type;
    d->flight[d->flight_count].epoch = epoch;
    memcpy(d->flight[d->flight_count].body, body, size);
    d->flight[d->flight_count].size = size;
    d->flight_count++;
}

// A handshake message of ours: its header, the transcript, and the flight.
static void handshake_out(rtc_dtls *d, const uint8_t type, const uint8_t *body, const size_t size, const uint16_t epoch)
{
    uint8_t message[800];
    if (size + 12 > sizeof message) {
        fail(d);
        return;
    }
    message[0] = type;
    rtc_put24(message + 1, (uint32_t)size);
    rtc_put16(message + 4, d->seq_out++);
    rtc_put24(message + 6, 0);
    rtc_put24(message + 9, (uint32_t)size);
    if (size) memcpy(message + 12, body, size); // ServerHelloDone has no body
    rtc_sha256_add(&d->transcript, message, 12 + size);
    add_to_flight(d, HANDSHAKE, epoch, message, 12 + size);
}

static void transcript_hash(const rtc_dtls *d, uint8_t hash[32])
{
    rtc_sha256 copy = d->transcript;
    rtc_sha256_end(&copy, hash);
}

// ---------------------------------------------------------------------------
// Keys

static void derive_keys(rtc_dtls *d, const uint8_t premaster[32])
{
    if (d->extended_master) {
        uint8_t session_hash[32];
        transcript_hash(d, session_hash); // Up to ClientKeyExchange
        rtc_prf(premaster, 32, "extended master secret", session_hash, 32, NULL, 0, d->master, 48);
    } else {
        rtc_prf(premaster, 32, "master secret", d->client_random, 32, d->server_random, 32, d->master, 48);
    }
    d->keys = true;
    uint8_t block[88];
    rtc_prf(d->master, 48, "key expansion", d->server_random, 32, d->client_random, 32, block, sizeof block);
    const uint8_t *client_key = block, *server_key = block + 32, *client_iv = block + 64, *server_iv = block + 76;
    memcpy(d->write_key, d->client ? client_key : server_key, 32);
    memcpy(d->read_key, d->client ? server_key : client_key, 32);
    memcpy(d->write_iv, d->client ? client_iv : server_iv, 12);
    memcpy(d->read_iv, d->client ? server_iv : client_iv, 12);
}

static void finished_data(const rtc_dtls *d, const bool client, uint8_t out_data[12])
{
    uint8_t hash[32];
    transcript_hash(d, hash);
    rtc_prf(d->master, 48, client ? "client finished" : "server finished", hash, 32, NULL, 0, out_data, 12);
}

// ECDSA over SHA-256 of `data`, as TLS writes it: the algorithm, then the
// signature in DER.
static void put_signature(rtc_dtls *d, out *o, const uint8_t hash[32])
{
    uint8_t sig[64], der[72];
    if (!rtc_p256_sign(d->identity->private_key, hash, sig)) {
        fail(d);
        return;
    }
    const size_t n = rtc_sig_to_der(sig, der);
    put16(o, ECDSA_SHA256);
    put16(o, (uint32_t)n);
    put(o, der, n);
}

static bool check_signature(const rtc_dtls *d, in *r, const uint8_t hash[32])
{
    const uint32_t algorithm = take16(r);
    const uint32_t n = take16(r);
    const uint8_t *der = take(r, n);
    uint8_t sig[64];
    return der && algorithm == ECDSA_SHA256 && d->have_their_key && rtc_sig_from_der(der, n, sig)
        && rtc_p256_verify(d->their_key, hash, sig);
}

// Their Certificate: the first one must have the fingerprint their description gave.
static bool read_certificate(rtc_dtls *d, in *r)
{
    take24(r); // The whole list's length
    const uint32_t n = take24(r);
    const uint8_t *cert = take(r, n);
    if (!cert) return false;
    uint8_t fingerprint[32];
    rtc_sha256_of(cert, n, fingerprint);
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= (uint8_t)(fingerprint[i] ^ d->their_fingerprint[i]);
    d->have_their_key = diff == 0 && rtc_cert_key(cert, n, d->their_key);
    return d->have_their_key;
}

static void put_certificate(rtc_dtls *d)
{
    uint8_t body[600];
    out o = {body, 0, sizeof body, false};
    put24(&o, (uint32_t)d->identity->cert_size + 3);
    put24(&o, (uint32_t)d->identity->cert_size);
    put(&o, d->identity->cert, d->identity->cert_size);
    if (o.overflow) fail(d);
    else handshake_out(d, CERTIFICATE, body, o.size, 0);
}

// The hash ECDHE's parameters are signed over: both randoms, then the parameters.
static void params_hash(const rtc_dtls *d, const uint8_t *params, const size_t size, uint8_t hash[32])
{
    rtc_sha256 s;
    rtc_sha256_init(&s);
    rtc_sha256_add(&s, d->client_random, 32);
    rtc_sha256_add(&s, d->server_random, 32);
    rtc_sha256_add(&s, params, size);
    rtc_sha256_end(&s, hash);
}

// ChangeCipherSpec and Finished: from here on, our records are sealed.
static void put_finished(rtc_dtls *d)
{
    static const uint8_t change = 1;
    add_to_flight(d, CHANGE_CIPHER_SPEC, 0, &change, 1);
    d->epoch_out = 1;
    uint8_t verify[12];
    finished_data(d, d->client, verify);
    handshake_out(d, FINISHED, verify, sizeof verify, 1);
}

// ---------------------------------------------------------------------------
// The client

static void send_client_hello(rtc_dtls *d, const double now)
{
    uint8_t body[400];
    out o = {body, 0, sizeof body, false};
    put16(&o, VERSION);
    put(&o, d->client_random, 32);
    put8(&o, 0); // No session to resume
    put8(&o, (uint32_t)d->cookie_size);
    put(&o, d->cookie, d->cookie_size);
    put16(&o, 2);
    put16(&o, CIPHER);
    put8(&o, 1);
    put8(&o, 0); // No compression
    const size_t extensions_at = o.size;
    put16(&o, 0);
    put16(&o, EXT_EXTENDED_MASTER);
    put16(&o, 0);
    put16(&o, EXT_RENEGOTIATION);
    put16(&o, 1);
    put8(&o, 0);
    put16(&o, EXT_GROUPS);
    put16(&o, 4);
    put16(&o, 2);
    put16(&o, SECP256R1);
    put16(&o, EXT_POINT_FORMATS);
    put16(&o, 2);
    put8(&o, 1);
    put8(&o, 0); // Uncompressed
    put16(&o, EXT_SIGNATURES);
    put16(&o, 4);
    put16(&o, 2);
    put16(&o, ECDSA_SHA256);
    rtc_put16(body + extensions_at, (uint32_t)(o.size - extensions_at - 2));

    // Kept out of the transcript until a ServerHello answers this one
    d->hello[0] = CLIENT_HELLO;
    rtc_put24(d->hello + 1, (uint32_t)o.size);
    rtc_put16(d->hello + 4, d->seq_out++);
    rtc_put24(d->hello + 6, 0);
    rtc_put24(d->hello + 9, (uint32_t)o.size);
    memcpy(d->hello + 12, body, o.size);
    d->hello_size = 12 + o.size;
    start_flight(d);
    add_to_flight(d, HANDSHAKE, 0, d->hello, d->hello_size);
    send_flight(d, now);
}

// Extensions: whether the other side has the extended master secret and
// renegotiation_info.
static void read_extensions(rtc_dtls *d, in *r)
{
    if (r->at == r->size) return;
    const uint32_t total = take16(r);
    const size_t end = r->at + total;
    while (!r->short_read && r->at + 4 <= end) {
        const uint32_t type = take16(r);
        const uint32_t n = take16(r);
        take(r, n);
        if (type == EXT_EXTENDED_MASTER) d->extended_master = true;
        if (type == EXT_RENEGOTIATION) d->renegotiation_info = true;
    }
    if (r->at != end || end != r->size) r->short_read = true; // Extensions end the message, exactly
}

static bool client_message(rtc_dtls *d, const uint8_t type, in *r, const uint8_t *message, const size_t size,
                           const double now)
{
    switch (type) {
    case HELLO_VERIFY_REQUEST: {
        take16(r);
        const uint32_t n = take8(r);
        const uint8_t *cookie = take(r, n);
        if (!cookie || n > sizeof d->cookie) return false;
        memcpy(d->cookie, cookie, n);
        d->cookie_size = n;
        send_client_hello(d, now);
        return true;
    }
    case SERVER_HELLO: {
        take16(r);
        const uint8_t *random = take(r, 32);
        take(r, take8(r)); // Session
        const uint32_t cipher = take16(r);
        const uint32_t compression = take8(r);
        read_extensions(d, r);
        if (!random || r->short_read || cipher != CIPHER || compression != 0) return false;
        memcpy(d->server_random, random, 32);
        rtc_sha256_add(&d->transcript, d->hello, d->hello_size);
        break;
    }
    case CERTIFICATE:
        if (!read_certificate(d, r)) return false;
        break;
    case SERVER_KEY_EXCHANGE: {
        const size_t params_at = r->at;
        const uint32_t curve_type = take8(r);
        const uint32_t curve = take16(r);
        const uint32_t n = take8(r);
        const uint8_t *point = take(r, n);
        if (!point || curve_type != 3 || curve != SECP256R1 || n != 65) return false;
        uint8_t hash[32];
        params_hash(d, r->p + params_at, r->at - params_at, hash);
        if (!check_signature(d, r, hash)) return false;
        uint8_t premaster[32];
        if (!rtc_p256_keys(d->ecdh_private, d->ecdh_public) || !rtc_p256_ecdh(d->ecdh_private, point, premaster)) {
            return false;
        }
        memcpy(d->premaster, premaster, 32); // Until ClientKeyExchange is in the transcript
        break;
    }
    case CERTIFICATE_REQUEST:
    case SERVER_HELLO_DONE: break;
    case FINISHED: {
        uint8_t want[12];
        finished_data(d, false, want);
        const uint8_t *got = take(r, 12);
        if (!got || !d->changed_cipher || memcmp(got, want, 12) != 0) return false;
        d->state = RTC_DTLS_OPEN;
        d->flight_count = 0;
        return true;
    }
    default: return false;
    }
    rtc_sha256_add(&d->transcript, message, size);

    if (type == SERVER_HELLO_DONE) {
        // Our flight: Certificate, ClientKeyExchange, CertificateVerify, then Finished
        start_flight(d);
        put_certificate(d);
        uint8_t body[70];
        body[0] = 65;
        memcpy(body + 1, d->ecdh_public, 65);
        handshake_out(d, CLIENT_KEY_EXCHANGE, body, 66, 0);
        derive_keys(d, d->premaster);
        uint8_t hash[32], verify[100];
        transcript_hash(d, hash);
        out o = {verify, 0, sizeof verify, false};
        put_signature(d, &o, hash);
        handshake_out(d, CERTIFICATE_VERIFY, verify, o.size, 0);
        put_finished(d);
        if (d->state != RTC_DTLS_FAILED) send_flight(d, now);
    }
    return true;
}

// ---------------------------------------------------------------------------
// The server

static bool server_message(rtc_dtls *d, const uint8_t type, in *r, const uint8_t *message, const size_t size,
                           const double now)
{
    switch (type) {
    case CLIENT_HELLO: {
        take16(r);
        const uint8_t *random = take(r, 32);
        take(r, take8(r)); // Session
        take(r, take8(r)); // Cookie: we don't ask for one
        const uint32_t suites = take16(r);
        const uint8_t *list = take(r, suites);
        take(r, take8(r)); // Compression
        read_extensions(d, r);
        bool chacha = false;
        for (uint32_t i = 0; list && i + 1 < suites; i += 2) chacha |= rtc_get16(list + i) == CIPHER;
        if (!random || r->short_read || !chacha) return false;
        memcpy(d->client_random, random, 32);
        rtc_sha256_add(&d->transcript, message, size);

        start_flight(d);
        uint8_t body[200];
        out o = {body, 0, sizeof body, false};
        put16(&o, VERSION);
        put(&o, d->server_random, 32);
        put8(&o, 0);
        put16(&o, CIPHER);
        put8(&o, 0);
        const size_t extensions_at = o.size;
        put16(&o, 0);
        if (d->extended_master) {
            put16(&o, EXT_EXTENDED_MASTER);
            put16(&o, 0);
        }
        if (d->renegotiation_info) {
            put16(&o, EXT_RENEGOTIATION);
            put16(&o, 1);
            put8(&o, 0);
        }
        put16(&o, EXT_POINT_FORMATS);
        put16(&o, 2);
        put8(&o, 1);
        put8(&o, 0);
        rtc_put16(body + extensions_at, (uint32_t)(o.size - extensions_at - 2));
        handshake_out(d, SERVER_HELLO, body, o.size, 0);
        put_certificate(d);

        if (!rtc_p256_keys(d->ecdh_private, d->ecdh_public)) return false;
        o = (out){body, 0, sizeof body, false};
        put8(&o, 3); // A named curve
        put16(&o, SECP256R1);
        put8(&o, 65);
        put(&o, d->ecdh_public, 65);
        uint8_t hash[32];
        params_hash(d, body, o.size, hash);
        put_signature(d, &o, hash);
        handshake_out(d, SERVER_KEY_EXCHANGE, body, o.size, 0);

        static const uint8_t request[] = {1, 64, 0, 2, 4, 3, 0, 0}; // ECDSA certificates, ECDSA with SHA-256, any CA
        handshake_out(d, CERTIFICATE_REQUEST, request, sizeof request, 0);
        handshake_out(d, SERVER_HELLO_DONE, NULL, 0, 0);
        if (d->state != RTC_DTLS_FAILED) send_flight(d, now);
        return true;
    }
    case CERTIFICATE:
        if (!read_certificate(d, r)) return false;
        break;
    case CLIENT_KEY_EXCHANGE: {
        const uint32_t n = take8(r);
        const uint8_t *point = take(r, n);
        uint8_t premaster[32];
        if (!point || n != 65 || !rtc_p256_ecdh(d->ecdh_private, point, premaster)) return false;
        rtc_sha256_add(&d->transcript, message, size);
        derive_keys(d, premaster); // The session hash takes in ClientKeyExchange
        return true;
    }
    case CERTIFICATE_VERIFY: {
        uint8_t hash[32];
        transcript_hash(d, hash);
        if (!check_signature(d, r, hash)) return false;
        break;
    }
    case FINISHED: {
        uint8_t want[12];
        finished_data(d, true, want);
        const uint8_t *got = take(r, 12);
        if (!got || !d->changed_cipher || memcmp(got, want, 12) != 0) return false;
        rtc_sha256_add(&d->transcript, message, size);
        start_flight(d);
        put_finished(d);
        d->flight_done = true; // Sent again only if their flight comes again
        send_flight(d, now);
        d->state = RTC_DTLS_OPEN;
        return true;
    }
    default: return false;
    }
    rtc_sha256_add(&d->transcript, message, size);
    return true;
}

// ---------------------------------------------------------------------------

bool rtc_dtls_start(rtc_dtls *d, const bool client, const rtc_identity *id, const uint8_t their_fingerprint[32],
                    const rtc_send_fn send, const rtc_send_fn data, void *user, const double now)
{
    memset(d, 0, sizeof *d);
    d->client = client;
    d->identity = id;
    memcpy(d->their_fingerprint, their_fingerprint, 32);
    d->send = send;
    d->data = data;
    d->user = user;
    d->started = now;
    rtc_sha256_init(&d->transcript);
    if (!rtc_random(client ? d->client_random : d->server_random, 32)) return false;
    if (client) send_client_hello(d, now);
    return true;
}

// Handshake messages in a record, one fragment at a time.
static void handshake_record(rtc_dtls *d, const uint8_t *p, const size_t size, const double now)
{
    in r = {p, size, 0, false};
    while (r.at + 12 <= size && d->state == RTC_DTLS_HANDSHAKE) {
        const uint32_t type = take8(&r);
        const uint32_t length = take24(&r);
        const uint32_t seq = take16(&r);
        const uint32_t offset = take24(&r);
        const uint32_t fragment = take24(&r);
        const uint8_t *data = take(&r, fragment);
        if (!data) return;
        if (!d->client && d->seq_out == 0 && type == CLIENT_HELLO && offset == 0) d->seq_in = seq; // Their first
        if (seq < d->seq_in) {
            // Theirs, again: ours didn't get there. Once per flight of theirs.
            if (d->flight_count && now - d->flight_sent > 0.1) send_flight(d, now);
            continue;
        }
        if (seq > d->seq_in || length + 12 > sizeof d->message || offset != d->message_have
            || offset + fragment > length) {
            continue; // Out of order: it comes again
        }
        if (offset == 0) {
            d->message[0] = (uint8_t)type;
            rtc_put24(d->message + 1, length);
            rtc_put16(d->message + 4, seq);
            rtc_put24(d->message + 6, 0);
            rtc_put24(d->message + 9, length);
        }
        memcpy(d->message + 12 + offset, data, fragment);
        d->message_have += fragment;
        if (d->message_have < length) continue;

        // Whole: the ClientHello can set where their numbering starts
        d->message_have = 0;
        d->seq_in = seq + 1;
        uint8_t message[sizeof d->message];
        memcpy(message, d->message, 12 + length);
        in body = {message + 12, length, 0, false};
        rtc_debug("dtls: their handshake message %u (%u bytes)", (unsigned)type, (unsigned)length);
        const bool ok = d->client ? client_message(d, (uint8_t)type, &body, message, 12 + length, now)
                                  : server_message(d, (uint8_t)type, &body, message, 12 + length, now);
        if (!ok) rtc_debug("dtls: refused their handshake message %u", (unsigned)type);
        if (!ok) fail(d);
    }
}

void rtc_dtls_receive(rtc_dtls *d, const uint8_t *datagram, const size_t size, const double now)
{
    size_t at = 0;
    while (at + 13 <= size && d->state != RTC_DTLS_FAILED && d->state != RTC_DTLS_CLOSED) {
        const uint8_t *header = datagram + at;
        const uint8_t type = header[0];
        const uint16_t epoch = rtc_get16(header + 3);
        const uint64_t seq = (uint64_t)rtc_get16(header + 5) << 32 | rtc_get32(header + 7);
        const size_t length = rtc_get16(header + 11);
        if (at + 13 + length > size) return;
        const uint8_t *body = header + 13;
        at += 13 + length;
        if (header[1] != 0xfe) continue; // DTLS; before the version's agreed, 1.0 is fine too

        uint8_t opened[2048];
        size_t n = length;
        if (epoch == 1) {
            // Sealed: only once there are keys, and never the same record twice
            if (!d->keys) continue;
            if (length < 16 || length - 16 > sizeof opened) continue;
            if (seq + 64 <= d->replay_top || (seq <= d->replay_top && d->replay_seen >> (d->replay_top - seq) & 1u)) {
                continue;
            }
            uint8_t nonce[12], ad[13];
            record_nonce(d->read_iv, 1, seq, nonce);
            record_ad(header, length - 16, ad);
            if (!rtc_open(d->read_key, nonce, ad, 13, body, length, opened)) {
                rtc_debug("dtls: a sealed record didn't open");
                continue;
            }
            if (seq > d->replay_top) {
                d->replay_seen = seq - d->replay_top >= 64 ? 1u : d->replay_seen << (seq - d->replay_top) | 1u;
                d->replay_top = seq;
            } else {
                d->replay_seen |= 1ull << (d->replay_top - seq);
            }
            body = opened;
            n = length - 16;
        } else if (epoch != 0) {
            continue;
        }

        switch (type) {
        case HANDSHAKE:
            if (d->state == RTC_DTLS_HANDSHAKE) handshake_record(d, body, n, now);
            else if (d->flight_count && d->flight_done && now - d->flight_sent > 0.1) send_flight(d, now); // Our last flight, lost
            break;
        case CHANGE_CIPHER_SPEC: d->changed_cipher = true; break;
        case ALERT:
            rtc_debug("dtls: their alert %u %u", n >= 2 ? (unsigned)body[0] : 0u, n >= 2 ? (unsigned)body[1] : 0u);
            if (n >= 2 && body[1] == 0) d->state = RTC_DTLS_CLOSED; // close_notify
            else if (n >= 2 && body[0] == 2) fail(d);
            break;
        case APPLICATION_DATA:
            if (epoch == 1 && d->state == RTC_DTLS_OPEN) d->data(d->user, body, n);
            break;
        default: break;
        }
    }
}

void rtc_dtls_update(rtc_dtls *d, const double now)
{
    if (d->state != RTC_DTLS_HANDSHAKE) return;
    if (now - d->started > HANDSHAKE_TIMEOUT) {
        fail(d);
        return;
    }
    if (d->flight_count && !d->flight_done && now - d->flight_sent >= d->flight_timeout) {
        send_flight(d, now);
        d->flight_timeout = d->flight_timeout * 2 < 4.0 ? d->flight_timeout * 2 : 4.0;
    }
}

void rtc_dtls_send(rtc_dtls *d, const void *data, const size_t size)
{
    if (d->state != RTC_DTLS_OPEN) return;
    uint8_t buffer[DATAGRAM + 200];
    out o = {buffer, 0, sizeof buffer, false};
    put_record(d, &o, APPLICATION_DATA, 1, data, size);
    if (!o.overflow) d->send(d->user, buffer, o.size);
}

void rtc_dtls_close(rtc_dtls *d)
{
    if (d->state == RTC_DTLS_OPEN) {
        static const uint8_t close_notify[] = {1, 0};
        uint8_t buffer[64];
        out o = {buffer, 0, sizeof buffer, false};
        put_record(d, &o, ALERT, 1, close_notify, sizeof close_notify);
        if (!o.overflow) d->send(d->user, buffer, o.size);
    }
    d->state = RTC_DTLS_CLOSED;
}
