#include "libraries.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Libraries by their formats: static ones are archives (`!<arch>`) of object
// files, whose first object says what they're for; dynamic ones are PE (.dll),
// ELF (.so) or Mach-O (.dylib) files. Only headers are read, so a library of
// any size costs a few small reads.

static uint16_t le16(const unsigned char *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

// A COFF machine's CPU, or 0 if it isn't one.
static unsigned coff_cpu(const uint16_t machine)
{
    switch (machine) {
    case 0x8664: return LIB_X64;
    case 0xAA64: return LIB_ARM64;
    case 0x014C: // x86
    case 0x01C4: // ARM
    case 0xA641: // ARM64EC
        return LIB_OTHER_CPU;
    default: return 0;
    }
}

static unsigned elf_cpu(const uint16_t machine)
{
    return machine == 62 ? LIB_X64 : machine == 183 ? LIB_ARM64 : LIB_OTHER_CPU;
}

static unsigned macho_cpu(const uint32_t cpu)
{
    return cpu == 0x01000007u ? LIB_X64 : cpu == 0x0100000Cu ? LIB_ARM64 : LIB_OTHER_CPU;
}

#define MACHO_64 0xFEEDFACFu // As its first four bytes read little-endian
#define MACHO_DYLIB 6

// An object file, from its first bytes: a static library's member, or a
// whole .so or .dylib.
static lib_info identify_object(const unsigned char *h, const size_t n)
{
    lib_info info = {0};
    if (n >= 20 && memcmp(h, "\x7f" "ELF", 4) == 0 && h[5] == 1) { // Little-endian
        info.platform = LIB_LINUX;
        info.cpus = elf_cpu(le16(h + 18));
        info.dynamic = le16(h + 16) == 3; // ET_DYN
    } else if (n >= 16 && le32(h) == MACHO_64) {
        info.platform = LIB_MACOS;
        info.cpus = macho_cpu(le32(h + 4));
        info.dynamic = le32(h + 12) == MACHO_DYLIB;
    } else if (n >= 8 && memcmp(h, "\0asm", 4) == 0) {
        info.platform = LIB_WEB;
        info.cpus = LIB_WASM32;
    } else if (n >= 8 && le16(h) == 0 && le16(h + 2) == 0xFFFF) {
        // An import library's member, or a big COFF object: the machine is later
        const unsigned cpu = coff_cpu(le16(h + 6));
        if (cpu) info = (lib_info){LIB_WINDOWS, cpu, false};
    } else if (n >= 20) {
        const unsigned cpu = coff_cpu(le16(h));
        if (cpu) info = (lib_info){LIB_WINDOWS, cpu, false};
    }
    return info;
}

// A static library: the first member that's an object, skipping the symbol
// tables and long names of GNU, BSD and Microsoft archives, and members it
// can't read (LLVM bitcode).
static lib_info identify_archive(const lib_read_fn read, void *user)
{
    uint64_t at = 8; // After "!<arch>\n"
    for (int member = 0; member < 256; member++) {
        char header[60];
        if (read(user, at, header, sizeof header) != sizeof header || header[58] != '`' || header[59] != '\n') break;
        char size_text[11] = {0};
        memcpy(size_text, header + 48, 10);
        const uint64_t size = strtoull(size_text, NULL, 10);
        uint64_t data = at + 60;
        uint64_t data_size = size;
        bool skip = false;
        if (memcmp(header, "#1/", 3) == 0) {
            // BSD: the name comes first in the data, "#1/<its length>"
            char length_text[14] = {0};
            memcpy(length_text, header + 3, 13);
            const uint64_t name_length = strtoull(length_text, NULL, 10);
            char name[10] = {0};
            read(user, data, name, name_length < 9 ? (size_t)name_length : 9);
            skip = memcmp(name, "__.SYMDEF", 9) == 0 || name_length > data_size;
            data += name_length;
            data_size -= skip ? 0 : name_length;
        } else {
            // GNU and Microsoft: "/" and "/SYM64/" symbol tables, "//" long names
            skip = (header[0] == '/' && (header[1] == ' ' || header[1] == '/' || memcmp(header, "/SYM64/", 7) == 0))
                || memcmp(header, "__.SYMDEF", 9) == 0;
        }
        if (!skip) {
            unsigned char h[64];
            const size_t n = read(user, data, h, data_size < sizeof h ? (size_t)data_size : sizeof h);
            lib_info info = identify_object(h, n);
            if (info.platform != LIB_UNKNOWN) {
                info.dynamic = false;
                return info;
            }
        }
        at += 60 + size + (size & 1); // The next header: members are padded to an even size
    }
    return (lib_info){0};
}

// A universal macOS library: a slice per CPU.
static lib_info identify_universal(const lib_read_fn read, void *user, const unsigned char *h)
{
    const uint32_t count = be32(h + 4);
    if (count == 0 || count > 16) return (lib_info){0}; // Java's class files start the same
    lib_info info = {LIB_MACOS, 0, false};
    for (uint32_t i = 0; i < count; i++) {
        unsigned char arch[20];
        if (read(user, 8 + 20 * (uint64_t)i, arch, sizeof arch) != sizeof arch) return (lib_info){0};
        info.cpus |= macho_cpu(be32(arch));
        if (i > 0) continue;
        unsigned char slice[16];
        const size_t n = read(user, be32(arch + 8), slice, sizeof slice);
        info.dynamic = n == sizeof slice && le32(slice) == MACHO_64 && le32(slice + 12) == MACHO_DYLIB;
    }
    return info;
}

lib_info lib_identify(const lib_read_fn read, void *user)
{
    unsigned char h[64];
    const size_t n = read(user, 0, h, sizeof h);
    if (n >= 8 && memcmp(h, "!<arch>\n", 8) == 0) return identify_archive(read, user);
    if (n >= 8 && be32(h) == 0xCAFEBABEu) return identify_universal(read, user, h);
    if (n >= 64 && h[0] == 'M' && h[1] == 'Z') { // A .dll: the PE header is where 0x3C says
        unsigned char pe[6];
        if (read(user, le32(h + 0x3C), pe, sizeof pe) != sizeof pe || memcmp(pe, "PE\0\0", 4) != 0) return (lib_info){0};
        const unsigned cpu = coff_cpu(le16(pe + 4));
        return (lib_info){LIB_WINDOWS, cpu ? cpu : LIB_OTHER_CPU, true};
    }
    return identify_object(h, n);
}

static size_t read_file(void *user, const uint64_t offset, void *buf, const size_t n)
{
    FILE *f = user;
    if (offset > 0x7FFFFFFF || fseek(f, (long)offset, SEEK_SET) != 0) return 0;
    return fread(buf, 1, n, f);
}

lib_info lib_identify_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return (lib_info){0};
    const lib_info info = lib_identify(read_file, f);
    fclose(f);
    return info;
}

// ---------------------------------------------------------------------------
// WebAssembly imports

typedef struct wasm_reader {
    const unsigned char *p;
    const unsigned char *end;
    bool ok;
} wasm_reader;

static uint64_t leb(wasm_reader *r)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) break;
        const unsigned char byte = *r->p++;
        v |= (uint64_t)(byte & 0x7F) << shift;
        if (!(byte & 0x80)) return v;
    }
    r->ok = false;
    return 0;
}

static unsigned char byte(wasm_reader *r)
{
    if (r->p >= r->end) {
        r->ok = false;
        return 0;
    }
    return *r->p++;
}

// A name: its length, then its bytes.
static const unsigned char *name(wasm_reader *r, size_t *len)
{
    const uint64_t n = leb(r);
    if (!r->ok || n > (uint64_t)(r->end - r->p)) {
        r->ok = false;
        return NULL;
    }
    const unsigned char *start = r->p;
    r->p += n;
    *len = (size_t)n;
    return start;
}

// A table's or memory's limits: flags, then the minimum and maybe a maximum.
static void limits(wasm_reader *r)
{
    const unsigned char flags = byte(r);
    leb(r);
    if (flags & 1) leb(r);
}

bool wasm_function_imports(const unsigned char *wasm, const size_t size, const char *module, const wasm_import_fn found,
                           void *user)
{
    if (size < 8 || memcmp(wasm, "\0asm", 4) != 0) return false;
    wasm_reader r = {wasm + 8, wasm + size, true};
    while (r.ok && r.p < r.end) {
        const unsigned char id = byte(&r);
        const uint64_t length = leb(&r);
        if (!r.ok || length > (uint64_t)(r.end - r.p)) return false;
        if (id != 2) { // Not the imports
            r.p += length;
            continue;
        }
        wasm_reader s = {r.p, r.p + length, true};
        const uint64_t count = leb(&s);
        for (uint64_t i = 0; i < count && s.ok; i++) {
            size_t module_len = 0;
            size_t field_len = 0;
            const unsigned char *from = name(&s, &module_len);
            const unsigned char *field = name(&s, &field_len);
            switch (byte(&s)) {
            case 0: // A function, of a type
                leb(&s);
                if (s.ok && module_len == strlen(module) && memcmp(from, module, module_len) == 0) {
                    found(user, (const char *)field, field_len);
                }
                break;
            case 1: // A table: its element type and limits
                byte(&s);
                limits(&s);
                break;
            case 2: limits(&s); break; // A memory
            case 3: // A global: its type and whether it changes
                byte(&s);
                byte(&s);
                break;
            case 4: // A tag: its attribute and type
                byte(&s);
                leb(&s);
                break;
            default: s.ok = false; break;
            }
        }
        return s.ok;
    }
    return r.ok;
}
