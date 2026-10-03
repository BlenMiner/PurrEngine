#pragma once

// The key tide signs Android apps with, and the signatures it makes with it:
// APK Signature Scheme v2's (apk.c) and JAR signing's, which App Bundles for
// Google Play have. Play takes upload keys of RSA only, 2048 bits or more, so
// that's what tide makes, and one key signs everything: an app and its
// updates have to be signed with the same one.
//
// The key is a PEM file, as OpenSSL writes them: the private key (PKCS #8,
// or PKCS #1's RSA PRIVATE KEY), then its certificate, self-signed. Keys made
// elsewhere work too: `openssl pkcs12 -in upload.p12 -nodes -out key.pem`
// makes one from a keystore's.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rsa.h"

typedef struct apk_key {
    rsa_key rsa;
    uint8_t cert[4096]; // DER
    size_t cert_size;
} apk_key;

// Reads the key at `path`, or makes one and writes it there; `made` says
// which (it may be NULL).
bool apk_key_load(const char *path, apk_key *key, bool *made, char *error, size_t error_size);

// A key from PEM text.
bool apk_key_parse(const char *pem, size_t size, apk_key *key, char *error, size_t error_size);

// The PEM file of a key, to free().
char *apk_key_pem(const apk_key *key);

// The key's SubjectPublicKeyInfo, DER, as its certificate has it.
bool apk_key_public_info(const apk_key *key, const uint8_t **info, size_t *size);

// Signs `data` (RSA, PKCS #1 v1.5 over SHA-256) into `sig`, which has room for
// RSA_MAX_BYTES; returns the signature's size, or 0.
size_t apk_key_sign(const apk_key *key, const void *data, size_t size, uint8_t *sig);

// A file a JAR signature covers
typedef struct jar_entry {
    const char *name;
    const uint8_t *data;
    size_t size;
} jar_entry;

// A JAR's signature: META-INF/MANIFEST.MF, with each entry's SHA-256;
// META-INF/TIDE.SF, the signature file, with the manifest's and each of its
// sections'; and META-INF/TIDE.RSA, the signature file's signature, PKCS #7.
typedef struct jar_signature {
    uint8_t *manifest, *signature_file, *block;
    size_t manifest_size, signature_file_size, block_size;
} jar_signature;

bool jar_sign(const jar_entry *entries, int count, const apk_key *key, jar_signature *out);
void jar_signature_free(jar_signature *s);

// Base64 of `n` bytes into `out`, which has room for 4 for every 3 and a
// zero; returns its length.
size_t sign_base64(const uint8_t *in, size_t n, char *out);
