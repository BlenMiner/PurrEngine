// Android apps' key and signatures (see sign.h), in DER as X.509 (RFC 5280),
// PKCS #8 (RFC 5208), PKCS #1 (RFC 8017) and PKCS #7 (RFC 2315) have them,
// and JAR signing as Java's JAR File Specification describes it.

#define _POSIX_C_SOURCE 200809L // chmod

#include "sign.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "rtc.h"

// ---------------------------------------------------------------------------
// Bytes

typedef struct buf {
    uint8_t *data;
    size_t size, capacity;
} buf;

static void put(buf *b, const void *p, const size_t n)
{
    if (b->size + n > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 1024;
        while (capacity < b->size + n) capacity *= 2;
        uint8_t *grown = realloc(b->data, capacity);
        if (!grown) {
            fprintf(stderr, "tide: out of memory\n");
            exit(1);
        }
        b->data = grown;
        b->capacity = capacity;
    }
    if (n) memcpy(b->data + b->size, p, n);
    b->size += n;
}

static void put8(buf *b, const uint8_t v)
{
    put(b, &v, 1);
}

static void put_text(buf *b, const char *s)
{
    put(b, s, strlen(s));
}

// ---------------------------------------------------------------------------
// DER: a tag, its contents' length, its contents

static void tlv(buf *out, const uint8_t tag, const void *contents, const size_t n)
{
    put8(out, tag);
    if (n < 0x80) {
        put8(out, (uint8_t)n);
    } else {
        int bytes = 1;
        while (bytes < 4 && n >> (8 * bytes)) bytes++;
        put8(out, (uint8_t)(0x80 | bytes));
        for (int i = bytes - 1; i >= 0; i--) put8(out, (uint8_t)(n >> (8 * i)));
    }
    put(out, contents, n);
}

// A constructed element of what `inner` holds, which it frees
static void wrap(buf *out, const uint8_t tag, buf *inner)
{
    tlv(out, tag, inner->data, inner->size);
    free(inner->data);
    *inner = (buf){0};
}

// An INTEGER of an unsigned number: no leading zeros, but one where the top
// bit is set, so it stays positive.
static void put_unsigned(buf *out, const uint8_t *value, size_t n)
{
    while (n > 1 && value[0] == 0) {
        value++;
        n--;
    }
    buf v = {0};
    if (n == 0 || value[0] & 0x80) put8(&v, 0);
    put(&v, value, n);
    wrap(out, 0x02, &v);
}

static void put_number(buf *out, const rsa_number *x)
{
    put_unsigned(out, x->bytes, x->size);
}

static void put_small(buf *out, const uint8_t v)
{
    const uint8_t value[3] = {0x02, 0x01, v};
    put(out, value, sizeof value);
}

// Algorithms, with their NULL parameters
static const uint8_t RSA_ENCRYPTION[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
                                         0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};
static const uint8_t SHA256_WITH_RSA[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7,
                                          0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00};
static const uint8_t SHA256[] = {0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00};
// PKCS #7's content types: data and signedData
static const uint8_t PKCS7_DATA[] = {0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x01};
static const uint8_t PKCS7_SIGNED[] = {0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x07, 0x02};

typedef struct element {
    uint8_t tag;
    const uint8_t *start; // The whole element, tag and all
    const uint8_t *contents;
    size_t size, whole;
} element;

// The element at *p, moving past it. False if it runs past `end`.
static bool next(const uint8_t **p, const uint8_t *end, element *e)
{
    if (end - *p < 2) return false;
    e->start = *p;
    e->tag = (*p)[0];
    size_t n = (*p)[1];
    const uint8_t *at = *p + 2;
    if (n & 0x80) {
        const size_t bytes = n & 0x7f;
        if (bytes == 0 || bytes > 4 || (size_t)(end - at) < bytes) return false;
        n = 0;
        for (size_t i = 0; i < bytes; i++) n = n << 8 | at[i];
        at += bytes;
    }
    if ((size_t)(end - at) < n) return false;
    e->contents = at;
    e->size = n;
    e->whole = (size_t)(at + n - e->start);
    *p = at + n;
    return true;
}

// The elements inside a constructed one: `inside` walks them
static bool enter(const element *e, const uint8_t tag, const uint8_t **inside, const uint8_t **end)
{
    if (e->tag != tag) return false;
    *inside = e->contents;
    *end = e->contents + e->size;
    return true;
}

// An INTEGER, unsigned: false if it's negative or too big
static bool get_number(const element *e, rsa_number *x)
{
    const uint8_t *v = e->contents;
    size_t n = e->size;
    if (e->tag != 0x02 || n == 0 || v[0] & 0x80) return false;
    while (n > 1 && v[0] == 0) {
        v++;
        n--;
    }
    if (n > RSA_MAX_BYTES) return false;
    x->size = n;
    memcpy(x->bytes, v, n);
    return true;
}

static bool same_number(const rsa_number *a, const rsa_number *b)
{
    const uint8_t *x = a->bytes, *y = b->bytes;
    size_t n = a->size, m = b->size;
    while (n > 1 && *x == 0) {
        x++;
        n--;
    }
    while (m > 1 && *y == 0) {
        y++;
        m--;
    }
    return n == m && memcmp(x, y, n) == 0;
}

// ---------------------------------------------------------------------------
// Base64 and PEM

static const char BASE64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t sign_base64(const uint8_t *in, const size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = BASE64[v >> 18];
        out[o++] = BASE64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? BASE64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? BASE64[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

// The bytes of the PEM block labeled `label` in `text`, or false
static bool pem_block(const char *text, const size_t size, const char *label, buf *out)
{
    char begin[64], end[64];
    snprintf(begin, sizeof begin, "-----BEGIN %s-----", label);
    snprintf(end, sizeof end, "-----END %s-----", label);
    const size_t begin_size = strlen(begin);
    const char *at = NULL;
    for (size_t i = 0; i + begin_size <= size; i++) {
        if (memcmp(text + i, begin, begin_size) == 0 && (i == 0 || text[i - 1] == '\n')) {
            at = text + i + begin_size;
            break;
        }
    }
    if (!at) return false;
    uint32_t bits = 0;
    int count = 0;
    for (const char *c = at; c < text + size; c++) {
        if (*c == '-') return (size_t)(text + size - c) >= strlen(end) && memcmp(c, end, strlen(end)) == 0;
        if (*c == '=' || *c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') continue;
        const char *found = strchr(BASE64, *c);
        if (!found || !*c) return false;
        bits = bits << 6 | (uint32_t)(found - BASE64);
        count += 6;
        if (count >= 8) {
            count -= 8;
            put8(out, (uint8_t)(bits >> count));
        }
    }
    return false;
}

static void put_pem(buf *out, const char *label, const uint8_t *data, const size_t size)
{
    char line[80];
    snprintf(line, sizeof line, "-----BEGIN %s-----\n", label);
    put_text(out, line);
    for (size_t i = 0; i < size; i += 48) {
        char text[72];
        sign_base64(data + i, size - i < 48 ? size - i : 48, text);
        put_text(out, text);
        put8(out, '\n');
    }
    snprintf(line, sizeof line, "-----END %s-----\n", label);
    put_text(out, line);
}

// ---------------------------------------------------------------------------
// The key and its certificate

static bool fail(char *error, const size_t error_size, const char *message, const char *what)
{
    snprintf(error, error_size, "%s%s", message, what ? what : "");
    return false;
}

// RSAPrivateKey: SEQUENCE { version, n, e, d, p, q, dp, dq, qinv }
static bool read_rsa_private_key(const uint8_t *data, const size_t size, rsa_key *key)
{
    const uint8_t *p = data, *inside, *end;
    element seq, e;
    if (!next(&p, data + size, &seq) || !enter(&seq, 0x30, &inside, &end)) return false;
    if (!next(&inside, end, &e) || e.tag != 0x02) return false; // The version
    rsa_number *const numbers[] = {&key->n, &key->e, &key->d, &key->p, &key->q, &key->dp, &key->dq, &key->qinv};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; i++) {
        if (!next(&inside, end, &e) || !get_number(&e, numbers[i])) return false;
    }
    return true;
}

static void write_rsa_private_key(buf *out, const rsa_key *key)
{
    buf seq = {0};
    put_small(&seq, 0);
    const rsa_number *const numbers[] = {&key->n, &key->e, &key->d, &key->p, &key->q, &key->dp, &key->dq, &key->qinv};
    for (size_t i = 0; i < sizeof numbers / sizeof numbers[0]; i++) put_number(&seq, numbers[i]);
    wrap(out, 0x30, &seq);
}

// What tide reads of a certificate: its serial, issuer and key
typedef struct cert_parts {
    element serial, issuer, spki;
    rsa_number n, e;
} cert_parts;

static bool read_cert(const uint8_t *cert, const size_t size, cert_parts *c)
{
    // Certificate: SEQUENCE { tbsCertificate, signatureAlgorithm, signature };
    // tbsCertificate: [0] version (optional), serial, signature, issuer,
    // validity, subject, subjectPublicKeyInfo, ...
    const uint8_t *p = cert, *inside, *end, *tbs_inside, *tbs_end;
    element whole, tbs, e, validity, subject;
    if (!next(&p, cert + size, &whole) || !enter(&whole, 0x30, &inside, &end)) return false;
    if (!next(&inside, end, &tbs) || !enter(&tbs, 0x30, &tbs_inside, &tbs_end)) return false;
    if (!next(&tbs_inside, tbs_end, &c->serial)) return false;
    if (c->serial.tag == 0xa0 && !next(&tbs_inside, tbs_end, &c->serial)) return false;
    if (c->serial.tag != 0x02 || !next(&tbs_inside, tbs_end, &e) || !next(&tbs_inside, tbs_end, &c->issuer)
        || !next(&tbs_inside, tbs_end, &validity) || !next(&tbs_inside, tbs_end, &subject)
        || !next(&tbs_inside, tbs_end, &c->spki)) {
        return false;
    }
    // SubjectPublicKeyInfo: SEQUENCE { algorithm, BIT STRING { RSAPublicKey } }
    const uint8_t *spki_inside, *spki_end, *key_inside, *key_end;
    element algorithm, bits, key, n, exponent;
    if (!enter(&c->spki, 0x30, &spki_inside, &spki_end) || !next(&spki_inside, spki_end, &algorithm)
        || algorithm.whole != sizeof RSA_ENCRYPTION || memcmp(algorithm.start, RSA_ENCRYPTION, sizeof RSA_ENCRYPTION) != 0
        || !next(&spki_inside, spki_end, &bits) || bits.tag != 0x03 || bits.size < 1 || bits.contents[0] != 0) {
        return false;
    }
    const uint8_t *k = bits.contents + 1;
    return next(&k, bits.contents + bits.size, &key) && enter(&key, 0x30, &key_inside, &key_end)
        && next(&key_inside, key_end, &n) && get_number(&n, &c->n) && next(&key_inside, key_end, &exponent)
        && get_number(&exponent, &c->e);
}

// A certificate for the key, self-signed: CN=Tide app key, from now on, with
// no end (RFC 5280's 99991231235959Z), as Play wants keys to outlast 2033.
static bool make_cert(apk_key *key)
{
    static const uint8_t name[] = {0x30, 0x17, 0x31, 0x15, 0x30, 0x13, 0x06, 0x03, 0x55, 0x04, 0x03, 0x0c, 0x0c,
                                   'T',  'i',  'd',  'e',  ' ',  'a',  'p',  'p',  ' ',  'k',  'e',  'y'};
    const time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    char from[16];
    snprintf(from, sizeof from, "%02d%02d%02d%02d%02d%02dZ", t.tm_year % 100, t.tm_mon + 1, t.tm_mday, t.tm_hour,
             t.tm_min, t.tm_sec);
    buf validity = {0};
    tlv(&validity, 0x17, from, 13);            // UTCTime
    tlv(&validity, 0x18, "99991231235959Z", 15); // GeneralizedTime
    uint8_t serial[16];
    if (!rtc_random(serial, sizeof serial)) return false;
    serial[0] = (uint8_t)((serial[0] & 0x7f) | 0x01); // Positive, and no leading zero

    buf public_key = {0}, rsa = {0}, bits = {0}, spki = {0}, tbs = {0}, version = {0};
    put_number(&rsa, &key->rsa.n);
    put_number(&rsa, &key->rsa.e);
    put8(&bits, 0); // No unused bits
    wrap(&bits, 0x30, &rsa);
    put(&public_key, RSA_ENCRYPTION, sizeof RSA_ENCRYPTION);
    wrap(&public_key, 0x03, &bits);
    wrap(&spki, 0x30, &public_key);

    put_small(&version, 2); // v3
    wrap(&tbs, 0xa0, &version);
    put_unsigned(&tbs, serial, sizeof serial);
    put(&tbs, SHA256_WITH_RSA, sizeof SHA256_WITH_RSA);
    put(&tbs, name, sizeof name);
    wrap(&tbs, 0x30, &validity);
    put(&tbs, name, sizeof name);
    put(&tbs, spki.data, spki.size);
    free(spki.data);
    buf tbs_der = {0};
    wrap(&tbs_der, 0x30, &tbs);

    uint8_t sig[RSA_MAX_BYTES];
    const size_t sig_size = apk_key_sign(key, tbs_der.data, tbs_der.size, sig);
    buf cert = {0}, sig_bits = {0}, der = {0};
    put(&cert, tbs_der.data, tbs_der.size);
    free(tbs_der.data);
    put(&cert, SHA256_WITH_RSA, sizeof SHA256_WITH_RSA);
    put8(&sig_bits, 0);
    put(&sig_bits, sig, sig_size);
    wrap(&cert, 0x03, &sig_bits);
    wrap(&der, 0x30, &cert);
    const bool ok = sig_size && der.size <= sizeof key->cert;
    if (ok) {
        memcpy(key->cert, der.data, der.size);
        key->cert_size = der.size;
    }
    free(der.data);
    return ok;
}

bool apk_key_parse(const char *pem, const size_t size, apk_key *key, char *error, const size_t error_size)
{
    memset(key, 0, sizeof *key);
    buf der = {0}, cert = {0};
    bool ok = true;
    if (pem_block(pem, size, "PRIVATE KEY", &der)) {
        // PrivateKeyInfo: SEQUENCE { version, algorithm, OCTET STRING { RSAPrivateKey } }
        const uint8_t *p = der.data, *inside, *end;
        element info, version, algorithm, octets;
        ok = next(&p, der.data + der.size, &info) && enter(&info, 0x30, &inside, &end) && next(&inside, end, &version)
          && next(&inside, end, &algorithm);
        if (ok && (algorithm.whole != sizeof RSA_ENCRYPTION
                   || memcmp(algorithm.start, RSA_ENCRYPTION, sizeof RSA_ENCRYPTION) != 0)) {
            free(der.data);
            return fail(error, error_size, "the key isn't RSA's, which Google Play takes keys of", NULL);
        }
        ok = ok && next(&inside, end, &octets) && octets.tag == 0x04
          && read_rsa_private_key(octets.contents, octets.size, &key->rsa);
    } else if (pem_block(pem, size, "RSA PRIVATE KEY", &der)) {
        ok = read_rsa_private_key(der.data, der.size, &key->rsa);
    } else {
        const bool other = strstr(pem, "PRIVATE KEY-----") != NULL;
        return fail(error, error_size,
                    other ? "the key is encrypted, or isn't RSA's: give tide one of RSA, unencrypted (openssl pkey "
                            "-in key.pem -out plain.pem)"
                          : "there's no private key in it",
                    NULL);
    }
    free(der.data);
    if (!ok || !rsa_check(&key->rsa)) return fail(error, error_size, "its private key is broken", NULL);
    if (rsa_size(&key->rsa.n) < 256) return fail(error, error_size, "its key is under 2048 bits, which Google Play won't take", NULL);
    cert_parts c;
    if (!pem_block(pem, size, "CERTIFICATE", &cert) || cert.size > sizeof key->cert) {
        free(cert.data);
        return fail(error, error_size, "there's no certificate in it, after its key", NULL);
    }
    memcpy(key->cert, cert.data, cert.size);
    key->cert_size = cert.size;
    free(cert.data);
    if (!read_cert(key->cert, key->cert_size, &c) || !same_number(&c.n, &key->rsa.n) || !same_number(&c.e, &key->rsa.e)) {
        return fail(error, error_size, "its certificate isn't its key's", NULL);
    }
    return true;
}

char *apk_key_pem(const apk_key *key)
{
    buf rsa = {0}, info = {0}, der = {0}, out = {0};
    write_rsa_private_key(&rsa, &key->rsa);
    put_small(&info, 0);
    put(&info, RSA_ENCRYPTION, sizeof RSA_ENCRYPTION);
    wrap(&info, 0x04, &rsa);
    wrap(&der, 0x30, &info);
    put_text(&out, "The key tide signs your Android apps with, and its certificate. Keep a copy somewhere safe:\n"
                   "an app's updates have to be signed with the key the app was, and on Google Play this is your\n"
                   "upload key. Don't share it: whoever has it can sign apps as you.\n\n");
    put_pem(&out, "PRIVATE KEY", der.data, der.size);
    put_pem(&out, "CERTIFICATE", key->cert, key->cert_size);
    put8(&out, 0);
    free(der.data);
    return (char *)out.data;
}

bool apk_key_load(const char *path, apk_key *key, bool *made, char *error, const size_t error_size)
{
    if (made) *made = false;
    FILE *f = fopen(path, "rb");
    if (f) {
        buf text = {0};
        char chunk[4096];
        size_t n;
        while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) put(&text, chunk, n);
        fclose(f);
        put8(&text, 0); // For strstr
        char why[256];
        const bool ok = apk_key_parse((const char *)text.data, text.size - 1, key, why, sizeof why);
        free(text.data);
        if (!ok) snprintf(error, error_size, "the key at %s can't sign apps: %s", path, why);
        return ok;
    }

    memset(key, 0, sizeof *key);
    if (!rsa_generate(&key->rsa, 2048) || !make_cert(key)) return fail(error, error_size, "can't make a key for the app", NULL);
    char *pem = apk_key_pem(key);
    // Written whole, under another name, then renamed: a key half written
    // would sign nothing anyone could update. Only its owner can read it.
    char partial[1024];
    snprintf(partial, sizeof partial, "%s.partial", path);
    f = fopen(partial, "wb");
    const size_t size = strlen(pem);
    bool ok = f && fwrite(pem, 1, size, f) == size;
    if (f && fclose(f) != 0) ok = false;
    free(pem);
#ifndef _WIN32
    if (ok) chmod(partial, 0600);
#endif
    if (!ok || rename(partial, path) != 0) {
        remove(partial);
        return fail(error, error_size, "can't write the app's key to ", path);
    }
    if (made) *made = true;
    return true;
}

bool apk_key_public_info(const apk_key *key, const uint8_t **info, size_t *size)
{
    cert_parts c;
    if (!read_cert(key->cert, key->cert_size, &c)) return false;
    *info = c.spki.start;
    *size = c.spki.whole;
    return true;
}

size_t apk_key_sign(const apk_key *key, const void *data, const size_t size, uint8_t *sig)
{
    uint8_t hash[32];
    rtc_sha256_of(data, size, hash);
    return rsa_sign(&key->rsa, hash, sig) ? rsa_size(&key->rsa.n) : 0;
}

// ---------------------------------------------------------------------------
// JAR signing

// A header line, "Name: value", wrapped at 72 bytes: each line after the
// first starts with a space.
static void put_header(buf *out, const char *name, const char *value)
{
    buf line = {0};
    put_text(&line, name);
    put_text(&line, ": ");
    put_text(&line, value);
    for (size_t at = 0; at < line.size;) {
        const size_t room = at == 0 ? 72 : 71;
        const size_t n = line.size - at < room ? line.size - at : room;
        if (at) put8(out, ' ');
        put(out, line.data + at, n);
        put_text(out, "\r\n");
        at += n;
    }
    free(line.data);
}

static void put_digest(buf *out, const char *name, const void *data, const size_t size)
{
    uint8_t hash[32];
    char text[48];
    rtc_sha256_of(data, size, hash);
    sign_base64(hash, sizeof hash, text);
    put_header(out, name, text);
}

bool jar_sign(const jar_entry *entries, const int count, const apk_key *key, jar_signature *out)
{
    *out = (jar_signature){0};
    // The manifest: its main section, then a section for each entry
    buf manifest = {0};
    put_header(&manifest, "Manifest-Version", "1.0");
    put_header(&manifest, "Created-By", "tide");
    put_text(&manifest, "\r\n");
    const size_t main_size = manifest.size;
    size_t *starts = malloc(sizeof(size_t) * (size_t)(count + 1));
    if (!starts) return false;
    for (int i = 0; i < count; i++) {
        starts[i] = manifest.size;
        put_header(&manifest, "Name", entries[i].name);
        put_digest(&manifest, "SHA-256-Digest", entries[i].data, entries[i].size);
        put_text(&manifest, "\r\n");
    }
    starts[count] = manifest.size;

    // The signature file: the manifest's digest, its main section's, and each
    // entry's section's
    buf sf = {0};
    put_header(&sf, "Signature-Version", "1.0");
    put_header(&sf, "Created-By", "tide");
    put_digest(&sf, "SHA-256-Digest-Manifest", manifest.data, manifest.size);
    put_digest(&sf, "SHA-256-Digest-Manifest-Main-Attributes", manifest.data, main_size);
    put_text(&sf, "\r\n");
    for (int i = 0; i < count; i++) {
        put_header(&sf, "Name", entries[i].name);
        put_digest(&sf, "SHA-256-Digest", manifest.data + starts[i], starts[i + 1] - starts[i]);
        put_text(&sf, "\r\n");
    }
    free(starts);

    // Its signature: PKCS #7 SignedData, detached, with the certificate and
    // one signer, who signed the file itself (no attributes)
    uint8_t sig[RSA_MAX_BYTES];
    const size_t sig_size = apk_key_sign(key, sf.data, sf.size, sig);
    cert_parts c;
    if (!sig_size || !read_cert(key->cert, key->cert_size, &c)) {
        free(manifest.data);
        free(sf.data);
        return false;
    }
    buf algorithms = {0}, content = {0}, certs = {0}, signer = {0}, issuer_serial = {0}, signers = {0};
    put(&algorithms, SHA256, sizeof SHA256);
    put(&content, PKCS7_DATA, sizeof PKCS7_DATA);
    put(&certs, key->cert, key->cert_size);
    put_small(&signer, 1);
    put(&issuer_serial, c.issuer.start, c.issuer.whole);
    put(&issuer_serial, c.serial.start, c.serial.whole);
    wrap(&signer, 0x30, &issuer_serial);
    put(&signer, SHA256, sizeof SHA256);
    put(&signer, RSA_ENCRYPTION, sizeof RSA_ENCRYPTION);
    tlv(&signer, 0x04, sig, sig_size);
    wrap(&signers, 0x30, &signer);

    buf signed_data = {0}, explicit_data = {0}, info = {0}, block = {0};
    put_small(&signed_data, 1);
    wrap(&signed_data, 0x31, &algorithms);
    wrap(&signed_data, 0x30, &content);
    wrap(&signed_data, 0xa0, &certs);
    wrap(&signed_data, 0x31, &signers);
    wrap(&explicit_data, 0x30, &signed_data);
    put(&info, PKCS7_SIGNED, sizeof PKCS7_SIGNED);
    wrap(&info, 0xa0, &explicit_data);
    wrap(&block, 0x30, &info);

    out->manifest = manifest.data;
    out->manifest_size = manifest.size;
    out->signature_file = sf.data;
    out->signature_file_size = sf.size;
    out->block = block.data;
    out->block_size = block.size;
    return true;
}

void jar_signature_free(jar_signature *s)
{
    free(s->manifest);
    free(s->signature_file);
    free(s->block);
    *s = (jar_signature){0};
}
