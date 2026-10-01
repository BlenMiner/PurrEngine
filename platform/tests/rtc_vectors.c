// The WebRTC stack's crypto against Node's, on random inputs: rtc_vectors.mjs
// writes cases with what Node got, this checks each, and signs what it's
// asked to for Node to verify.
//
//     tide_platform_rtc_vectors <cases> <signatures out>
//
// A case is a line: its kind, then its fields in hex ("-" for none):
//
//     sha256|sha1|md5 <data> <digest>
//     hmac256|hmac1 <key> <data> <mac>
//     prf <secret> <label> <seed> <out>
//     seal <key> <nonce> <ad> <text> <sealed>
//     public <private key> <public key>
//     ecdh <private key> <their public key> <secret>
//     verify <public key> <message> <signature> <1 if it's good, 0 if not>
//     sign <private key> <message>    (writes "signed <public key> <message> <signature>")

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc/rtc.h"

static size_t unhex(const char *text, uint8_t *out)
{
    if (strcmp(text, "-") == 0) return 0;
    size_t n = 0;
    for (; text[0] && text[1]; text += 2) {
        unsigned v = 0;
        sscanf(text, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void hex(FILE *f, const uint8_t *data, const size_t size)
{
    if (!size) fputc('-', f);
    for (size_t i = 0; i < size; i++) fprintf(f, "%02x", data[i]);
}

int main(const int argc, char **argv)
{
    if (argc < 3) return fprintf(stderr, "usage: tide_platform_rtc_vectors <cases> <signatures out>\n"), 2;
    FILE *in = fopen(argv[1], "r");
    FILE *out = fopen(argv[2], "w");
    if (!in || !out) return fprintf(stderr, "can't open the files\n"), 2;

    static char line[65536];
    static uint8_t a[8192], b[8192], c[8192], d[8192], e[8192], got[8192];
    int cases = 0, failures = 0;
    while (fgets(line, sizeof line, in)) {
        char *fields[8] = {0};
        int count = 0;
        for (char *t = strtok(line, " \r\n"); t && count < 8; t = strtok(NULL, " \r\n")) fields[count++] = t;
        if (!count) continue;
        const char *kind = fields[0];
        cases++;
        bool ok = true;
        if (strcmp(kind, "sha256") == 0 || strcmp(kind, "sha1") == 0 || strcmp(kind, "md5") == 0) {
            const size_t n = unhex(fields[1], a), want = unhex(fields[2], b);
            if (kind[3] == '2') {
                rtc_sha256_of(a, n, got);
            } else if (kind[0] == 's') {
                rtc_sha1 s;
                rtc_sha1_init(&s);
                rtc_sha1_add(&s, a, n);
                rtc_sha1_end(&s, got);
            } else {
                rtc_md5(a, n, got);
            }
            ok = memcmp(got, b, want) == 0;
        } else if (strcmp(kind, "hmac256") == 0 || strcmp(kind, "hmac1") == 0) {
            const size_t k = unhex(fields[1], a), n = unhex(fields[2], b), want = unhex(fields[3], c);
            if (kind[4] == '2') {
                rtc_hmac256 h;
                rtc_hmac256_init(&h, a, k);
                rtc_hmac256_add(&h, b, n);
                rtc_hmac256_end(&h, got);
            } else {
                rtc_hmac1 h;
                rtc_hmac1_init(&h, a, k);
                rtc_hmac1_add(&h, b, n);
                rtc_hmac1_end(&h, got);
            }
            ok = memcmp(got, c, want) == 0;
        } else if (strcmp(kind, "prf") == 0) {
            const size_t s = unhex(fields[1], a), l = unhex(fields[2], b), n = unhex(fields[3], c), want = unhex(fields[4], d);
            char label[256];
            memcpy(label, b, l);
            label[l] = '\0';
            // The seed in two pieces, split anywhere
            rtc_prf(a, s, label, c, n / 3, c + n / 3, n - n / 3, got, want);
            ok = memcmp(got, d, want) == 0;
        } else if (strcmp(kind, "seal") == 0) {
            unhex(fields[1], a);
            unhex(fields[2], b);
            const size_t ad = unhex(fields[3], c), n = unhex(fields[4], d), want = unhex(fields[5], e);
            rtc_seal(a, b, c, ad, d, n, got);
            ok = want == n + 16 && memcmp(got, e, want) == 0;
            uint8_t opened[8192];
            ok = ok && rtc_open(a, b, c, ad, e, want, opened) && memcmp(opened, d, n) == 0;
            e[want / 2] ^= 0x10; // And tampered with, refused
            ok = ok && !rtc_open(a, b, c, ad, e, want, opened);
        } else if (strcmp(kind, "public") == 0) {
            unhex(fields[1], a);
            unhex(fields[2], b);
            ok = rtc_p256_public(a, got) && memcmp(got, b, 65) == 0;
        } else if (strcmp(kind, "ecdh") == 0) {
            unhex(fields[1], a);
            unhex(fields[2], b);
            unhex(fields[3], c);
            ok = rtc_p256_ecdh(a, b, got) && memcmp(got, c, 32) == 0;
        } else if (strcmp(kind, "verify") == 0) {
            unhex(fields[1], a);
            const size_t n = unhex(fields[2], b);
            unhex(fields[3], c);
            uint8_t hash[32];
            rtc_sha256_of(b, n, hash);
            ok = rtc_p256_verify(a, hash, c) == (strcmp(fields[4], "1") == 0);
        } else if (strcmp(kind, "sign") == 0) {
            unhex(fields[1], a);
            const size_t n = unhex(fields[2], b);
            uint8_t hash[32], sig[64], key[65];
            rtc_sha256_of(b, n, hash);
            ok = rtc_p256_public(a, key) && rtc_p256_sign(a, hash, sig);
            fputs("signed ", out);
            hex(out, key, 65);
            fputc(' ', out);
            hex(out, b, n);
            fputc(' ', out);
            hex(out, sig, 64);
            fputc('\n', out);
        } else {
            ok = false;
        }
        if (!ok) {
            failures++;
            if (failures <= 10) fprintf(stderr, "FAIL: a %s case (%d)\n", kind, cases);
        }
    }
    fclose(in);
    fclose(out);
    printf("%d cases, %d wrong\n", cases, failures);
    return failures ? 1 : 0;
}
