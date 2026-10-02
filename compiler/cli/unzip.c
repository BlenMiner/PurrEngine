#define _FILE_OFFSET_BITS 64 // Archives past 2 GiB, where long is 32 bits
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L // fseeko, chmod
#endif

#include "unzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define seek64 _fseeki64
#define tell64 _ftelli64
#else
#include <sys/stat.h>
#define seek64 fseeko
#define tell64 ftello
#endif

#include "sys.h"

uint32_t rtc_crc32(const void *data, size_t size); // The platform layer's, which tide builds in (platform/src/rtc)

// ---------------------------------------------------------------------------
// DEFLATE: blocks stored as they are, or coded with Huffman codes, fixed or
// given in the block, of literals, lengths and distances back.

typedef struct inflater {
    const uint8_t *in;
    size_t in_size, in_at;
    uint32_t bits; // Bits read but not used, from the lowest
    int bit_count;
    uint8_t *out;
    size_t out_size, out_at;
    bool broken; // Read past the end, or coded wrongly
} inflater;

static uint32_t take_bits(inflater *s, const int n)
{
    while (s->bit_count < n) {
        if (s->in_at == s->in_size) {
            s->broken = true;
            return 0;
        }
        s->bits |= (uint32_t)s->in[s->in_at++] << s->bit_count;
        s->bit_count += 8;
    }
    const uint32_t value = s->bits & ((1u << n) - 1);
    s->bits >>= n;
    s->bit_count -= n;
    return value;
}

// A canonical Huffman code: how many codes of each length, and the symbols in
// the order of their codes.
typedef struct huffman {
    uint16_t count[16];
    uint16_t symbol[320];
} huffman;

// Builds the code from each symbol's code length; false if it says more codes
// than lengths can hold.
static bool build_huffman(huffman *h, const uint8_t *lengths, const int n)
{
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left = left * 2 - h->count[len];
        if (left < 0) return false;
    }
    uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; len++) offsets[len + 1] = (uint16_t)(offsets[len] + h->count[len]);
    for (int i = 0; i < n; i++) {
        if (lengths[i]) h->symbol[offsets[lengths[i]]++] = (uint16_t)i;
    }
    return true;
}

// The next symbol: codes are read a bit at a time, from their first.
static int decode(inflater *s, const huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)take_bits(s, 1);
        const int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->broken) return -1;
    }
    s->broken = true;
    return -1;
}

static const uint16_t length_base[] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                       31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t distance_base[] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t distance_extra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static bool inflate_codes(inflater *s, const huffman *lengths, const huffman *distances)
{
    for (;;) {
        const int symbol = decode(s, lengths);
        if (symbol < 0) return false;
        if (symbol < 256) {
            if (s->out_at == s->out_size) return false;
            s->out[s->out_at++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 256) return true; // The block's end
        const int l = symbol - 257;
        if (l >= 29) return false;
        const size_t length = length_base[l] + take_bits(s, length_extra[l]);
        const int d = decode(s, distances);
        if (d < 0 || d >= 30) return false;
        const size_t distance = distance_base[d] + take_bits(s, distance_extra[d]);
        if (s->broken || distance > s->out_at || length > s->out_size - s->out_at) return false;
        for (size_t i = 0; i < length; i++, s->out_at++) s->out[s->out_at] = s->out[s->out_at - distance];
    }
}

static bool inflate_stored(inflater *s)
{
    s->bits = 0; // To the next byte
    s->bit_count = 0;
    if (s->in_size - s->in_at < 4) return false;
    const size_t length = (size_t)s->in[s->in_at] | (size_t)s->in[s->in_at + 1] << 8;
    const size_t check = (size_t)s->in[s->in_at + 2] | (size_t)s->in[s->in_at + 3] << 8;
    s->in_at += 4;
    if ((length ^ 0xffff) != check || length > s->in_size - s->in_at || length > s->out_size - s->out_at) return false;
    memcpy(s->out + s->out_at, s->in + s->in_at, length);
    s->in_at += length;
    s->out_at += length;
    return true;
}

static bool inflate_fixed(inflater *s)
{
    static huffman lengths, distances;
    static bool built;
    if (!built) {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        build_huffman(&lengths, l, 288);
        uint8_t d[30];
        for (int i = 0; i < 30; i++) d[i] = 5;
        build_huffman(&distances, d, 30);
        built = true;
    }
    return inflate_codes(s, &lengths, &distances);
}

// A block that gives its codes: their lengths, coded themselves.
static bool inflate_dynamic(inflater *s)
{
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int literal_count = (int)take_bits(s, 5) + 257;
    const int distance_count = (int)take_bits(s, 5) + 1;
    const int code_count = (int)take_bits(s, 4) + 4;
    if (literal_count > 286 || distance_count > 30) return false;
    uint8_t lengths[320] = {0};
    for (int i = 0; i < code_count; i++) lengths[order[i]] = (uint8_t)take_bits(s, 3);
    huffman code;
    if (!build_huffman(&code, lengths, 19)) return false;
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < literal_count + distance_count;) {
        const int symbol = decode(s, &code);
        if (symbol < 0) return false;
        if (symbol < 16) {
            lengths[i++] = (uint8_t)symbol;
            continue;
        }
        uint8_t repeated = 0;
        int times;
        if (symbol == 16) {
            if (i == 0) return false;
            repeated = lengths[i - 1];
            times = 3 + (int)take_bits(s, 2);
        } else if (symbol == 17) {
            times = 3 + (int)take_bits(s, 3);
        } else {
            times = 11 + (int)take_bits(s, 7);
        }
        if (i + times > literal_count + distance_count) return false;
        while (times--) lengths[i++] = repeated;
    }
    if (lengths[256] == 0) return false; // No way to end the block
    huffman literals, distances;
    if (!build_huffman(&literals, lengths, literal_count) || !build_huffman(&distances, lengths + literal_count, distance_count)) {
        return false;
    }
    return inflate_codes(s, &literals, &distances);
}

bool unzip_inflate(const uint8_t *in, const size_t in_size, uint8_t *out, const size_t out_size)
{
    inflater s = {in, in_size, 0, 0, 0, out, out_size, 0, false};
    bool last = false;
    while (!last) {
        last = take_bits(&s, 1) != 0;
        const uint32_t kind = take_bits(&s, 2);
        bool ok;
        if (kind == 0) ok = inflate_stored(&s);
        else if (kind == 1) ok = inflate_fixed(&s);
        else if (kind == 2) ok = inflate_dynamic(&s);
        else ok = false;
        if (!ok || s.broken) return false;
    }
    return s.out_at == out_size;
}

// ---------------------------------------------------------------------------
// The archive: its central directory lists the entries, each with its local
// header and data where the directory says.

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

static bool read_at(FILE *f, const uint64_t at, void *out, const size_t size)
{
    return seek64(f, (int64_t)at, SEEK_SET) == 0 && fread(out, 1, size, f) == size;
}

static bool fail(const char *archive, const char *why)
{
    fprintf(stderr, "tide: can't unpack %s: %s\n", archive, why);
    return false;
}

// Where the directory starts, how big it is and how many entries it lists,
// from the end of the archive (ZIP64's, when the counts don't fit).
static bool find_directory(FILE *f, const char *archive, uint64_t *start, uint64_t *size, uint64_t *count)
{
    if (seek64(f, 0, SEEK_END) != 0) return fail(archive, "can't read it");
    const uint64_t length = (uint64_t)tell64(f);
    const size_t tail = length < 65536 + 22 ? (size_t)length : 65536 + 22; // The end, and a comment that long
    uint8_t *buf = malloc(tail);
    if (!buf || !read_at(f, length - tail, buf, tail)) {
        free(buf);
        return fail(archive, "can't read it");
    }
    size_t end = SIZE_MAX;
    for (size_t i = tail - 22 + 1; i-- > 0;) {
        if (get32(buf + i) == 0x06054b50) {
            end = i;
            break;
        }
    }
    if (end == SIZE_MAX) {
        free(buf);
        return fail(archive, "it isn't a zip");
    }
    *count = get16(buf + end + 10);
    *size = get32(buf + end + 12);
    *start = get32(buf + end + 16);
    const uint64_t end_at = length - tail + end;
    free(buf);
    if (*count == 0xffff || *size == 0xffffffffu || *start == 0xffffffffu) { // ZIP64: its locator is just before
        uint8_t locator[20], record[56];
        if (end_at < 20 || !read_at(f, end_at - 20, locator, 20) || get32(locator) != 0x07064b50) {
            return fail(archive, "its ZIP64 end is missing");
        }
        if (!read_at(f, get64(locator + 8), record, 56) || get32(record) != 0x06064b50) {
            return fail(archive, "its ZIP64 end is broken");
        }
        *count = get64(record + 32);
        *size = get64(record + 40);
        *start = get64(record + 48);
    }
    return true;
}

// An entry's sizes and offset past 4 GiB are in its ZIP64 extra field.
static void zip64_sizes(const uint8_t *extra, const size_t extra_size, uint64_t *uncompressed, uint64_t *compressed,
                        uint64_t *offset)
{
    for (size_t at = 0; at + 4 <= extra_size;) {
        const uint32_t id = get16(extra + at), size = get16(extra + at + 2);
        if (at + 4 + size > extra_size) return;
        if (id == 0x0001) {
            const uint8_t *p = extra + at + 4;
            size_t left = size;
            if (*uncompressed == 0xffffffffu && left >= 8) *uncompressed = get64(p), p += 8, left -= 8;
            if (*compressed == 0xffffffffu && left >= 8) *compressed = get64(p), p += 8, left -= 8;
            if (*offset == 0xffffffffu && left >= 8) *offset = get64(p);
            return;
        }
        at += 4 + size;
    }
}

static bool extract(FILE *f, const char *archive, const char *dir, const char *to, const uint64_t local,
                    const uint64_t compressed, const uint64_t uncompressed, const uint32_t method, const uint32_t crc,
                    const uint32_t mode)
{
    uint8_t header[30];
    if (!read_at(f, local, header, 30) || get32(header) != 0x04034b50) return fail(archive, "an entry's header is broken");
    const uint64_t data_at = local + 30 + get16(header + 26) + get16(header + 28);
    if (compressed > ((uint64_t)1 << 32) || uncompressed > ((uint64_t)1 << 32)) return fail(archive, "an entry is too big");
    uint8_t *in = malloc(compressed ? (size_t)compressed : 1);
    uint8_t *out = method == 0 ? in : malloc(uncompressed ? (size_t)uncompressed : 1);
    bool ok = in && out && (compressed == 0 || read_at(f, data_at, in, (size_t)compressed));
    if (ok && method == 8) ok = unzip_inflate(in, (size_t)compressed, out, (size_t)uncompressed);
    else if (ok && method == 0) ok = compressed == uncompressed;
    else ok = false;
    if (ok) ok = rtc_crc32(out, (size_t)uncompressed) == crc;
    char *path = path_join(dir, to);
    if (ok) {
        char *folder = path_dir(path);
        sys_mkdirs(folder);
        free(folder);
        ok = sys_write_file(path, (const char *)out, (size_t)uncompressed);
#ifndef _WIN32
        if (ok && (mode & 0111)) chmod(path, (mode_t)(mode & 0777));
#else
        (void)mode;
#endif
        if (!ok) fprintf(stderr, "tide: can't write %s\n", path);
    } else {
        fail(archive, method != 0 && method != 8 ? "an entry is packed in a way tide doesn't read" : "an entry is damaged");
    }
    free(path);
    if (out != in) free(out);
    free(in);
    return ok;
}

bool unzip(const char *archive, const char *dir, const unzip_filter wanted, void *user)
{
    FILE *f = fopen(archive, "rb");
    if (!f) return fail(archive, "can't open it");
    uint64_t start, size, count;
    if (!find_directory(f, archive, &start, &size, &count)) {
        fclose(f);
        return false;
    }
    uint8_t *directory = size < ((uint64_t)1 << 31) ? malloc((size_t)size ? (size_t)size : 1) : NULL;
    if (!directory || !read_at(f, start, directory, (size_t)size)) {
        free(directory);
        fclose(f);
        return fail(archive, "can't read its directory");
    }
    bool ok = true;
    size_t at = 0;
    for (uint64_t i = 0; ok && i < count; i++) {
        if (at + 46 > size || get32(directory + at) != 0x02014b50) {
            ok = fail(archive, "its directory is broken");
            break;
        }
        const uint8_t *e = directory + at;
        const uint32_t method = get16(e + 10), crc = get32(e + 16);
        uint64_t compressed = get32(e + 20), uncompressed = get32(e + 24), local = get32(e + 42);
        const size_t name_size = get16(e + 28), extra_size = get16(e + 30), comment_size = get16(e + 32);
        if (at + 46 + name_size + extra_size + comment_size > size) {
            ok = fail(archive, "its directory is broken");
            break;
        }
        zip64_sizes(e + 46 + name_size, extra_size, &uncompressed, &compressed, &local);
        char name[1024];
        const size_t n = name_size < sizeof name - 1 ? name_size : sizeof name - 1;
        memcpy(name, e + 46, n);
        name[n] = '\0';
        for (char *c = name; *c; c++) {
            if (*c == '\\') *c = '/';
        }
        char to[1024];
        // Folders are made for the files in them; nothing climbs out of `dir`.
        const bool folder = n > 0 && name[n - 1] == '/';
        if (!folder && !strstr(name, "..") && wanted(user, name, to, sizeof to)) {
            const uint32_t mode = get16(e + 4) >> 8 == 3 ? get32(e + 38) >> 16 : 0; // Unix attributes, from Unix zips
            ok = extract(f, archive, dir, to, local, compressed, uncompressed, method, crc, mode);
        }
        at += 46 + name_size + extra_size + comment_size;
    }
    free(directory);
    fclose(f);
    return ok;
}
