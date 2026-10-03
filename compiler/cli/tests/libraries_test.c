// Which platform and CPU a prebuilt library was built for, from its bytes:
// the headers of each format, built here in memory.

#include <stdio.h>
#include <string.h>

#include "libraries.h"
#include "tide_test.h"

typedef struct bytes {
    unsigned char data[4096];
    size_t len;
} bytes;

static size_t read_bytes(void *user, const uint64_t offset, void *buf, const size_t n)
{
    const bytes *b = user;
    if (offset >= b->len) return 0;
    const size_t count = b->len - offset < n ? (size_t)(b->len - offset) : n;
    memcpy(buf, b->data + offset, count);
    return count;
}

static lib_info identify(bytes *b)
{
    return lib_identify(read_bytes, b);
}

static void put(bytes *b, const void *data, const size_t n)
{
    memcpy(b->data + b->len, data, n);
    b->len += n;
}

static void put_at(bytes *b, const size_t at, const void *data, const size_t n)
{
    memcpy(b->data + at, data, n);
    if (at + n > b->len) b->len = at + n;
}

static void le16_at(bytes *b, const size_t at, const unsigned v)
{
    const unsigned char p[2] = {(unsigned char)v, (unsigned char)(v >> 8)};
    put_at(b, at, p, 2);
}

static void le32_at(bytes *b, const size_t at, const unsigned long v)
{
    const unsigned char p[4] = {(unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24)};
    put_at(b, at, p, 4);
}

static void be32_at(bytes *b, const size_t at, const unsigned long v)
{
    const unsigned char p[4] = {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v};
    put_at(b, at, p, 4);
}

// An archive member: its 60-byte header, then its data, padded to even.
static void member(bytes *b, const char *name, const void *data, const size_t n)
{
    char header[61];
    snprintf(header, sizeof header, "%-16s%-12s%-6s%-6s%-8s%-10zu`\n", name, "0", "0", "0", "644", n);
    put(b, header, 60);
    put(b, data, n);
    if (n & 1) put(b, "\n", 1);
}

// The first bytes of an ELF object: 64-bit, little-endian, of `type` and `machine`.
static void elf(unsigned char *out, const unsigned type, const unsigned machine)
{
    memset(out, 0, 64);
    memcpy(out, "\x7f" "ELF", 4);
    out[4] = 2; // 64-bit
    out[5] = 1; // Little-endian
    out[16] = (unsigned char)type;
    out[18] = (unsigned char)machine;
}

static void macho(unsigned char *out, const unsigned long cpu, const unsigned filetype)
{
    memset(out, 0, 32);
    const unsigned char magic[4] = {0xCF, 0xFA, 0xED, 0xFE};
    memcpy(out, magic, 4);
    out[4] = (unsigned char)cpu;
    out[7] = (unsigned char)(cpu >> 24);
    out[12] = (unsigned char)filetype;
}

TIDE_TEST(cli_libraries_gnu_archive_of_elf)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "/", "\0\0\0\0", 4);             // The symbol table
    member(&b, "//", "a_long_name.o/\n", 15);   // Long names
    unsigned char object[64];
    elf(object, 1, 62); // ET_REL, x86-64
    member(&b, "/0", object, sizeof object);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_LINUX);
    TIDE_CHECK(info.cpus == LIB_X64);
    TIDE_CHECK(!info.dynamic);
}

TIDE_TEST(cli_libraries_microsoft_import_library)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "/", "\0\0\0\0", 4); // The two linker members
    member(&b, "/", "\0\0\0\0", 4);
    member(&b, "//", "steam_api64.dll\0", 16);
    unsigned char import[20] = {0};
    import[2] = 0xFF;
    import[3] = 0xFF;
    import[6] = 0x64; // AMD64
    import[7] = 0x86;
    member(&b, "steam_api64.dll/", import, sizeof import);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_WINDOWS);
    TIDE_CHECK(info.cpus == LIB_X64);
    TIDE_CHECK(!info.dynamic);
}

TIDE_TEST(cli_libraries_mingw_archive_of_coff)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "/", "\0\0\0\0", 4);
    unsigned char object[20] = {0x64, 0x86}; // COFF, AMD64
    member(&b, "noise.o/", object, sizeof object);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_WINDOWS);
    TIDE_CHECK(info.cpus == LIB_X64);
}

TIDE_TEST(cli_libraries_bsd_archive_of_macho)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    // BSD long names: "#1/<length>", the name first in the data.
    char symdef[28] = "__.SYMDEF SORTED\0\0\0\0";
    memset(symdef + 20, 0, 8);
    member(&b, "#1/20", symdef, sizeof symdef);
    unsigned char object[12 + 32];
    memcpy(object, "noise.o\0\0\0\0\0", 12);
    macho(object + 12, 0x0100000Cul, 1); // arm64, an object
    member(&b, "#1/12", object, sizeof object);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_MACOS);
    TIDE_CHECK(info.cpus == LIB_ARM64);
    TIDE_CHECK(!info.dynamic);
}

TIDE_TEST(cli_libraries_wasm_archive)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "/", "\0\0\0\0", 4);
    member(&b, "noise.o/", "\0asm\x01\0\0\0", 8);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_WEB);
    TIDE_CHECK(info.cpus == LIB_WASM32);
}

TIDE_TEST(cli_libraries_archive_of_bitcode_is_unknown)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "noise.o/", "BC\xC0\xDE\0\0\0\0", 8);
    TIDE_CHECK(identify(&b).platform == LIB_UNKNOWN);
}

TIDE_TEST(cli_libraries_dll)
{
    bytes b = {0};
    put(&b, "MZ", 2);
    le32_at(&b, 0x3C, 0x80);
    put_at(&b, 0x80, "PE\0\0", 4);
    le16_at(&b, 0x84, 0x8664);
    b.len = 0x100;
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_WINDOWS);
    TIDE_CHECK(info.cpus == LIB_X64);
    TIDE_CHECK(info.dynamic);
}

TIDE_TEST(cli_libraries_shared_object)
{
    bytes b = {0};
    unsigned char so[64];
    elf(so, 3, 183); // ET_DYN, AArch64
    put(&b, so, sizeof so);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_LINUX);
    TIDE_CHECK(info.cpus == LIB_ARM64);
    TIDE_CHECK(info.dynamic);
}

static void le64_at(bytes *b, const size_t at, const unsigned long long v)
{
    le32_at(b, at, (unsigned long)(v & 0xFFFFFFFFu));
    le32_at(b, at + 4, (unsigned long)(v >> 32));
}

// A 64-bit ELF file's header, with `count` program headers after it.
static void elf_file(bytes *b, const unsigned type, const unsigned machine, const unsigned count)
{
    unsigned char h[64];
    elf(h, type, machine);
    put(b, h, sizeof h);
    le64_at(b, 32, 64); // Where the program headers are
    le16_at(b, 54, 56); // ...and the size of each
    le16_at(b, 56, count);
}

enum { LOAD = 1, DYNAMIC = 2, NOTE = 4 };

// The `index`th program header: `size` bytes at `offset`, loaded at `address`.
static void segment(bytes *b, const unsigned index, const unsigned type, const size_t offset,
                    const unsigned long long address, const size_t size, const unsigned long long align)
{
    const size_t at = 64 + 56 * (size_t)index;
    le32_at(b, at, type);
    le64_at(b, at + 8, offset);
    le64_at(b, at + 16, address);
    le64_at(b, at + 32, size);
    le64_at(b, at + 40, size);
    le64_at(b, at + 48, align);
}

// A note at `at`, its name and its description each padded to `align` from
// the note's start; returns where the next one goes.
static size_t note(bytes *b, const size_t at, const char *name, const unsigned type, const void *description,
                   const size_t size, const size_t align)
{
    const size_t name_size = strlen(name) + 1;
    le32_at(b, at, (unsigned long)name_size);
    le32_at(b, at + 4, (unsigned long)size);
    le32_at(b, at + 8, type);
    put_at(b, at + 12, name, name_size);
    const size_t start = (12 + name_size + align - 1) / align * align;
    put_at(b, at + start, description, size);
    const size_t end = (start + size + align - 1) / align * align;
    if (at + end > b->len) b->len = at + end;
    return at + end;
}

// Android's note, as the NDK's crtbegin_so.o has it: the API level the
// library was built for, then the NDK and its build, 64 bytes each.
static size_t android_note(bytes *b, const size_t at)
{
    unsigned char ident[4 + 64 + 64] = {29};
    memcpy(ident + 4, "r30", 3);
    memcpy(ident + 68, "16248370", 8);
    return note(b, at, "Android", 1, ident, sizeof ident, 4);
}

// The dynamic section at 0x400, whose strings are at 0x500 in the file and
// 0x10500 as loaded: what the library needs, and its name if it has one.
static void dynamic(bytes *b, const char *needs, const char *soname)
{
    char strings[64] = {0};
    const size_t needs_at = 1;
    const size_t soname_at = needs_at + strlen(needs) + 1;
    memcpy(strings + needs_at, needs, strlen(needs));
    if (soname) memcpy(strings + soname_at, soname, strlen(soname));
    put_at(b, 0x500, strings, sizeof strings);
    size_t at = 0x400;
    le64_at(b, at, 1); // DT_NEEDED
    le64_at(b, at + 8, needs_at);
    at += 16;
    if (soname) {
        le64_at(b, at, 14); // DT_SONAME
        le64_at(b, at + 8, soname_at);
        at += 16;
    }
    le64_at(b, at, 5); // DT_STRTAB
    le64_at(b, at + 8, 0x10500);
    le64_at(b, at + 16, 0); // DT_NULL
    le64_at(b, at + 24, 0);
}

// A shared library the NDK linked: Android's note says so, among the others
// its notes hold, and its segments are aligned to 16 KiB pages.
TIDE_TEST(cli_libraries_android_shared_object)
{
    bytes b = {0};
    elf_file(&b, 3, 183, 5); // ET_DYN, AArch64
    static const unsigned char id[20] = {1, 2, 3};
    static const unsigned char property[16] = {0, 0, 0, 0xC0, 4, 0, 0, 0, 3};
    const size_t notes_end = android_note(&b, note(&b, 0x200, "GNU", 3, id, sizeof id, 4)); // After its build ID
    const size_t property_end = note(&b, 0x300, "GNU", 5, property, sizeof property, 8);
    dynamic(&b, "libc.so", "libnoise.so");
    segment(&b, 0, LOAD, 0, 0x10000, 0x600, 0x4000);
    segment(&b, 1, LOAD, 0x600, 0x14600, 0, 0x4000);
    segment(&b, 2, NOTE, 0x300, 0x10300, property_end - 0x300, 8);
    segment(&b, 3, NOTE, 0x200, 0x10200, notes_end - 0x200, 4);
    segment(&b, 4, DYNAMIC, 0x400, 0x10400, 64, 8);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_ANDROID);
    TIDE_CHECK(info.cpus == LIB_ARM64);
    TIDE_CHECK(info.dynamic);
    TIDE_CHECK(info.page_size == 16384);
    TIDE_CHECK(strcmp(info.name, "libnoise.so") == 0);
}

// One linked by an older NDK, for 4 KiB pages, and with no name of its own
// (Go's are so): the game asks for it by its file's name.
TIDE_TEST(cli_libraries_android_shared_object_for_small_pages)
{
    bytes b = {0};
    elf_file(&b, 3, 62, 4); // ET_DYN, x86-64
    const size_t notes_end = android_note(&b, 0x200);
    dynamic(&b, "libc.so", NULL);
    segment(&b, 0, NOTE, 0x200, 0x10200, notes_end - 0x200, 4);
    segment(&b, 1, LOAD, 0, 0x10000, 0x600, 0x1000);
    segment(&b, 2, LOAD, 0x600, 0x11600, 0, 0x4000);
    segment(&b, 3, DYNAMIC, 0x400, 0x10400, 48, 8);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_ANDROID);
    TIDE_CHECK(info.cpus == LIB_X64);
    TIDE_CHECK(info.page_size == 4096); // The smallest of its segments' alignments
    TIDE_CHECK(info.name[0] == '\0');
}

// Linux's has no such note, whatever it needs: musl's C library is libc.so
// too, as Android's is.
TIDE_TEST(cli_libraries_linux_shared_object_has_no_android_note)
{
    static const char *const needs[] = {"libc.so.6", "libc.so"};
    for (int i = 0; i < 2; i++) {
        bytes b = {0};
        elf_file(&b, 3, 62, 3);
        static const unsigned char id[20] = {1, 2, 3};
        // Another's note of the same type, and one named like Android's of another type
        size_t notes_end = note(&b, 0x200, "GNU", 1, id, 16, 4);
        notes_end = note(&b, notes_end, "Android", 4, id, 8, 4);
        dynamic(&b, needs[i], "libnoise.so");
        segment(&b, 0, LOAD, 0, 0x10000, 0x600, 0x1000);
        segment(&b, 1, NOTE, 0x200, 0x10200, notes_end - 0x200, 4);
        segment(&b, 2, DYNAMIC, 0x400, 0x10400, 64, 8);
        const lib_info info = identify(&b);
        TIDE_CHECK(info.platform == LIB_LINUX);
        TIDE_CHECK(info.cpus == LIB_X64);
        TIDE_CHECK(info.dynamic);
        TIDE_CHECK(info.page_size == 4096);
        TIDE_CHECK(strcmp(info.name, "libnoise.so") == 0);
    }
}

// A static library's objects are the same for Linux and Android (the note
// is in the C runtime's own objects, which a shared library is linked with),
// so an ELF one is Linux's.
TIDE_TEST(cli_libraries_archive_of_elf_is_linux)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "/", "\0\0\0\0", 4);
    unsigned char object[64];
    elf(object, 1, 183); // ET_REL, AArch64
    member(&b, "noise.o/", object, sizeof object);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_LINUX);
    TIDE_CHECK(info.cpus == LIB_ARM64);
    TIDE_CHECK(!info.dynamic);
}

// A shared library cut short, or whose name never ends, is one tide can't read.
TIDE_TEST(cli_libraries_broken_shared_object_is_unknown)
{
    bytes cut = {0};
    elf_file(&cut, 3, 183, 3);
    segment(&cut, 0, LOAD, 0, 0x10000, 0x600, 0x4000); // The other two headers aren't there
    TIDE_CHECK(identify(&cut).platform == LIB_UNKNOWN);

    bytes endless = {0};
    elf_file(&endless, 3, 183, 2);
    dynamic(&endless, "libc.so", "libnoise.so");
    segment(&endless, 0, LOAD, 0, 0x10000, 0x600, 0x4000);
    segment(&endless, 1, DYNAMIC, 0x400, 0x10400, 64, 8);
    memset(endless.data + 0x500 + 9, 'x', 0x200); // Its name runs on past any file's
    endless.len = 0x700;
    TIDE_CHECK(identify(&endless).platform == LIB_UNKNOWN);

    bytes nowhere = {0};
    elf_file(&nowhere, 3, 183, 2);
    dynamic(&nowhere, "libc.so", "libnoise.so");
    segment(&nowhere, 0, LOAD, 0, 0x20000, 0x600, 0x4000); // Its strings' address isn't in what it loads
    segment(&nowhere, 1, DYNAMIC, 0x400, 0x20400, 64, 8);
    TIDE_CHECK(identify(&nowhere).platform == LIB_UNKNOWN);
}

TIDE_TEST(cli_libraries_universal_dylib)
{
    bytes b = {0};
    be32_at(&b, 0, 0xCAFEBABEul);
    be32_at(&b, 4, 2);
    be32_at(&b, 8, 0x01000007ul); // x86_64, its slice at 0x100
    be32_at(&b, 16, 0x100);
    be32_at(&b, 28, 0x0100000Cul); // arm64
    be32_at(&b, 36, 0x200);
    unsigned char slice[32];
    macho(slice, 0x01000007ul, 6); // A dylib
    put_at(&b, 0x100, slice, sizeof slice);
    macho(slice, 0x0100000Cul, 6);
    put_at(&b, 0x200, slice, sizeof slice);
    const lib_info info = identify(&b);
    TIDE_CHECK(info.platform == LIB_MACOS);
    TIDE_CHECK(info.cpus == (LIB_X64 | LIB_ARM64));
    TIDE_CHECK(info.dynamic);
}

TIDE_TEST(cli_libraries_other_files_are_unknown)
{
    bytes b = {0};
    const char script[] = "/* GNU ld script */\nGROUP ( /lib/libc.so.6 )\n";
    put(&b, script, sizeof script - 1);
    TIDE_CHECK(identify(&b).platform == LIB_UNKNOWN);
    bytes java = {0};
    be32_at(&java, 0, 0xCAFEBABEul);
    be32_at(&java, 4, 0x34); // A class file's version, not a count of slices
    TIDE_CHECK(identify(&java).platform == LIB_UNKNOWN);
    bytes empty = {0};
    TIDE_CHECK(identify(&empty).platform == LIB_UNKNOWN);
}

static void put_name(bytes *b, const char *name)
{
    const unsigned char n = (unsigned char)strlen(name);
    put(b, &n, 1);
    put(b, name, n);
}

static void found_import(void *user, const char *name, const size_t len)
{
    char *out = user;
    strncat(out, name, len);
    strcat(out, " ");
}

TIDE_TEST(cli_wasm_function_imports_of_a_module)
{
    // The imports: functions from env and tide, a memory, a global and a table.
    bytes imports = {0};
    put(&imports, "\x06", 1);
    put_name(&imports, "env");
    put_name(&imports, "glClear");
    put(&imports, "\x00\x01", 2);
    put_name(&imports, "env");
    put_name(&imports, "memory");
    put(&imports, "\x02\x01\x02\x80\x01", 5); // Limits with a maximum: 2, 128
    put_name(&imports, "tide");
    put_name(&imports, "run");
    put(&imports, "\x00\x00", 2);
    put_name(&imports, "env");
    put_name(&imports, "__stack_pointer");
    put(&imports, "\x03\x7f\x01", 3); // i32, mutable
    put_name(&imports, "env");
    put_name(&imports, "__indirect_function_table");
    put(&imports, "\x01\x70\x00\x01", 4); // funcref, at least 1
    put_name(&imports, "env");
    put_name(&imports, "hello_dll");
    put(&imports, "\x00\x02", 2);

    bytes wasm = {0};
    put(&wasm, "\0asm\x01\0\0\0", 8);
    put(&wasm, "\x01\x04\x01\x60\x00\x00", 6); // A type section before it: () -> ()
    const unsigned char header[2] = {2, (unsigned char)imports.len};
    put(&wasm, header, 2);
    put(&wasm, imports.data, imports.len);

    char names[256] = "";
    TIDE_REQUIRE(wasm_function_imports(wasm.data, wasm.len, "env", found_import, names));
    TIDE_CHECK(strcmp(names, "glClear hello_dll ") == 0);
    TIDE_CHECK(!wasm_function_imports((const unsigned char *)"MZ", 2, "env", found_import, names));
    wasm.data[wasm.len - 1] = 0x80; // Cut short: a type index that never ends
    TIDE_CHECK(!wasm_function_imports(wasm.data, wasm.len, "env", found_import, names));
}
