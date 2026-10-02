// Android apps (apk.c): checked the way Android checks them before it installs
// one. The zip's directory and its entries, then the v2 signature: the
// signing block before the directory, the digest of the rest, and the
// signature over it with the certificate's key.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apk.h"
#include "rtc.h"
#include "tide_test.h"

static uint32_t get16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t get32(const uint8_t *p)
{
    return get16(p) | get16(p + 2) << 16;
}

static uint64_t get64(const uint8_t *p)
{
    return get32(p) | (uint64_t)get32(p + 4) << 32;
}

static uint8_t *read_all(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size);
    if (fread(data, 1, *size, f) != *size) *size = 0;
    fclose(f);
    return data;
}

// A length-prefixed piece of the signature, and what follows it.
static const uint8_t *piece(const uint8_t *p, const uint8_t *end, const uint8_t **next, uint32_t *size)
{
    if (end - p < 4) return NULL;
    *size = get32(p);
    if ((uint64_t)(end - p - 4) < *size) return NULL;
    *next = p + 4 + *size;
    return p + 4;
}

// Android's digest: 1 MiB chunks of each section, each hashed with 0xa5.
static void add_chunks(const uint8_t *data, const size_t size, rtc_sha256 *all, uint32_t *count, uint8_t *hashes)
{
    (void)all;
    for (size_t at = 0; at < size; at += 1u << 20) {
        const size_t n = size - at < (1u << 20) ? size - at : (1u << 20);
        uint8_t prefix[5] = {0xa5, (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)(n >> 16), (uint8_t)(n >> 24)};
        rtc_sha256 s;
        rtc_sha256_init(&s);
        rtc_sha256_add(&s, prefix, 5);
        rtc_sha256_add(&s, data + at, n);
        rtc_sha256_end(&s, hashes + 32 * (*count)++);
    }
}

TIDE_TEST(apk_is_signed_as_android_checks)
{
    // A library of 2.5 MiB, so the digest goes over several chunks
    const char *lib_path = "apk_test_libgame.so";
    const size_t lib_size = 5u << 19;
    uint8_t *lib = malloc(lib_size);
    for (size_t i = 0; i < lib_size; i++) lib[i] = (uint8_t)(i * 31u + (i >> 9));
    FILE *f = fopen(lib_path, "wb");
    TIDE_REQUIRE(f && fwrite(lib, 1, lib_size, f) == lib_size);
    fclose(f);

    char error[256];
    apk_key key;
    remove("apk_test.key");
    TIDE_REQUIRE(apk_key_load("apk_test.key", &key, error, sizeof error));
    apk_key again;
    TIDE_REQUIRE(apk_key_load("apk_test.key", &again, error, sizeof error)); // Read back, the same
    TIDE_CHECK(memcmp(key.private_key, again.private_key, 32) == 0 && key.cert_size == again.cert_size);

    const apk_desc desc = {.package = "dev.tide.test", .label = "Test", .lib_name = "game", .version_code = 3,
                           .version_name = "1.2", .min_sdk = 29, .target_sdk = 35, .lib_count = 1,
                           .abis = {"arm64-v8a"}, .libs = {lib_path}};
    TIDE_REQUIRE(apk_write("apk_test.apk", &desc, &key, error, sizeof error));
    size_t size = 0;
    uint8_t *apk = read_all("apk_test.apk", &size);
    TIDE_REQUIRE(apk && size > 22);

    // The end of the central directory, and the directory
    const uint8_t *end = apk + size - 22;
    TIDE_REQUIRE(get32(end) == 0x06054b50);
    TIDE_CHECK(get16(end + 10) == 2); // The manifest and the library
    const uint32_t directory_size = get32(end + 12), directory = get32(end + 16);
    TIDE_REQUIRE(directory + directory_size == size - 22);

    // Each entry stored, its data where the directory says, the library on a 16 KiB page
    const uint8_t *e = apk + directory;
    bool found_lib = false;
    for (int i = 0; i < 2; i++) {
        TIDE_REQUIRE(get32(e) == 0x02014b50);
        const uint32_t name_size = get16(e + 28), local = get32(e + 42);
        const uint32_t entry_size = get32(e + 24);
        TIDE_CHECK(get16(e + 10) == 0); // Stored
        const uint8_t *header = apk + local;
        TIDE_REQUIRE(get32(header) == 0x04034b50);
        const uint32_t data = local + 30 + get16(header + 26) + get16(header + 28);
        TIDE_CHECK(rtc_crc32(apk + data, entry_size) == get32(e + 16));
        if (name_size == strlen("lib/arm64-v8a/libgame.so") && memcmp(e + 46, "lib/arm64-v8a/libgame.so", name_size) == 0) {
            found_lib = true;
            TIDE_CHECK(data % 16384 == 0);
            TIDE_CHECK(entry_size == lib_size && memcmp(apk + data, lib, lib_size) == 0);
        }
        e += 46 + name_size + get16(e + 30) + get16(e + 32);
    }
    TIDE_CHECK(found_lib);

    // The signing block, just before the directory
    TIDE_REQUIRE(memcmp(apk + directory - 16, "APK Sig Block 42", 16) == 0);
    const uint64_t block_size = get64(apk + directory - 24);
    const uint8_t *block = apk + directory - block_size - 8;
    TIDE_REQUIRE(get64(block) == block_size);
    const uint64_t pair_size = get64(block + 8);
    TIDE_REQUIRE(get32(block + 16) == 0x7109871a);
    const uint8_t *v2 = block + 20, *v2_end = block + 16 + pair_size;

    const uint8_t *next;
    uint32_t n;
    const uint8_t *signers = piece(v2, v2_end, &next, &n);
    TIDE_REQUIRE(signers);
    const uint8_t *signer = piece(signers, signers + n, &next, &n);
    TIDE_REQUIRE(signer);
    const uint8_t *signer_end = signer + n;
    const uint8_t *signed_data = piece(signer, signer_end, &next, &n);
    TIDE_REQUIRE(signed_data);
    const uint32_t signed_size = n;
    const uint8_t *signatures = piece(next, signer_end, &next, &n);
    TIDE_REQUIRE(signatures);
    const uint8_t *public_key = piece(next, signer_end, &next, &n);
    TIDE_REQUIRE(public_key && n == 91);

    // The digest in the signed data, against one made as Android makes it,
    // with the end's directory offset where the block starts
    const uint8_t *digests = piece(signed_data, signed_data + signed_size, &next, &n);
    const uint8_t *certs_at = next;
    const uint8_t *digest = piece(digests, digests + n, &next, &n);
    TIDE_REQUIRE(digest && get32(digest) == 0x0201);
    const uint8_t *value = piece(digest + 4, digest + n, &next, &n);
    TIDE_REQUIRE(value && n == 32);
    uint8_t *end_copy = malloc(22);
    memcpy(end_copy, end, 22);
    const uint32_t block_start = (uint32_t)(block - apk);
    end_copy[16] = (uint8_t)block_start;
    end_copy[17] = (uint8_t)(block_start >> 8);
    end_copy[18] = (uint8_t)(block_start >> 16);
    end_copy[19] = (uint8_t)(block_start >> 24);
    uint8_t hashes[32 * 16];
    uint32_t count = 0;
    add_chunks(apk, block_start, NULL, &count, hashes);
    add_chunks(apk + directory, directory_size, NULL, &count, hashes);
    add_chunks(end_copy, 22, NULL, &count, hashes);
    TIDE_CHECK(count == 5); // 2.5 MiB of entries, the directory and the end
    uint8_t prefix[5] = {0x5a, (uint8_t)count, 0, 0, 0}, want[32];
    rtc_sha256 s;
    rtc_sha256_init(&s);
    rtc_sha256_add(&s, prefix, 5);
    rtc_sha256_add(&s, hashes, 32 * count);
    rtc_sha256_end(&s, want);
    TIDE_CHECK(memcmp(value, want, 32) == 0);
    free(end_copy);

    // The certificate's key is the signer's, and it signed the signed data
    const uint8_t *certs = piece(certs_at, signed_data + signed_size, &next, &n);
    const uint8_t *cert = piece(certs, certs + n, &next, &n);
    TIDE_REQUIRE(cert);
    uint8_t cert_key[65];
    TIDE_REQUIRE(rtc_cert_key(cert, n, cert_key));
    TIDE_CHECK(memcmp(public_key + 26, cert_key, 65) == 0);
    const uint8_t *signature = piece(signatures, signatures + 4 + get32(signatures - 4), &next, &n);
    TIDE_REQUIRE(signature && get32(signature) == 0x0201);
    const uint8_t *der = piece(signature + 4, signature + n, &next, &n);
    uint8_t sig[64], hash[32];
    TIDE_REQUIRE(der && rtc_sig_from_der(der, n, sig));
    rtc_sha256_of(signed_data, signed_size, hash);
    TIDE_CHECK(rtc_p256_verify(cert_key, hash, sig));

    free(apk);
    free(lib);
    remove("apk_test.apk");
    remove("apk_test.key");
    remove(lib_path);
}

// The manifest: binary XML with its string pool, its resource map and the
// app's ID and library in it.
TIDE_TEST(apk_manifest_is_binary_xml)
{
    const apk_desc desc = {.package = "dev.tide.sand", .label = "Tide sand", .lib_name = "sand", .version_code = 1,
                           .version_name = "1.0", .min_sdk = 29, .target_sdk = 35};
    uint8_t *xml;
    const size_t size = apk_manifest(&desc, &xml);
    TIDE_REQUIRE(size > 8);
    TIDE_CHECK(get16(xml) == 0x0003 && get32(xml + 4) == size);
    TIDE_CHECK(get16(xml + 8) == 0x0001); // The string pool first
    const uint32_t pool_size = get32(xml + 12);
    TIDE_CHECK(get16(xml + 8 + pool_size) == 0x0180); // Then the resource map
    bool package = false, lib = false;
    for (size_t i = 0; i + 13 <= size; i++) {
        if (memcmp(xml + i, "dev.tide.sand", 13) == 0) package = true;
        if (i + 4 <= size && memcmp(xml + i, "sand", 4) == 0 && xml[i + 4] == 0) lib = true;
    }
    TIDE_CHECK(package && lib);
    free(xml);
}
