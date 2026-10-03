// Android apps (apk.c, sign.c): checked the way Android checks them before it
// installs one. The zip's directory and its entries, then the v2 signature:
// the signing block before the directory, the digest of the rest, and the
// signature over it with the certificate's key. Bundles, the way a JAR's
// signature is checked: each entry's digest in the manifest, the manifest's
// in the signature file, and the signature file's signature.

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
    uint8_t *data = malloc(*size + 1);
    if (fread(data, 1, *size, f) != *size) *size = 0;
    data[*size] = 0;
    fclose(f);
    return data;
}

static bool contains(const uint8_t *data, const size_t size, const void *what, const size_t n)
{
    for (size_t i = 0; i + n <= size; i++) {
        if (memcmp(data + i, what, n) == 0) return true;
    }
    return false;
}

// A key made once for every test here: making one takes a while.
static const apk_key *test_key(void)
{
    static apk_key key;
    static bool made;
    char error[256];
    if (!made) {
        remove("apk_test_shared.pem");
        made = apk_key_load("apk_test_shared.pem", &key, NULL, error, sizeof error);
        remove("apk_test_shared.pem");
    }
    return made ? &key : NULL;
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
static void add_chunks(const uint8_t *data, const size_t size, uint32_t *count, uint8_t *hashes)
{
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

// The key file: made once, then read back as the same key, in PEM
TIDE_TEST(apk_key_is_made_once)
{
    char error[256];
    apk_key key, again;
    bool made;
    remove("apk_test.pem");
    TIDE_REQUIRE(apk_key_load("apk_test.pem", &key, &made, error, sizeof error));
    TIDE_CHECK(made);
    TIDE_REQUIRE(apk_key_load("apk_test.pem", &again, &made, error, sizeof error));
    TIDE_CHECK(!made);
    TIDE_CHECK(key.cert_size == again.cert_size && memcmp(key.cert, again.cert, key.cert_size) == 0);
    TIDE_CHECK(key.rsa.d.size == again.rsa.d.size && memcmp(key.rsa.d.bytes, again.rsa.d.bytes, key.rsa.d.size) == 0);
    TIDE_CHECK(rsa_size(&key.rsa.n) == 256); // 2048 bits, as Google Play wants
    size_t size;
    uint8_t *pem = read_all("apk_test.pem", &size);
    TIDE_REQUIRE(pem);
    TIDE_CHECK(strstr((char *)pem, "-----BEGIN PRIVATE KEY-----\n") && strstr((char *)pem, "-----BEGIN CERTIFICATE-----\n"));
    free(pem);
    remove("apk_test.pem");

    // Others' keys are refused, with why
    static const char ec[] = "-----BEGIN EC PRIVATE KEY-----\nMHcCAQEE\n-----END EC PRIVATE KEY-----\n";
    TIDE_CHECK(!apk_key_parse(ec, sizeof ec - 1, &again, error, sizeof error));
    TIDE_CHECK(strstr(error, "RSA") != NULL);
    char *text = apk_key_pem(&key);
    char *cert = strstr(text, "-----BEGIN CERTIFICATE-----");
    TIDE_REQUIRE(cert);
    TIDE_CHECK(apk_key_parse(text, strlen(text), &again, error, sizeof error));
    *cert = '\0'; // Without its certificate
    TIDE_CHECK(!apk_key_parse(text, strlen(text), &again, error, sizeof error));
    free(text);
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
    const apk_key *key = test_key();
    TIDE_REQUIRE(key);
    const apk_desc desc = {.package = "dev.tide.test", .label = "Test", .lib_name = "game", .version_code = 3,
                           .version_name = "1.2", .min_sdk = 29, .target_sdk = 36, .lib_count = 1,
                           .abis = {"arm64-v8a"}, .libs = {lib_path}};
    TIDE_REQUIRE(apk_write("apk_test.apk", &desc, key, error, sizeof error));
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
    TIDE_REQUIRE(public_key);
    const uint32_t public_key_size = n;

    // The digest in the signed data, against one made as Android makes it,
    // with the end's directory offset where the block starts
    const uint8_t *digests = piece(signed_data, signed_data + signed_size, &next, &n);
    const uint8_t *certs_at = next;
    const uint8_t *digest = piece(digests, digests + n, &next, &n);
    TIDE_REQUIRE(digest && get32(digest) == 0x0103); // RSASSA-PKCS1-v1_5 with SHA-256
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
    add_chunks(apk, block_start, &count, hashes);
    add_chunks(apk + directory, directory_size, &count, hashes);
    add_chunks(end_copy, 22, &count, hashes);
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
    TIDE_CHECK(n == key->cert_size && memcmp(cert, key->cert, n) == 0);
    TIDE_CHECK(contains(cert, n, public_key, public_key_size));
    const uint8_t *signature = piece(signatures, signatures + 4 + get32(signatures - 4), &next, &n);
    TIDE_REQUIRE(signature && get32(signature) == 0x0103);
    const uint8_t *sig = piece(signature + 4, signature + n, &next, &n);
    uint8_t hash[32];
    TIDE_REQUIRE(sig);
    rtc_sha256_of(signed_data, signed_size, hash);
    TIDE_CHECK(rsa_verify(&key->rsa.n, &key->rsa.e, sig, n, hash));

    free(apk);
    free(lib);
    remove("apk_test.apk");
    remove(lib_path);
}

// An icon: the resource table that names it, and the image, stored and aligned.
TIDE_TEST(apk_has_its_icon)
{
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    FILE *f = fopen("apk_test_icon.png", "wb");
    TIDE_REQUIRE(f && fwrite(png, 1, sizeof png, f) == sizeof png);
    fclose(f);
    f = fopen("apk_test_libgame.so", "wb");
    TIDE_REQUIRE(f && fwrite("lib", 1, 3, f) == 3);
    fclose(f);
    char error[256];
    const apk_key *key = test_key();
    TIDE_REQUIRE(key);
    const apk_desc desc = {.package = "dev.tide.test", .label = "Test", .lib_name = "game", .version_code = 1,
                           .version_name = "1.0", .min_sdk = 29, .target_sdk = 36, .lib_count = 1,
                           .abis = {"x86_64"}, .libs = {"apk_test_libgame.so"}, .icon = "apk_test_icon.png"};
    TIDE_REQUIRE(apk_write("apk_test.apk", &desc, key, error, sizeof error));
    size_t size = 0;
    uint8_t *apk = read_all("apk_test.apk", &size);
    TIDE_REQUIRE(apk);
    const uint8_t *end = apk + size - 22;
    TIDE_CHECK(get16(end + 10) == 4); // The manifest, the table, the icon and the library
    const uint8_t *e = apk + get32(end + 16);
    bool table = false, icon = false;
    for (int i = 0; i < 4; i++) {
        const uint32_t name_size = get16(e + 28), local = get32(e + 42);
        const uint8_t *header = apk + local;
        const uint32_t data = local + 30 + get16(header + 26) + get16(header + 28);
        if (name_size == 14 && memcmp(e + 46, "resources.arsc", 14) == 0) {
            table = get16(e + 10) == 0 && data % 4 == 0 && get16(apk + data) == 0x0002; // Stored, aligned, a table
        }
        if (name_size == 19 && memcmp(e + 46, "res/mipmap/icon.png", 19) == 0) {
            icon = memcmp(apk + data, png, sizeof png) == 0;
        }
        e += 46 + name_size + get16(e + 30) + get16(e + 32);
    }
    TIDE_CHECK(table && icon);

    // Something that isn't a PNG is refused
    const apk_desc wrong = {.package = "dev.tide.test", .label = "Test", .lib_name = "game", .version_code = 1,
                            .version_name = "1.0", .min_sdk = 29, .target_sdk = 36, .lib_count = 1,
                            .abis = {"x86_64"}, .libs = {"apk_test_libgame.so"}, .icon = "apk_test_libgame.so"};
    TIDE_CHECK(!apk_write("apk_test.apk", &wrong, key, error, sizeof error));
    free(apk);
    remove("apk_test.apk");
    remove("apk_test_icon.png");
    remove("apk_test_libgame.so");
}

// The manifest: binary XML with its string pool, its resource map and the
// app's ID and library in it, and the back button kept for the game.
TIDE_TEST(apk_manifest_is_binary_xml)
{
    const apk_desc desc = {.package = "dev.tide.sand", .label = "Tide sand", .lib_name = "sand", .version_code = 1,
                           .version_name = "1.0", .min_sdk = 29, .target_sdk = 36};
    uint8_t *xml;
    const size_t size = apk_manifest(&desc, &xml);
    TIDE_REQUIRE(size > 8);
    TIDE_CHECK(get16(xml) == 0x0003 && get32(xml + 4) == size);
    TIDE_CHECK(get16(xml + 8) == 0x0001); // The string pool first
    const uint32_t pool_size = get32(xml + 12);
    const uint8_t *map = xml + 8 + pool_size;
    TIDE_CHECK(get16(map) == 0x0180); // Then the resource map
    bool back = false;
    for (uint32_t i = 8; i < get32(map + 4); i += 4) back = back || get32(map + i) == 0x0101066c;
    TIDE_CHECK(back); // enableOnBackInvokedCallback
    TIDE_CHECK(contains(xml, size, "dev.tide.sand", 13) && contains(xml, size, "sand\0", 5));
    free(xml);
}

// A JAR section's digest, as the manifest or the signature file has it
static bool has_digest(const char *text, const char *name, const char *header, const void *data, const size_t size)
{
    uint8_t hash[32];
    char want[256], b64[48];
    rtc_sha256_of(data, size, hash);
    sign_base64(hash, sizeof hash, b64);
    if (name) snprintf(want, sizeof want, "Name: %s\r\n%s: %s\r\n\r\n", name, header, b64);
    else snprintf(want, sizeof want, "%s: %s\r\n", header, b64);
    return strstr(text, want) != NULL;
}

// A bundle: its signature first, then its config, manifest, resources, icon
// and libraries, as bundletool lays them out; signed as a JAR.
TIDE_TEST(apk_bundle_is_signed_as_a_jar)
{
    const uint8_t png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    FILE *f = fopen("apk_test_icon.png", "wb");
    TIDE_REQUIRE(f && fwrite(png, 1, sizeof png, f) == sizeof png);
    fclose(f);
    f = fopen("apk_test_libgame.so", "wb");
    TIDE_REQUIRE(f && fwrite("library", 1, 7, f) == 7);
    fclose(f);
    char error[256];
    const apk_key *key = test_key();
    TIDE_REQUIRE(key);
    const apk_desc desc = {.package = "dev.tide.test", .label = "Test", .lib_name = "game", .version_code = 7,
                           .version_name = "1.0", .min_sdk = 29, .target_sdk = 36, .lib_count = 2,
                           .abis = {"arm64-v8a", "x86_64"}, .libs = {"apk_test_libgame.so", "apk_test_libgame.so"},
                           .icon = "apk_test_icon.png"};
    TIDE_REQUIRE(apk_bundle_write("apk_test.aab", &desc, key, error, sizeof error));
    size_t size = 0;
    uint8_t *aab = read_all("apk_test.aab", &size);
    TIDE_REQUIRE(aab && size > 22);

    static const char *const names[] = {"META-INF/MANIFEST.MF", "META-INF/TIDE.SF", "META-INF/TIDE.RSA",
                                        "BundleConfig.pb", "base/manifest/AndroidManifest.xml", "base/resources.pb",
                                        "base/res/mipmap/icon.png", "base/lib/arm64-v8a/libgame.so",
                                        "base/lib/x86_64/libgame.so"};
    enum { COUNT = sizeof names / sizeof names[0] };
    const uint8_t *end = aab + size - 22;
    TIDE_REQUIRE(get32(end) == 0x06054b50 && get16(end + 10) == COUNT);
    const uint8_t *e = aab + get32(end + 16);
    const uint8_t *data[COUNT];
    uint32_t sizes[COUNT];
    for (int i = 0; i < COUNT; i++) {
        const uint32_t name_size = get16(e + 28), local = get32(e + 42);
        TIDE_REQUIRE(name_size == strlen(names[i]) && memcmp(e + 46, names[i], name_size) == 0);
        const uint8_t *header = aab + local;
        data[i] = header + 30 + get16(header + 26) + get16(header + 28);
        sizes[i] = get32(e + 24);
        TIDE_CHECK(rtc_crc32(data[i], sizes[i]) == get32(e + 16));
        e += 46 + name_size + get16(e + 30) + get16(e + 32);
    }

    // The manifest has each entry's digest; the signature file the
    // manifest's, and each of its sections'
    char *manifest = malloc(sizes[0] + 1), *sf = malloc(sizes[1] + 1);
    memcpy(manifest, data[0], sizes[0]);
    manifest[sizes[0]] = 0;
    memcpy(sf, data[1], sizes[1]);
    sf[sizes[1]] = 0;
    TIDE_CHECK(strncmp(manifest, "Manifest-Version: 1.0\r\n", 23) == 0);
    TIDE_CHECK(strncmp(sf, "Signature-Version: 1.0\r\n", 24) == 0);
    TIDE_CHECK(has_digest(sf, NULL, "SHA-256-Digest-Manifest", manifest, sizes[0]));
    for (int i = 3; i < COUNT; i++) {
        TIDE_CHECK(has_digest(manifest, names[i], "SHA-256-Digest", data[i], sizes[i]));
        char section[256];
        const char *at = strstr(manifest, names[i]);
        TIDE_REQUIRE(at && at - manifest >= 6);
        const char *section_end = strstr(at, "\r\n\r\n");
        TIDE_REQUIRE(section_end);
        const size_t n = (size_t)(section_end + 4 - (at - 6));
        TIDE_REQUIRE(n < sizeof section);
        memcpy(section, at - 6, n);
        TIDE_CHECK(has_digest(sf, names[i], "SHA-256-Digest", section, n));
    }

    // The block is PKCS #7, its signer's signature last: of the signature file
    uint8_t hash[32];
    rtc_sha256_of(sf, sizes[1], hash);
    TIDE_CHECK(data[2][0] == 0x30 && contains(data[2], sizes[2], key->cert, key->cert_size));
    TIDE_CHECK(rsa_verify(&key->rsa.n, &key->rsa.e, data[2] + sizes[2] - 256, 256, hash));

    // The config says its bundletool and pages of 16 KiB; the manifest is an
    // XmlNode with the app's ID
    static const uint8_t config[] = {0x0a, 0x08, 0x12, 0x06, '1', '.', '1', '8', '.', '3',
                                     0x12, 0x06, 0x12, 0x04, 0x08, 0x01, 0x10, 0x02};
    TIDE_CHECK(sizes[3] == sizeof config && memcmp(data[3], config, sizeof config) == 0);
    TIDE_CHECK(data[4][0] == 0x0a && contains(data[4], sizes[4], "dev.tide.test", 13));
    TIDE_CHECK(contains(data[5], sizes[5], "res/mipmap/icon.png", 19));
    TIDE_CHECK(sizes[6] == sizeof png && memcmp(data[6], png, sizeof png) == 0);

    free(manifest);
    free(sf);
    free(aab);
    remove("apk_test.aab");
    remove("apk_test_icon.png");
    remove("apk_test_libgame.so");
}

// The libraries the game's needs go beside it, each under its CPU and its
// name, stored on 16 KiB pages as the game's is: Android loads them from the
// app when it loads the game's. In a bundle too, each signed.
TIDE_TEST(apk_holds_the_libraries_the_game_needs)
{
    enum { PER_ABI = 9, NEEDED = 2 * PER_ABI }; // More than a few
    static const char *const abis[2] = {"arm64-v8a", "x86_64"};
    FILE *f = fopen("apk_test_libgame.so", "wb");
    TIDE_REQUIRE(f && fwrite("game", 1, 4, f) == 4);
    fclose(f);
    static char names[NEEDED][32], paths[NEEDED][48], contents[NEEDED][48];
    apk_lib needed[NEEDED];
    for (int i = 0; i < NEEDED; i++) {
        snprintf(names[i], sizeof names[i], "libneeded%d.so", i % PER_ABI);
        snprintf(paths[i], sizeof paths[i], "apk_test_needed%d.so", i);
        snprintf(contents[i], sizeof contents[i], "library %d for %s", i % PER_ABI, abis[i / PER_ABI]);
        f = fopen(paths[i], "wb");
        TIDE_REQUIRE(f && fwrite(contents[i], 1, strlen(contents[i]), f) == strlen(contents[i]));
        fclose(f);
        needed[i] = (apk_lib){abis[i / PER_ABI], names[i], paths[i]};
    }
    char error[256];
    const apk_key *key = test_key();
    TIDE_REQUIRE(key);
    const apk_desc desc = {.package = "dev.tide.test", .label = "Test", .lib_name = "game", .version_code = 1,
                           .version_name = "1.0", .min_sdk = 29, .target_sdk = 36, .lib_count = 2,
                           .abis = {abis[0], abis[1]}, .libs = {"apk_test_libgame.so", "apk_test_libgame.so"},
                           .needed_count = NEEDED, .needed = needed};

    TIDE_REQUIRE(apk_write("apk_test.apk", &desc, key, error, sizeof error));
    size_t size = 0;
    uint8_t *apk = read_all("apk_test.apk", &size);
    TIDE_REQUIRE(apk && size > 22);
    const uint8_t *end = apk + size - 22;
    TIDE_REQUIRE(get32(end) == 0x06054b50);
    const uint32_t count = get16(end + 10);
    TIDE_CHECK(count == 1 + 2 + NEEDED); // The manifest, the game's library for each CPU, and those
    const uint8_t *e = apk + get32(end + 16);
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        TIDE_REQUIRE(get32(e) == 0x02014b50);
        const uint32_t name_size = get16(e + 28), local = get32(e + 42), entry_size = get32(e + 24);
        const uint8_t *header = apk + local;
        const uint32_t data = local + 30 + get16(header + 26) + get16(header + 28);
        for (int k = 0; k < NEEDED; k++) {
            char name[96];
            snprintf(name, sizeof name, "lib/%s/%s", needed[k].abi, needed[k].name);
            if (name_size != strlen(name) || memcmp(e + 46, name, name_size) != 0) continue;
            found++;
            TIDE_CHECK(get16(e + 10) == 0); // Stored
            TIDE_CHECK(data % 16384 == 0);
            TIDE_CHECK(entry_size == strlen(contents[k]) && memcmp(apk + data, contents[k], entry_size) == 0);
        }
        e += 46 + name_size + get16(e + 30) + get16(e + 32);
    }
    TIDE_CHECK(found == NEEDED);
    TIDE_CHECK(memcmp(apk + get32(end + 16) - 16, "APK Sig Block 42", 16) == 0); // Signed, as the others are
    free(apk);

    TIDE_REQUIRE(apk_bundle_write("apk_test.aab", &desc, key, error, sizeof error));
    uint8_t *aab = read_all("apk_test.aab", &size);
    TIDE_REQUIRE(aab && size > 22);
    end = aab + size - 22;
    const uint32_t bundle_count = get16(end + 10);
    TIDE_CHECK(bundle_count == 3 + 2 + 2 + NEEDED); // The signature, the config and manifest, and the libraries
    e = aab + get32(end + 16);
    const uint8_t *manifest_header = aab + get32(e + 42); // META-INF/MANIFEST.MF comes first
    const uint32_t manifest_size = get32(e + 24);
    char *manifest = malloc(manifest_size + 1);
    memcpy(manifest, manifest_header + 30 + get16(manifest_header + 26) + get16(manifest_header + 28), manifest_size);
    manifest[manifest_size] = 0;
    found = 0;
    for (uint32_t i = 0; i < bundle_count; i++) {
        const uint32_t name_size = get16(e + 28), local = get32(e + 42), entry_size = get32(e + 24);
        const uint8_t *header = aab + local;
        const uint8_t *data = header + 30 + get16(header + 26) + get16(header + 28);
        for (int k = 0; k < NEEDED; k++) {
            char name[96];
            snprintf(name, sizeof name, "base/lib/%s/%s", needed[k].abi, needed[k].name);
            if (name_size != strlen(name) || memcmp(e + 46, name, name_size) != 0) continue;
            found++;
            TIDE_CHECK(entry_size == strlen(contents[k]) && memcmp(data, contents[k], entry_size) == 0);
            TIDE_CHECK(has_digest(manifest, name, "SHA-256-Digest", data, entry_size));
        }
        e += 46 + name_size + get16(e + 30) + get16(e + 32);
    }
    TIDE_CHECK(found == NEEDED);
    free(manifest);
    free(aab);

    // One that isn't there is said, by its path
    needed[3].path = "apk_test_missing.so";
    TIDE_CHECK(!apk_write("apk_test.apk", &desc, key, error, sizeof error));
    TIDE_CHECK(strstr(error, "apk_test_missing.so") != NULL);

    remove("apk_test.apk");
    remove("apk_test.aab");
    remove("apk_test_libgame.so");
    for (int i = 0; i < NEEDED; i++) remove(paths[i]);
}
