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

// A WebAssembly object that calls `function` without defining it: an import
// from env.
static void put_object(bytes *b, const char *name, const char *function)
{
    bytes wasm = {0};
    put(&wasm, "\0asm\x01\0\0\0", 8);
    const unsigned char header[3] = {2, (unsigned char)(strlen(function) + 8), 1}; // The imports: their size, and one
    put(&wasm, header, 3);
    put_name(&wasm, "env");
    put_name(&wasm, function);
    put(&wasm, "\x00\x00", 2);
    member(b, name, wasm.data, wasm.len);
}

TIDE_TEST(cli_wasm_function_imports_of_a_library)
{
    bytes b = {0};
    put(&b, "!<arch>\n", 8);
    member(&b, "/", "\0\0\0\0", 4);
    put_object(&b, "rlgl.o/", "glClear");
    member(&b, "notes.txt/", "odd", 3); // Not an object, and padded
    put_object(&b, "rcore.o/", "glViewport");

    char names[256] = "";
    TIDE_REQUIRE(wasm_archive_imports(b.data, b.len, "env", found_import, names));
    TIDE_CHECK(strcmp(names, "glClear glViewport ") == 0);
    TIDE_CHECK(!wasm_archive_imports((const unsigned char *)"\0asm\x01\0\0\0", 8, "env", found_import, names));
    b.len -= 4; // Cut short: a member that says it's longer than the file
    TIDE_CHECK(!wasm_archive_imports(b.data, b.len, "env", found_import, names));
}
