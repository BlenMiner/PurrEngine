// Android apps (see apk.h): the binary manifest, the zip, and the v2 signature.
// Written from Android's sources and documentation: ResourceTypes.h for the
// binary XML, and https://source.android.com/docs/security/features/apksigning/v2
// for the signature. P-256, certificates, CRC-32 and random bytes are the
// platform layer's own (platform/src/rtc).

#include "apk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtc.h"

// ---------------------------------------------------------------------------
// Bytes, little-endian as Android's formats and zip are

typedef struct bytes {
    uint8_t *data;
    size_t size, capacity;
} bytes;

static void put(bytes *b, const void *p, const size_t n)
{
    if (b->size + n > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 4096;
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

static void put8(bytes *b, const uint8_t v)
{
    put(b, &v, 1);
}

static void put16(bytes *b, const uint16_t v)
{
    const uint8_t p[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, p, 2);
}

static void put32(bytes *b, const uint32_t v)
{
    const uint8_t p[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    put(b, p, 4);
}

static void put64(bytes *b, const uint64_t v)
{
    put32(b, (uint32_t)v);
    put32(b, (uint32_t)(v >> 32));
}

static void set32(uint8_t *at, const uint32_t v)
{
    at[0] = (uint8_t)v;
    at[1] = (uint8_t)(v >> 8);
    at[2] = (uint8_t)(v >> 16);
    at[3] = (uint8_t)(v >> 24);
}

// A length-prefixed block, as the signature's structures nest them: put32 a
// placeholder, write, then close it.
static size_t open_length(bytes *b)
{
    put32(b, 0);
    return b->size;
}

static void close_length(bytes *b, const size_t start)
{
    set32(b->data + start - 4, (uint32_t)(b->size - start));
}

// ---------------------------------------------------------------------------
// The manifest, as binary XML

#define ANDROID_NS "http://schemas.android.com/apk/res/android"

// Android's attributes (frameworks/base/core/res/res/values/public-final.xml)
enum {
    ATTR_THEME = 0x01010000,
    ATTR_LABEL = 0x01010001,
    ATTR_ICON = 0x01010002,
    ATTR_NAME = 0x01010003,
    ATTR_HAS_CODE = 0x0101000c,
    ATTR_DEBUGGABLE = 0x0101000f,
    ATTR_EXPORTED = 0x01010010,
    ATTR_LAUNCH_MODE = 0x0101001d,
    ATTR_CONFIG_CHANGES = 0x0101001f,
    ATTR_VALUE = 0x01010024,
    ATTR_MIN_SDK = 0x0101020c,
    ATTR_VERSION_CODE = 0x0101021b,
    ATTR_VERSION_NAME = 0x0101021c,
    ATTR_TARGET_SDK = 0x01010270,
    ATTR_GL_ES_VERSION = 0x01010281,
    ATTR_REQUIRED = 0x0101028e,
    ATTR_EXTRACT_NATIVE_LIBS = 0x010104ea,
    ATTR_APP_CATEGORY = 0x01010545,
};
#define THEME_FULLSCREEN 0x0103000au // @android:style/Theme.Black.NoTitleBar.Fullscreen
#define APP_ICON 0x7f010000u         // @mipmap/icon: the app's own resources, its first type, its first entry
#define ICON_PATH "res/mipmap/icon.png"
#define LAUNCH_SINGLE_TASK 2
#define CATEGORY_GAME 0
// Every configuration change the activity takes itself, rather than starting
// over (the game with it): turns, keyboards, sizes, density, ...
#define CONFIG_CHANGES 0x5000ffffu

// Res_value's types
enum { VALUE_REFERENCE = 0x01, VALUE_STRING = 0x03, VALUE_INT = 0x10, VALUE_HEX = 0x11, VALUE_BOOL = 0x12 };

typedef struct xml_attr {
    uint32_t id;      // Android's resource ID for it, or 0 for one of no namespace (`package`)
    const char *name;
    uint8_t type;
    uint32_t data;    // VALUE_STRING: unused
    const char *text; // VALUE_STRING's
} xml_attr;

#define XML_MAX_ATTRS 10

typedef struct xml_node {
    const char *name;
    bool end; // Closes the element `name`
    int count;
    xml_attr attrs[XML_MAX_ATTRS];
} xml_node;

typedef struct xml {
    xml_node nodes[24];
    int count;
    const char *strings[64]; // The string pool, those with resource IDs first, in the resource map's order
    uint32_t ids[64];
    int string_count, id_count;
} xml;

static xml_node *open_node(xml *x, const char *name)
{
    xml_node *n = &x->nodes[x->count++];
    *n = (xml_node){.name = name};
    return n;
}

static void close_node(xml *x, const char *name)
{
    x->nodes[x->count++] = (xml_node){.name = name, .end = true};
}

static void attr_value(xml_node *n, const uint32_t id, const char *name, const uint8_t type, const uint32_t data)
{
    n->attrs[n->count++] = (xml_attr){id, name, type, data, NULL};
}

static void attr_text(xml_node *n, const uint32_t id, const char *name, const char *text)
{
    n->attrs[n->count++] = (xml_attr){id, name, VALUE_STRING, 0, text};
}

static uint32_t string_index(xml *x, const char *s)
{
    for (int i = 0; i < x->string_count; i++) {
        if (strcmp(x->strings[i], s) == 0) return (uint32_t)i;
    }
    x->strings[x->string_count] = s;
    return (uint32_t)x->string_count++;
}

// Attributes in the order Android reads them: by resource ID, then those of
// no namespace by name, as aapt2 sorts them.
static int compare_attrs(const void *a, const void *b)
{
    const xml_attr *x = a, *y = b;
    if (x->id && y->id) return x->id < y->id ? -1 : x->id > y->id;
    if (x->id || y->id) return x->id ? -1 : 1;
    return strcmp(x->name, y->name);
}

// A string in the pool: its length in UTF-16 units and in bytes (ASCII here,
// so the same), each a byte below 128 or two above, then its bytes and a zero.
static void put_pool_length(bytes *b, const size_t n)
{
    if (n < 0x80) {
        put8(b, (uint8_t)n);
    } else {
        put8(b, (uint8_t)(0x80 | (n >> 8)));
        put8(b, (uint8_t)n);
    }
}

static void write_xml(xml *x, bytes *out)
{
    // The pool: names with resource IDs first, the resource map naming them.
    for (int i = 0; i < x->count; i++) {
        xml_node *n = &x->nodes[i];
        qsort(n->attrs, (size_t)n->count, sizeof n->attrs[0], compare_attrs);
        for (int k = 0; k < n->count; k++) {
            if (!n->attrs[k].id) continue;
            const uint32_t at = string_index(x, n->attrs[k].name);
            if (at == (uint32_t)x->id_count) x->ids[x->id_count++] = n->attrs[k].id;
        }
    }
    const uint32_t prefix = string_index(x, "android");
    const uint32_t uri = string_index(x, ANDROID_NS);
    for (int i = 0; i < x->count; i++) {
        const xml_node *n = &x->nodes[i];
        string_index(x, n->name);
        for (int k = 0; k < n->count; k++) {
            string_index(x, n->attrs[k].name);
            if (n->attrs[k].type == VALUE_STRING) string_index(x, n->attrs[k].text);
        }
    }

    put16(out, 0x0003); // RES_XML_TYPE
    put16(out, 8);
    put32(out, 0); // Its size, once it's written

    // RES_STRING_POOL_TYPE, of UTF-8 strings
    const size_t pool = out->size;
    put16(out, 0x0001);
    put16(out, 28);
    put32(out, 0);
    put32(out, (uint32_t)x->string_count);
    put32(out, 0);      // Styles
    put32(out, 1 << 8); // UTF8_FLAG
    put32(out, (uint32_t)(28 + 4 * x->string_count));
    put32(out, 0);
    bytes data = {0};
    for (int i = 0; i < x->string_count; i++) {
        put32(out, (uint32_t)data.size);
        const size_t n = strlen(x->strings[i]);
        put_pool_length(&data, n);
        put_pool_length(&data, n);
        put(&data, x->strings[i], n);
        put8(&data, 0);
    }
    while (data.size % 4) put8(&data, 0);
    put(out, data.data, data.size);
    free(data.data);
    set32(out->data + pool + 4, (uint32_t)(out->size - pool));

    // RES_XML_RESOURCE_MAP_TYPE
    put16(out, 0x0180);
    put16(out, 8);
    put32(out, (uint32_t)(8 + 4 * x->id_count));
    for (int i = 0; i < x->id_count; i++) put32(out, x->ids[i]);

    // Nodes: a line number and no comment each
    put16(out, 0x0100); // RES_XML_START_NAMESPACE_TYPE
    put16(out, 16);
    put32(out, 24);
    put32(out, 1);
    put32(out, 0xffffffffu);
    put32(out, prefix);
    put32(out, uri);
    for (int i = 0; i < x->count; i++) {
        const xml_node *n = &x->nodes[i];
        const uint32_t name = string_index(x, n->name);
        if (n->end) {
            put16(out, 0x0103); // RES_XML_END_ELEMENT_TYPE
            put16(out, 16);
            put32(out, 24);
            put32(out, 1);
            put32(out, 0xffffffffu);
            put32(out, 0xffffffffu);
            put32(out, name);
            continue;
        }
        put16(out, 0x0102); // RES_XML_START_ELEMENT_TYPE
        put16(out, 16);
        put32(out, (uint32_t)(16 + 20 + 20 * n->count));
        put32(out, 1);
        put32(out, 0xffffffffu);
        put32(out, 0xffffffffu); // The element's namespace: none
        put32(out, name);
        put16(out, 20); // Where its attributes start
        put16(out, 20); // Each one's size
        put16(out, (uint16_t)n->count);
        put16(out, 0); // id, class and style attributes: none
        put16(out, 0);
        put16(out, 0);
        for (int k = 0; k < n->count; k++) {
            const xml_attr *a = &n->attrs[k];
            const uint32_t text = a->type == VALUE_STRING ? string_index(x, a->text) : 0xffffffffu;
            put32(out, a->id ? uri : 0xffffffffu);
            put32(out, string_index(x, a->name));
            put32(out, text);
            put16(out, 8); // Res_value
            put8(out, 0);
            put8(out, a->type);
            put32(out, a->type == VALUE_STRING ? text : a->data);
        }
    }
    put16(out, 0x0101); // RES_XML_END_NAMESPACE_TYPE
    put16(out, 16);
    put32(out, 24);
    put32(out, 1);
    put32(out, 0xffffffffu);
    put32(out, prefix);
    put32(out, uri);
    set32(out->data + 4, (uint32_t)out->size);
}

size_t apk_manifest(const apk_desc *desc, uint8_t **out)
{
    xml x = {0};
    xml_node *n = open_node(&x, "manifest");
    attr_value(n, ATTR_VERSION_CODE, "versionCode", VALUE_INT, (uint32_t)desc->version_code);
    attr_text(n, ATTR_VERSION_NAME, "versionName", desc->version_name);
    attr_text(n, 0, "package", desc->package);

    n = open_node(&x, "uses-sdk");
    attr_value(n, ATTR_MIN_SDK, "minSdkVersion", VALUE_INT, (uint32_t)desc->min_sdk);
    attr_value(n, ATTR_TARGET_SDK, "targetSdkVersion", VALUE_INT, (uint32_t)desc->target_sdk);
    close_node(&x, "uses-sdk");

    n = open_node(&x, "uses-feature"); // OpenGL ES 3, which the renderer needs
    attr_value(n, ATTR_GL_ES_VERSION, "glEsVersion", VALUE_HEX, 0x00030000);
    attr_value(n, ATTR_REQUIRED, "required", VALUE_BOOL, 0xffffffffu);
    close_node(&x, "uses-feature");

    n = open_node(&x, "uses-permission"); // Matches go over the network
    attr_text(n, ATTR_NAME, "name", "android.permission.INTERNET");
    close_node(&x, "uses-permission");

    // No code of its own, and its libraries straight from the APK, unpacked:
    // they're stored, aligned to pages, as Android maps them.
    n = open_node(&x, "application");
    attr_value(n, ATTR_THEME, "theme", VALUE_REFERENCE, THEME_FULLSCREEN);
    attr_text(n, ATTR_LABEL, "label", desc->label);
    if (desc->icon) attr_value(n, ATTR_ICON, "icon", VALUE_REFERENCE, APP_ICON);
    attr_value(n, ATTR_HAS_CODE, "hasCode", VALUE_BOOL, 0);
    if (desc->debuggable) attr_value(n, ATTR_DEBUGGABLE, "debuggable", VALUE_BOOL, 0xffffffffu);
    attr_value(n, ATTR_EXTRACT_NATIVE_LIBS, "extractNativeLibs", VALUE_BOOL, 0);
    attr_value(n, ATTR_APP_CATEGORY, "appCategory", VALUE_INT, CATEGORY_GAME);

    n = open_node(&x, "activity");
    attr_text(n, ATTR_LABEL, "label", desc->label);
    attr_text(n, ATTR_NAME, "name", "android.app.NativeActivity");
    attr_value(n, ATTR_EXPORTED, "exported", VALUE_BOOL, 0xffffffffu);
    attr_value(n, ATTR_LAUNCH_MODE, "launchMode", VALUE_INT, LAUNCH_SINGLE_TASK);
    attr_value(n, ATTR_CONFIG_CHANGES, "configChanges", VALUE_HEX, CONFIG_CHANGES);

    n = open_node(&x, "meta-data");
    attr_text(n, ATTR_NAME, "name", "android.app.lib_name");
    attr_text(n, ATTR_VALUE, "value", desc->lib_name);
    close_node(&x, "meta-data");

    open_node(&x, "intent-filter");
    n = open_node(&x, "action");
    attr_text(n, ATTR_NAME, "name", "android.intent.action.MAIN");
    close_node(&x, "action");
    n = open_node(&x, "category");
    attr_text(n, ATTR_NAME, "name", "android.intent.category.LAUNCHER");
    close_node(&x, "category");
    close_node(&x, "intent-filter");

    close_node(&x, "activity");
    close_node(&x, "application");
    close_node(&x, "manifest");

    bytes b = {0};
    write_xml(&x, &b);
    *out = b.data;
    return b.size;
}

// ---------------------------------------------------------------------------
// The resource table: one package (the app's, 0x7f), one type (mipmap) and
// one entry (icon), its value the icon's path in the APK, for every
// configuration (one image, which Android scales to each screen).

// A string pool of UTF-8 strings, as the manifest's (write_xml).
static void put_pool(bytes *b, const char *const *strings, const int count)
{
    const size_t start = b->size;
    put16(b, 0x0001);
    put16(b, 28);
    put32(b, 0);
    put32(b, (uint32_t)count);
    put32(b, 0);
    put32(b, 1 << 8); // UTF8_FLAG
    put32(b, (uint32_t)(28 + 4 * count));
    put32(b, 0);
    bytes data = {0};
    for (int i = 0; i < count; i++) {
        put32(b, (uint32_t)data.size);
        const size_t n = strlen(strings[i]);
        put_pool_length(&data, n);
        put_pool_length(&data, n);
        put(&data, strings[i], n);
        put8(&data, 0);
    }
    while (data.size % 4) put8(&data, 0);
    put(b, data.data, data.size);
    free(data.data);
    set32(b->data + start + 4, (uint32_t)(b->size - start));
}

size_t apk_resources(const char *package, uint8_t **out)
{
    bytes b = {0};
    put16(&b, 0x0002); // RES_TABLE_TYPE
    put16(&b, 12);
    put32(&b, 0);
    put32(&b, 1); // Packages
    const char *const values[] = {ICON_PATH};
    put_pool(&b, values, 1);

    const size_t pkg = b.size;
    put16(&b, 0x0200); // RES_TABLE_PACKAGE_TYPE
    put16(&b, 288);
    put32(&b, 0);
    put32(&b, 0x7f);
    const size_t name = strlen(package);
    for (size_t i = 0; i < 128; i++) put16(&b, i < name && i < 127 ? (uint16_t)package[i] : 0); // UTF-16
    put32(&b, 0); // Where the type strings start, once they're written
    put32(&b, 1); // The last public type
    put32(&b, 0); // ...and key strings
    put32(&b, 1);
    put32(&b, 0); // typeIdOffset
    set32(b.data + pkg + 268, (uint32_t)(b.size - pkg));
    const char *const types[] = {"mipmap"};
    put_pool(&b, types, 1);
    set32(b.data + pkg + 276, (uint32_t)(b.size - pkg));
    const char *const keys[] = {"icon"};
    put_pool(&b, keys, 1);

    put16(&b, 0x0202); // RES_TABLE_TYPE_SPEC_TYPE: which configurations each entry varies by (none)
    put16(&b, 16);
    put32(&b, 16 + 4);
    put8(&b, 1); // The type's ID
    put8(&b, 0);
    put16(&b, 0);
    put32(&b, 1); // Entries
    put32(&b, 0);

    const size_t type = b.size;
    put16(&b, 0x0201); // RES_TABLE_TYPE_TYPE: the entries for one configuration, the default
    put16(&b, 20 + 64);
    put32(&b, 0);
    put8(&b, 1);
    put8(&b, 0);
    put16(&b, 0);
    put32(&b, 1);           // Entries
    put32(&b, 20 + 64 + 4); // Where they start
    put32(&b, 64);          // ResTable_config: its size, then zeros, which match every screen
    for (int i = 0; i < 60; i++) put8(&b, 0);
    put32(&b, 0); // The entry's offset
    put16(&b, 8); // ResTable_entry: its size, no flags, its key
    put16(&b, 0);
    put32(&b, 0);
    put16(&b, 8); // Res_value: the path, a string of the table's
    put8(&b, 0);
    put8(&b, VALUE_STRING);
    put32(&b, 0);
    set32(b.data + type + 4, (uint32_t)(b.size - type));
    set32(b.data + pkg + 4, (uint32_t)(b.size - pkg));
    set32(b.data + 4, (uint32_t)b.size);
    *out = b.data;
    return b.size;
}

// ---------------------------------------------------------------------------
// The zip: every file stored as it is, and libraries aligned to 16 KiB pages,
// so Android maps them from the APK. Times are 1980's first day, so the same
// files make the same APK.

typedef struct zip_entry {
    const char *name;
    uint32_t crc, size, offset;
} zip_entry;

static bool read_file(const char *path, bytes *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) put(out, chunk, n);
    const bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static void zip_add(bytes *z, zip_entry *e, const char *name, const uint8_t *data, const size_t size, const size_t align)
{
    const size_t name_size = strlen(name);
    e->name = name;
    e->crc = rtc_crc32(data, size);
    e->size = (uint32_t)size;
    e->offset = (uint32_t)z->size;
    // The data starts at a multiple of `align`: an extra field pads it, as
    // zipalign's does (0xd935, which says the alignment).
    const size_t header = 30 + name_size;
    size_t extra = 6;
    while ((z->size + header + extra) % align) extra++;
    put32(z, 0x04034b50);
    put16(z, 10); // Version needed: stored
    put16(z, 0);  // Flags
    put16(z, 0);  // Stored
    put16(z, 0);  // Time
    put16(z, 0x21); // Date: 1980-01-01
    put32(z, e->crc);
    put32(z, e->size);
    put32(z, e->size);
    put16(z, (uint16_t)name_size);
    put16(z, (uint16_t)extra);
    put(z, name, name_size);
    put16(z, 0xd935);
    put16(z, (uint16_t)(extra - 4));
    put16(z, (uint16_t)align);
    for (size_t i = 6; i < extra; i++) put8(z, 0);
    put(z, data, size);
}

// ---------------------------------------------------------------------------
// APK Signature Scheme v2, with ECDSA over P-256 and SHA-256

#define SIG_ECDSA_SHA256 0x0201u
#define V2_BLOCK_ID 0x7109871au
#define CHUNK (1u << 20)

// SHA-256 of a section of the APK in 1 MiB chunks, each hashed with 0xa5 and
// its size; adds each chunk's hash to `hashes`.
static void hash_chunks(const uint8_t *data, const size_t size, bytes *hashes, uint32_t *count)
{
    for (size_t at = 0; at < size; at += CHUNK) {
        const size_t n = size - at < CHUNK ? size - at : CHUNK;
        uint8_t prefix[5] = {0xa5};
        set32(prefix + 1, (uint32_t)n);
        rtc_sha256 s;
        rtc_sha256_init(&s);
        rtc_sha256_add(&s, prefix, sizeof prefix);
        rtc_sha256_add(&s, data + at, n);
        uint8_t hash[32];
        rtc_sha256_end(&s, hash);
        put(hashes, hash, sizeof hash);
        (*count)++;
    }
}

// The key's SubjectPublicKeyInfo, in DER: what the certificate has.
static void public_key_info(const uint8_t public_key[65], bytes *out)
{
    static const uint8_t head[] = {0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02, 0x01,
                                   0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03, 0x42, 0x00};
    put(out, head, sizeof head);
    put(out, public_key, 65);
}

// The signing block for an APK whose entries are `entries`, central directory
// `directory` and end `end` (its directory offset where the block goes).
static bool signing_block(const bytes *entries, const bytes *directory, const bytes *end, const apk_key *key, bytes *out)
{
    bytes chunks = {0};
    uint32_t count = 0;
    hash_chunks(entries->data, entries->size, &chunks, &count);
    hash_chunks(directory->data, directory->size, &chunks, &count);
    hash_chunks(end->data, end->size, &chunks, &count);
    uint8_t prefix[5] = {0x5a};
    set32(prefix + 1, count);
    rtc_sha256 s;
    rtc_sha256_init(&s);
    rtc_sha256_add(&s, prefix, sizeof prefix);
    rtc_sha256_add(&s, chunks.data, chunks.size);
    uint8_t digest[32];
    rtc_sha256_end(&s, digest);
    free(chunks.data);

    uint8_t public_key[65];
    if (!rtc_p256_public(key->private_key, public_key)) return false;

    // signed data: digests, certificates, additional attributes
    bytes signed_data = {0};
    size_t list = open_length(&signed_data);
    size_t item = open_length(&signed_data);
    put32(&signed_data, SIG_ECDSA_SHA256);
    size_t value = open_length(&signed_data);
    put(&signed_data, digest, sizeof digest);
    close_length(&signed_data, value);
    close_length(&signed_data, item);
    close_length(&signed_data, list);
    list = open_length(&signed_data);
    item = open_length(&signed_data);
    put(&signed_data, key->cert, key->cert_size);
    close_length(&signed_data, item);
    close_length(&signed_data, list);
    put32(&signed_data, 0);

    uint8_t hash[32], sig[64], sig_der[72];
    rtc_sha256_of(signed_data.data, signed_data.size, hash);
    if (!rtc_p256_sign(key->private_key, hash, sig)) {
        free(signed_data.data);
        return false;
    }
    const size_t sig_size = rtc_sig_to_der(sig, sig_der);

    // The v2 block: signers, each its signed data, signatures and public key.
    bytes v2 = {0};
    const size_t signers = open_length(&v2);
    const size_t signer = open_length(&v2);
    value = open_length(&v2);
    put(&v2, signed_data.data, signed_data.size);
    close_length(&v2, value);
    free(signed_data.data);
    list = open_length(&v2);
    item = open_length(&v2);
    put32(&v2, SIG_ECDSA_SHA256);
    value = open_length(&v2);
    put(&v2, sig_der, sig_size);
    close_length(&v2, value);
    close_length(&v2, item);
    close_length(&v2, list);
    value = open_length(&v2);
    public_key_info(public_key, &v2);
    close_length(&v2, value);
    close_length(&v2, signer);
    close_length(&v2, signers);

    // The APK Signing Block: its size, an ID-value pair, its size again, magic
    const uint64_t pair = 4 + v2.size;
    const uint64_t size = 8 + pair + 8 + 16;
    put64(out, size);
    put64(out, pair);
    put32(out, V2_BLOCK_ID);
    put(out, v2.data, v2.size);
    put64(out, size);
    put(out, "APK Sig Block 42", 16);
    free(v2.data);
    return true;
}

// ---------------------------------------------------------------------------

static bool fail(char *error, const size_t error_size, const char *message, const char *what)
{
    snprintf(error, error_size, "%s%s", message, what ? what : "");
    return false;
}

bool apk_write(const char *path, const apk_desc *desc, const apk_key *key, char *error, const size_t error_size)
{
    bytes entries = {0};
    zip_entry list[3 + APK_MAX_LIBS];
    int count = 0;

    uint8_t *manifest;
    const size_t manifest_size = apk_manifest(desc, &manifest);
    zip_add(&entries, &list[count++], "AndroidManifest.xml", manifest, manifest_size, 4);
    free(manifest);
    if (desc->icon) {
        bytes icon = {0};
        static const uint8_t png[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
        if (!read_file(desc->icon, &icon) || icon.size < 8 || memcmp(icon.data, png, 8) != 0) {
            free(icon.data);
            free(entries.data);
            return fail(error, error_size, "the icon isn't a PNG: ", desc->icon);
        }
        uint8_t *table;
        const size_t table_size = apk_resources(desc->package, &table);
        zip_add(&entries, &list[count++], "resources.arsc", table, table_size, 4); // Stored and aligned, as Android 11 wants
        free(table);
        zip_add(&entries, &list[count++], ICON_PATH, icon.data, icon.size, 4);
        free(icon.data);
    }

    char names[APK_MAX_LIBS][256];
    for (int i = 0; i < desc->lib_count; i++) {
        bytes lib = {0};
        if (!read_file(desc->libs[i], &lib)) {
            free(entries.data);
            return fail(error, error_size, "can't read ", desc->libs[i]);
        }
        snprintf(names[i], sizeof names[i], "lib/%s/lib%s.so", desc->abis[i], desc->lib_name);
        zip_add(&entries, &list[count++], names[i], lib.data, lib.size, 16384);
        free(lib.data);
    }

    bytes directory = {0};
    for (int i = 0; i < count; i++) {
        const zip_entry *e = &list[i];
        const size_t name_size = strlen(e->name);
        put32(&directory, 0x02014b50);
        put16(&directory, 10); // Made by: MS-DOS's attributes, version 1.0
        put16(&directory, 10);
        put16(&directory, 0);
        put16(&directory, 0);
        put16(&directory, 0);
        put16(&directory, 0x21);
        put32(&directory, e->crc);
        put32(&directory, e->size);
        put32(&directory, e->size);
        put16(&directory, (uint16_t)name_size);
        put16(&directory, 0); // Extra
        put16(&directory, 0); // Comment
        put16(&directory, 0); // Disk
        put16(&directory, 0); // Internal attributes
        put32(&directory, 0); // External attributes
        put32(&directory, e->offset);
        put(&directory, e->name, name_size);
    }

    bytes end = {0};
    put32(&end, 0x06054b50);
    put16(&end, 0);
    put16(&end, 0);
    put16(&end, (uint16_t)count);
    put16(&end, (uint16_t)count);
    put32(&end, (uint32_t)directory.size);
    put32(&end, (uint32_t)entries.size); // Where the directory starts: where the signing block goes
    put16(&end, 0);

    bytes block = {0};
    bool ok = signing_block(&entries, &directory, &end, key, &block);
    if (!ok) fail(error, error_size, "can't sign the app", NULL);
    set32(end.data + 16, (uint32_t)(entries.size + block.size)); // The directory, after the block

    if (ok) {
        FILE *f = fopen(path, "wb");
        ok = f && fwrite(entries.data, 1, entries.size, f) == entries.size
          && fwrite(block.data, 1, block.size, f) == block.size
          && fwrite(directory.data, 1, directory.size, f) == directory.size
          && fwrite(end.data, 1, end.size, f) == end.size;
        if (f && fclose(f) != 0) ok = false;
        if (!ok) fail(error, error_size, "can't write ", path);
    }
    free(entries.data);
    free(directory.data);
    free(end.data);
    free(block.data);
    return ok;
}

// ---------------------------------------------------------------------------
// The key: "tide android key 1", then the private key, then the certificate.

static const char KEY_MAGIC[] = "tide android key 1\n";

bool apk_key_load(const char *path, apk_key *key, char *error, const size_t error_size)
{
    bytes file = {0};
    if (read_file(path, &file)) {
        const size_t magic = sizeof KEY_MAGIC - 1;
        const bool ok = file.size > magic + 32 && file.size - magic - 32 <= sizeof key->cert
                     && memcmp(file.data, KEY_MAGIC, magic) == 0;
        if (ok) {
            memcpy(key->private_key, file.data + magic, 32);
            key->cert_size = file.size - magic - 32;
            memcpy(key->cert, file.data + magic + 32, key->cert_size);
            uint8_t public_key[65], cert_key[65];
            if (!rtc_p256_public(key->private_key, public_key) || !rtc_cert_key(key->cert, key->cert_size, cert_key)
                || memcmp(public_key, cert_key, sizeof public_key) != 0) {
                free(file.data);
                return fail(error, error_size, "the app's key doesn't go with its certificate: ", path);
            }
        }
        free(file.data);
        if (!ok) return fail(error, error_size, "not a key tide made: ", path);
        return true;
    }

    rtc_identity id;
    if (!rtc_identity_new(&id)) return fail(error, error_size, "can't make a key for the app", NULL);
    memcpy(key->private_key, id.private_key, 32);
    memcpy(key->cert, id.cert, id.cert_size);
    key->cert_size = id.cert_size;
    // Written whole, under another name, then renamed: a key half written
    // would sign nothing anyone could update.
    char partial[1024];
    snprintf(partial, sizeof partial, "%s.partial", path);
    FILE *f = fopen(partial, "wb");
    bool ok = f && fwrite(KEY_MAGIC, 1, sizeof KEY_MAGIC - 1, f) == sizeof KEY_MAGIC - 1
           && fwrite(key->private_key, 1, 32, f) == 32 && fwrite(key->cert, 1, key->cert_size, f) == key->cert_size;
    if (f && fclose(f) != 0) ok = false;
    if (!ok || rename(partial, path) != 0) {
        remove(partial);
        return fail(error, error_size, "can't write the app's key to ", path);
    }
    return true;
}
