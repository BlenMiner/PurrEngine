// Certificates for DTLS (see rtc.h): WebRTC doesn't trust them the way HTTPS
// does. Each side makes its own, self-signed, and says the SHA-256 of it (its
// fingerprint) in its description, which goes through the relay. The other
// side then only checks the certificate it gets has that fingerprint, and
// uses its key.
//
// So this writes the one kind of certificate DTLS needs (P-256, ECDSA with
// SHA-256) and reads only what DTLS uses of the other's: its key. DER, as
// X.509 (RFC 5280) and ECDSA signatures in TLS (RFC 4492) encode them.

#include "rtc.h"

#include <string.h>

// A DER writer: tag, length, contents, into a fixed buffer.
typedef struct der {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool overflow;
} der;

static void put_bytes(der *d, const void *p, const size_t n)
{
    if (d->size + n > d->capacity) {
        d->overflow = true;
        return;
    }
    memcpy(d->data + d->size, p, n);
    d->size += n;
}

static void put_byte(der *d, const uint8_t b)
{
    put_bytes(d, &b, 1);
}

static void put_length(der *d, const size_t n)
{
    if (n < 128) {
        put_byte(d, (uint8_t)n);
    } else if (n < 256) {
        put_byte(d, 0x81);
        put_byte(d, (uint8_t)n);
    } else {
        put_byte(d, 0x82);
        put_byte(d, (uint8_t)(n >> 8));
        put_byte(d, (uint8_t)n);
    }
}

static void put_tlv(der *d, const uint8_t tag, const void *contents, const size_t n)
{
    put_byte(d, tag);
    put_length(d, n);
    put_bytes(d, contents, n);
}

// An INTEGER from an unsigned big-endian number: no leading zeros, but one
// where the top bit is set, so it stays positive.
static void put_unsigned(der *d, const uint8_t *value, size_t n)
{
    while (n > 1 && value[0] == 0) {
        value++;
        n--;
    }
    const bool pad = value[0] & 0x80;
    put_byte(d, 0x02);
    put_length(d, n + pad);
    if (pad) put_byte(d, 0);
    put_bytes(d, value, n);
}

size_t rtc_sig_to_der(const uint8_t sig[64], uint8_t out[72])
{
    uint8_t body[72];
    der inner = {body, 0, sizeof body, false};
    put_unsigned(&inner, sig, 32);
    put_unsigned(&inner, sig + 32, 32);
    der d = {out, 0, 72, false};
    put_tlv(&d, 0x30, body, inner.size);
    return d.overflow ? 0 : d.size;
}

// ---------------------------------------------------------------------------
// Reading DER: one element at a time.

typedef struct element {
    uint8_t tag;
    const uint8_t *contents;
    size_t size;
} element;

// The element at *p, moving past it. False if it runs past `end`.
static bool next_element(const uint8_t **p, const uint8_t *end, element *e)
{
    if (end - *p < 2) return false;
    e->tag = (*p)[0];
    size_t n = (*p)[1];
    const uint8_t *at = *p + 2;
    if (n & 0x80) {
        const size_t bytes = n & 0x7f;
        if (bytes == 0 || bytes > 2 || (size_t)(end - at) < bytes) return false;
        n = 0;
        for (size_t i = 0; i < bytes; i++) n = n << 8 | at[i];
        at += bytes;
    }
    if ((size_t)(end - at) < n) return false;
    e->contents = at;
    e->size = n;
    *p = at + n;
    return true;
}

// An INTEGER, as 32 bytes: false if it's negative or bigger.
static bool get_unsigned(const element *e, uint8_t out[32])
{
    const uint8_t *v = e->contents;
    size_t n = e->size;
    if (e->tag != 0x02 || n == 0 || v[0] & 0x80) return false;
    while (n > 1 && v[0] == 0) {
        v++;
        n--;
    }
    if (n > 32) return false;
    memset(out, 0, 32);
    memcpy(out + 32 - n, v, n);
    return true;
}

bool rtc_sig_from_der(const uint8_t *data, const size_t size, uint8_t sig[64])
{
    const uint8_t *p = data;
    element seq, r, s;
    if (!next_element(&p, data + size, &seq) || seq.tag != 0x30) return false;
    const uint8_t *q = seq.contents;
    const uint8_t *end = seq.contents + seq.size;
    return next_element(&q, end, &r) && next_element(&q, end, &s) && q == end && get_unsigned(&r, sig)
        && get_unsigned(&s, sig + 32);
}

// id-ecPublicKey and prime256v1, and ecdsa-with-SHA256
static const uint8_t EC_KEY_OID[] = {0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01};
static const uint8_t P256_OID[] = {0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07};
static const uint8_t ECDSA_SHA256[] = {0x30, 0x0a, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x04, 0x03, 0x02};

bool rtc_cert_key(const uint8_t *cert, const size_t size, uint8_t key[65])
{
    // Certificate: SEQUENCE { tbsCertificate, signatureAlgorithm, signature }
    const uint8_t *p = cert;
    element c, tbs, e;
    if (!next_element(&p, cert + size, &c) || c.tag != 0x30) return false;
    p = c.contents;
    if (!next_element(&p, c.contents + c.size, &tbs) || tbs.tag != 0x30) return false;
    // tbsCertificate: [0] version (optional), serial, signature, issuer,
    // validity, subject, then subjectPublicKeyInfo
    p = tbs.contents;
    const uint8_t *end = tbs.contents + tbs.size;
    if (!next_element(&p, end, &e)) return false;
    if (e.tag == 0xa0 && !next_element(&p, end, &e)) return false; // Past the version, to the serial
    for (int i = 0; i < 4; i++) {
        if (!next_element(&p, end, &e)) return false;
    }
    element spki, algorithm, bits;
    if (!next_element(&p, end, &spki) || spki.tag != 0x30) return false;
    p = spki.contents;
    end = spki.contents + spki.size;
    if (!next_element(&p, end, &algorithm) || !next_element(&p, end, &bits) || bits.tag != 0x03) return false;
    if (algorithm.size != sizeof EC_KEY_OID + sizeof P256_OID
        || memcmp(algorithm.contents, EC_KEY_OID, sizeof EC_KEY_OID) != 0
        || memcmp(algorithm.contents + sizeof EC_KEY_OID, P256_OID, sizeof P256_OID) != 0) {
        return false;
    }
    // BIT STRING: no unused bits, then the point
    if (bits.size != 66 || bits.contents[0] != 0 || bits.contents[1] != 4) return false;
    memcpy(key, bits.contents + 1, 65);
    return true;
}

// ---------------------------------------------------------------------------

bool rtc_identity_new(rtc_identity *id)
{
    if (!rtc_p256_keys(id->private_key, id->public_key)) return false;

    // CN=purr, for issuer and subject alike
    static const uint8_t name[] = {0x30, 0x0f, 0x31, 0x0d, 0x30, 0x0b, 0x06, 0x03, 0x55,
                                   0x04, 0x03, 0x0c, 0x04, 'p',  'u',  'r',  'r'};
    // Validity: WebRTC doesn't look, but it has to be there
    static const uint8_t validity[] = {0x30, 0x1e, 0x17, 0x0d, '2', '5', '0', '1', '0', '1', '0', '0', '0', '0', '0', '0',
                                       'Z',  0x17, 0x0d, '4',  '9', '1', '2', '3', '1', '2', '3', '5', '9', '5', '9', 'Z'};
    _Static_assert(sizeof validity == 32, "two UTCTimes, YYMMDDHHMMSSZ");
    uint8_t serial[8];
    if (!rtc_random(serial, sizeof serial)) return false;
    serial[0] &= 0x7f;
    serial[0] |= 0x01; // Positive, and no leading zero

    uint8_t body[400];
    der tbs = {body, 0, sizeof body, false};
    static const uint8_t version[] = {0xa0, 0x03, 0x02, 0x01, 0x02}; // v3
    put_bytes(&tbs, version, sizeof version);
    put_tlv(&tbs, 0x02, serial, sizeof serial);
    put_bytes(&tbs, ECDSA_SHA256, sizeof ECDSA_SHA256);
    put_bytes(&tbs, name, sizeof name);
    put_bytes(&tbs, validity, sizeof validity);
    put_bytes(&tbs, name, sizeof name);
    uint8_t spki[91];
    der key = {spki, 0, sizeof spki, false};
    uint8_t algorithm[sizeof EC_KEY_OID + sizeof P256_OID];
    memcpy(algorithm, EC_KEY_OID, sizeof EC_KEY_OID);
    memcpy(algorithm + sizeof EC_KEY_OID, P256_OID, sizeof P256_OID);
    put_tlv(&key, 0x30, algorithm, sizeof algorithm);
    uint8_t point[66] = {0};
    memcpy(point + 1, id->public_key, 65);
    put_tlv(&key, 0x03, point, sizeof point);
    put_tlv(&tbs, 0x30, spki, key.size);

    uint8_t tbs_der[420];
    der whole_tbs = {tbs_der, 0, sizeof tbs_der, false};
    put_tlv(&whole_tbs, 0x30, body, tbs.size);

    uint8_t hash[32], sig[64], sig_der[72];
    rtc_sha256_of(tbs_der, whole_tbs.size, hash);
    if (!rtc_p256_sign(id->private_key, hash, sig)) return false;
    const size_t sig_size = rtc_sig_to_der(sig, sig_der);
    uint8_t sig_bits[73];
    sig_bits[0] = 0;
    memcpy(sig_bits + 1, sig_der, sig_size);

    uint8_t all[512];
    der cert_body = {all, 0, sizeof all, false};
    put_bytes(&cert_body, tbs_der, whole_tbs.size);
    put_bytes(&cert_body, ECDSA_SHA256, sizeof ECDSA_SHA256);
    put_tlv(&cert_body, 0x03, sig_bits, sig_size + 1);
    der cert = {id->cert, 0, sizeof id->cert, false};
    put_tlv(&cert, 0x30, all, cert_body.size);
    if (tbs.overflow || key.overflow || whole_tbs.overflow || cert_body.overflow || cert.overflow || !sig_size) {
        return false;
    }
    id->cert_size = cert.size;
    rtc_sha256_of(id->cert, id->cert_size, id->fingerprint);
    return true;
}
