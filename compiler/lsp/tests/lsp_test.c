// The language server, driven with JSON-RPC messages as an editor sends them.
// In test documents, `$` marks the cursor.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "json.h"
#include "purr_test.h"
#include "server.h"

static lsp_server server;
static char *sent[16];
static int sent_count;

static void clear_sent(void)
{
    for (int i = 0; i < sent_count; i++) free(sent[i]);
    sent_count = 0;
}

static void capture(void *user, const char *message, const size_t len)
{
    (void)user;
    if (sent_count == 16) return;
    char *copy = malloc(len + 1);
    memcpy(copy, message, len);
    copy[len] = '\0';
    sent[sent_count++] = copy;
}

static void handle(const char *message)
{
    lsp_handle(&server, message, strlen(message));
}

static const char *last_sent(void)
{
    return sent_count > 0 ? sent[sent_count - 1] : "";
}

static bool has(const char *haystack, const char *needle)
{
    return strstr(haystack, needle) != NULL;
}

static void start(void)
{
    clear_sent();
    lsp_free(&server);
    lsp_init(&server, capture, NULL);
    handle("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}");
}

// Opens `text` as the document, first removing the `$` that marks the cursor.
static int cursor_line;
static int cursor_character;

static void open_document(const char *marked)
{
    char text[8192];
    size_t n = 0;
    int line = 0;
    int character = 0;
    cursor_line = cursor_character = -1;
    for (const char *p = marked; *p && n + 1 < sizeof text; p++) {
        if (*p == '$') {
            cursor_line = line;
            cursor_character = character;
            continue;
        }
        text[n++] = *p;
        if (*p == '\n') {
            line++;
            character = 0;
        } else {
            character++;
        }
    }
    text[n] = '\0';

    jbuf b = {0};
    jb_put(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":"
               "{\"uri\":\"file:///test.purr\",\"languageId\":\"purrlang\",\"version\":1,\"text\":");
    jb_string(&b, text);
    jb_put(&b, "}}}");
    clear_sent();
    handle(b.data);
    jb_free(&b);
}

// Sends a request at the cursor, with `extra` params (JSON members, or ""), and
// returns the reply.
static const char *request_with(const char *method, const char *extra)
{
    char message[1024];
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"file:///test.purr\"},"
             "\"position\":{\"line\":%d,\"character\":%d}%s%s}}",
             method, cursor_line, cursor_character, extra[0] ? "," : "", extra);
    clear_sent();
    handle(message);
    return last_sent();
}

static const char *request(const char *method)
{
    return request_with(method, "");
}

// How many times `needle` appears.
static int count(const char *haystack, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(haystack, needle); p; p = strstr(p + 1, needle)) n++;
    return n;
}

static size_t offset_of(const char *text, const int line, const int character)
{
    size_t i = 0;
    for (int l = 0; l < line && text[i]; i++) {
        if (text[i] == '\n') l++;
    }
    return i + (size_t)character;
}

static int edit_order(const void *a, const void *b)
{
    const json *x = *(const json *const *)a;
    const json *y = *(const json *const *)b;
    const int lx = json_int(json_path(x, "range", "start", "line", NULL), 0);
    const int ly = json_int(json_path(y, "range", "start", "line", NULL), 0);
    if (lx != ly) return ly - lx;
    return json_int(json_path(y, "range", "start", "character", NULL), 0)
         - json_int(json_path(x, "range", "start", "character", NULL), 0);
}

// Applies the TextEdit[] in `edits` to `text` (ASCII), last edit first.
static void apply_edits(char *text, const size_t cap, const json *edits)
{
    json *sorted[256];
    int n = 0;
    for (int i = 0; i < edits->count && n < 256; i++) sorted[n++] = edits->items[i];
    qsort(sorted, (size_t)n, sizeof sorted[0], edit_order);
    for (int i = 0; i < n; i++) {
        const json *range = json_get(sorted[i], "range");
        const size_t start = offset_of(text, json_int(json_path(range, "start", "line", NULL), 0),
                                       json_int(json_path(range, "start", "character", NULL), 0));
        const size_t end = offset_of(text, json_int(json_path(range, "end", "line", NULL), 0),
                                     json_int(json_path(range, "end", "character", NULL), 0));
        const json *replacement = json_get(sorted[i], "newText");
        const size_t len = strlen(text);
        if (len - (end - start) + replacement->string_len >= cap) return;
        memmove(text + start + replacement->string_len, text + end, len - end + 1);
        memcpy(text + start, replacement->string, replacement->string_len);
    }
}

// The document after applying the edits in the last reply's result (or at `path` inside it).
static char applied[8192];

static const char *apply_reply(const char *original, const char *path)
{
    snprintf(applied, sizeof applied, "%s", original);
    const json *reply = json_parse(last_sent(), strlen(last_sent()));
    const json *edits = json_get(reply, "result");
    if (path) edits = json_path(edits, "changes", path, NULL);
    if (edits && edits->kind == JSON_ARRAY) apply_edits(applied, sizeof applied, edits);
    json_release();
    return applied;
}

static const char *complete(const char *marked)
{
    open_document(marked);
    return request("textDocument/completion");
}

// `"label":"name"` appears in a completion list.
static bool offers(const char *reply, const char *label)
{
    char needle[128];
    snprintf(needle, sizeof needle, "\"label\":\"%s\"", label);
    return has(reply, needle);
}

#define GAME_TYPES                                                             \
    "component Body\n"                                                         \
    "{\n"                                                                      \
    "    float2 position;\n"                                                   \
    "    float radius = 10;\n"                                                 \
    "}\n"                                                                      \
    "\n"                                                                       \
    "singleton Arena\n"                                                        \
    "{\n"                                                                      \
    "    float2 halfSize = float2(400, 225);\n"                                \
    "}\n"                                                                      \
    "\n"                                                                       \
    "input PlayerInput\n"                                                      \
    "{\n"                                                                      \
    "    float2 move;\n"                                                       \
    "    bool fire;\n"                                                         \
    "}\n"                                                                      \
    "\n"                                                                       \
    "system Main()\n"                                                          \
    "{\n"                                                                      \
    "    Spawn(Body, Owner);\n"                                                \
    "}\n"

PURR_TEST(lsp_initialize_advertises_features)
{
    start();
    const char *reply = last_sent();
    PURR_CHECK(has(reply, "\"id\":1"));
    PURR_CHECK(has(reply, "\"completionProvider\""));
    PURR_CHECK(has(reply, "\"hoverProvider\":true"));
    PURR_CHECK(has(reply, "\"tokenTypes\":[\"namespace\""));
}

PURR_TEST(lsp_diagnostics)
{
    start();
    open_document(GAME_TYPES);
    PURR_CHECK(has(last_sent(), "\"method\":\"textDocument/publishDiagnostics\""));
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.pos = 1;\n}\n");
    PURR_CHECK(has(last_sent(), "Body has no field 'pos'"));
    PURR_CHECK(has(last_sent(), "\"range\":{\"start\":{\"line\":23,\"character\":9},\"end\":{\"line\":23,\"character\":12}}"));
    PURR_CHECK(has(last_sent(), "\"severity\":1"));
}

PURR_TEST(lsp_diagnostics_keep_going_after_syntax_errors)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.position = ;\n    var x = 1 +;\n    body.radius = true;\n}\n");
    const char *reply = last_sent();
    PURR_CHECK(has(reply, "expected an expression, found ';'"));
    PURR_CHECK(has(reply, "\"line\":24")); // The second syntax error
    PURR_CHECK(has(reply, "can't assign bool to float")); // And the checker still ran
}

PURR_TEST(lsp_complete_fields_after_dot)
{
    start();
    const char *reply = complete(GAME_TYPES "system Move(mut Body body)\n{\n    body.$\n}\n");
    PURR_CHECK(offers(reply, "position"));
    PURR_CHECK(offers(reply, "radius"));
    PURR_CHECK(!offers(reply, "halfSize"));
}

PURR_TEST(lsp_complete_chains_and_swizzles)
{
    start();
    const char *reply = complete(GAME_TYPES "system Move(mut Body body)\n{\n    var p = body.position.$\n}\n");
    PURR_CHECK(offers(reply, "x"));
    PURR_CHECK(offers(reply, "y"));
    PURR_CHECK(!offers(reply, "z"));
    PURR_CHECK(offers(reply, "xy") == false); // A float2's only swizzle of all its components is itself
}

PURR_TEST(lsp_complete_input_edges)
{
    start();
    const char *reply = complete(GAME_TYPES "system Fire(PlayerInput input)\n{\n    if (input.fire.$) return;\n}\n");
    PURR_CHECK(offers(reply, "down"));
    PURR_CHECK(offers(reply, "up"));
}

// Inside Sanitize, the input's fields are in scope; Sample's devices aren't.
PURR_TEST(lsp_sanitize)
{
    start();
    static const char program[] = "input PlayerInput\n{\n    float move;\n\n    Sample(Devices devices) { }\n\n"
                                  "    Sanitize()\n    {\n        move = Math.Clamp($, -1, 1);\n    }\n}\n"
                                  "system Main() { }\n";
    const char *reply = complete(program);
    PURR_CHECK(offers(reply, "move"));
    PURR_CHECK(!offers(reply, "devices"));

    open_document("input PlayerInput\n{\n    float move;\n    Sani$tize() { move = Math.Clamp(move, -1, 1); }\n}\n"
         "system Main() { }\n");
    PURR_CHECK(has(sent[0], "\"diagnostics\":[]"));
    PURR_CHECK(has(request("textDocument/hover"), "Runs on every input before the simulation reads it"));

    open_document("input PlayerInput\n{\n    bool jump;\n    Sam$ple(Devices devices) { jump = devices.keyboard.space.pressed; }\n}\n"
         "system Main() { }\n");
    PURR_CHECK(has(sent[0], "\"diagnostics\":[]"));
    PURR_CHECK(has(request("textDocument/hover"), "Builds the player's input from the devices"));
}

static const char *format_reply(const char *text);

// [Clamp], [Min] and [Max] on input fields: completion, and formatting.
PURR_TEST(lsp_field_attributes)
{
    start();
    const char *names = complete("input PlayerInput\n{\n    [$] float move;\n}\nsystem Main() { }\n");
    PURR_CHECK(offers(names, "Clamp") && offers(names, "Min") && offers(names, "Max"));
    PURR_CHECK(!offers(names, "Before"));
    const char *bounds = complete("input PlayerInput\n{\n    [Clamp(float2(0, 0), $)] float2 aim;\n}\nsystem Main() { }\n");
    PURR_CHECK(offers(bounds, "Math") && offers(bounds, "float2"));

    static const char messy[] = "input PlayerInput\n{\n[Clamp(-1,1)]float move;\n    [Min(0),Max(3)]   int gear;\n}\n"
                                "system Main() { }\n";
    static const char expected[] = "input PlayerInput\n{\n    [Clamp(-1, 1)] float move;\n    [Min(0), Max(3)] int gear;\n}\n"
                                   "system Main() { }\n";
    open_document(messy);
    PURR_CHECK(has(sent[0], "\"diagnostics\":[]"));
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}

PURR_TEST(lsp_complete_builtin_owners)
{
    start();
    const char *math = complete(GAME_TYPES "system S(mut Body body)\n{\n    body.radius = Math.$\n}\n");
    PURR_CHECK(offers(math, "Sin"));
    PURR_CHECK(offers(math, "PI"));
    PURR_CHECK(offers(math, "Normalize"));

    const char *draw = complete(GAME_TYPES "view V(Body body)\n{\n    Draw.$\n}\n");
    PURR_CHECK(offers(draw, "Circle"));
    PURR_CHECK(has(draw, "Circle(${1:center}, ${2:radius}, ${3:color})"));

    const char *color = complete(GAME_TYPES "view V(Body body)\n{\n    Draw.Circle(body.position, 1, Color.$);\n}\n");
    PURR_CHECK(offers(color, "red"));
}

PURR_TEST(lsp_complete_draw_only_in_views)
{
    start();
    const char *system = complete(GAME_TYPES "system S(mut Body body)\n{\n    $\n}\n");
    PURR_CHECK(!offers(system, "Draw"));
    PURR_CHECK(offers(system, "Spawn"));
    const char *view = complete(GAME_TYPES "view V(Body body)\n{\n    $\n}\n");
    PURR_CHECK(offers(view, "Draw"));
    PURR_CHECK(!offers(view, "Spawn"));
}

PURR_TEST(lsp_complete_names_in_scope)
{
    start();
    const char *reply = complete(GAME_TYPES "system S(Time time, mut Body body)\n{\n    var speed = 2.0;\n"
                                            "    if (true)\n    {\n        var inner = 1;\n    }\n    body.radius = $\n}\n");
    PURR_CHECK(offers(reply, "speed"));
    PURR_CHECK(offers(reply, "time"));
    PURR_CHECK(offers(reply, "body"));
    PURR_CHECK(!offers(reply, "inner")); // Out of scope
    PURR_CHECK(offers(reply, "float3"));
    PURR_CHECK(offers(reply, "Math"));
}

PURR_TEST(lsp_complete_in_constructor)
{
    start();
    const char *devices = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample(Devices devices)\n    {\n"
                                   "        var keys = devices.$\n    }\n}\nsystem Main() { }\n");
    PURR_CHECK(offers(devices, "keyboard"));
    PURR_CHECK(offers(devices, "gamepad"));

    const char *keys = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample(Devices devices)\n    {\n"
                                "        var keys = devices.keyboard;\n        if (keys.$\n    }\n}\nsystem Main() { }\n");
    PURR_CHECK(offers(keys, "space"));
    PURR_CHECK(offers(keys, "leftShift"));
}

PURR_TEST(lsp_complete_declarations_and_headers)
{
    start();
    const char *top = complete(GAME_TYPES "\n$");
    PURR_CHECK(offers(top, "component"));
    PURR_CHECK(offers(top, "view"));

    const char *header = complete(GAME_TYPES "system S($)\n{\n}\n");
    PURR_CHECK(offers(header, "mut"));
    PURR_CHECK(offers(header, "without"));
    PURR_CHECK(offers(header, "Body"));
    PURR_CHECK(offers(header, "Arena"));
    PURR_CHECK(offers(header, "PlayerInput"));

    const char *filter = complete(GAME_TYPES "system S(with $)\n{\n}\n");
    PURR_CHECK(offers(filter, "Body"));
    PURR_CHECK(!offers(filter, "Arena")); // Singletons aren't filters

    const char *name = complete(GAME_TYPES "system S(PlayerInput $)\n{\n}\n");
    PURR_CHECK(offers(name, "playerInput"));
}

PURR_TEST(lsp_complete_literal_fields)
{
    start();
    const char *reply = complete(GAME_TYPES "system S()\n{\n    Spawn(Body { $ });\n}\n");
    PURR_CHECK(offers(reply, "position"));
    PURR_CHECK(offers(reply, "radius"));
}

PURR_TEST(lsp_hover)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body$.radius = 1;\n}\n");
    const char *param = request("textDocument/hover");
    PURR_CHECK(has(param, "mut Body body"));

    open_document(GAME_TYPES "system Move(mut B$ody body)\n{\n}\n");
    const char *type = request("textDocument/hover");
    PURR_CHECK(has(type, "component Body"));
    PURR_CHECK(has(type, "float radius = 10;"));

    open_document(GAME_TYPES "view V(Body body)\n{\n    Draw.Cir$cle(body.position, 1, Color.red);\n}\n");
    const char *function = request("textDocument/hover");
    PURR_CHECK(has(function, "Draw.Circle(float2 center, float radius, Color color)"));
    PURR_CHECK(has(function, "A filled circle."));
}

PURR_TEST(lsp_definition)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    bo$dy.radius = 1;\n}\n");
    const char *param = request("textDocument/definition");
    PURR_CHECK(has(param, "\"range\":{\"start\":{\"line\":21,\"character\":21}"));

    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.rad$ius = 1;\n}\n");
    const char *field = request("textDocument/definition");
    PURR_CHECK(has(field, "\"range\":{\"start\":{\"line\":3,\"character\":10}"));
}

// The line a definition reply points at, read from the file it names ("" if none).
static const char *definition_line(void)
{
    static char text[512];
    text[0] = '\0';
    const json *reply = json_parse(last_sent(), strlen(last_sent()));
    const char *uri = json_str(json_path(reply, "result", "uri", NULL));
    const int line = json_int(json_path(reply, "result", "range", "start", "line", NULL), -1);
    if (uri && strncmp(uri, "file://", 7) == 0 && line >= 0) {
        const char *path = uri + 7;
        if (path[0] == '/' && path[2] == ':') path++; // file:///D:/... on Windows
        FILE *f = fopen(path, "rb");
        for (int l = 0; f && l <= line && fgets(text, sizeof text, f); l++) {
        }
        if (f) fclose(f);
    }
    json_release();
    return text;
}

static const char *c_definition(const char *marked)
{
    open_document(marked);
    request("textDocument/definition");
    return definition_line();
}

// Built-ins lead to their C definitions in the engine headers.
PURR_TEST(lsp_definition_in_c)
{
    start();
#define IN_SYSTEM(code) GAME_TYPES "system S(mut Body body, Time time)\n{\n    " code "\n}\n"
#define IN_VIEW(code) GAME_TYPES "view V(Body body)\n{\n    " code "\n}\n"
    PURR_CHECK(has(c_definition(IN_VIEW("Draw.Cir$cle(body.position, 1, Color.red);")), "void purr_draw_circle("));
    PURR_CHECK(has(c_definition(IN_VIEW("Draw.Circle(body.position, 1, Color.r$ed);")), "#define PURR_COLOR_RED"));
    PURR_CHECK(has(c_definition(IN_VIEW("Dr$aw.Clear(Color.red);")), "#pragma once"));
    PURR_CHECK(has(c_definition(IN_SYSTEM("body.radius = Math.S$in(body.radius);")), "purr_sin_f("));
    PURR_CHECK(has(c_definition(IN_SYSTEM("body.position = Math.S$in(body.position);")), "PURR_MAP1(float, f, sin, f)"));
    PURR_CHECK(has(c_definition(IN_SYSTEM("body.position = Math.Nor$malize(body.position);")), "purr_normalize_f##N("));
    PURR_CHECK(has(c_definition(IN_SYSTEM("body.radius = Math.D$ot(float3(1), float3(2));")), "purr_dot_f3("));
    PURR_CHECK(has(c_definition(IN_SYSTEM("body.radius = Math.P$I;")), "#define PURR_PI_F"));
    PURR_CHECK(has(c_definition(IN_SYSTEM("var q = quaternion.ident$ity;")), "purr_identity_q(void)"));
    PURR_CHECK(has(c_definition(IN_SYSTEM("flo$at3 v = float3(1);")), "typedef struct purr_float3"));
    PURR_CHECK(has(c_definition(IN_SYSTEM("body.radius = body.position.x$;")), "typedef struct purr_float2 { float x"));
    PURR_CHECK(has(c_definition(IN_SYSTEM("var c = Color.red.g$;")), "float r, g, b, a;"));
    PURR_CHECK(strcmp(c_definition(GAME_TYPES "system S(Ti$me time) { }\n"), "") == 0); // Generated: no C definition

    static const char constructor[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample(Devices devices)\n    {\n"
                                      "        var keys = devices.key$board;\n        fire = keys.space.pressed;\n    }\n}\n"
                                      "system Main() { }\n";
    PURR_CHECK(has(c_definition(constructor), "purr_keyboard keyboard;"));
    static const char key[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample(Devices devices)\n    {\n"
                              "        var keys = devices.keyboard;\n        fire = keys.spa$ce.pressed;\n    }\n}\n"
                              "system Main() { }\n";
    PURR_CHECK(has(c_definition(key), "X(space)"));
    static const char button[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample(Devices devices)\n    {\n"
                                 "        fire = devices.mouse.left.pre$ssed;\n    }\n}\nsystem Main() { }\n";
    PURR_CHECK(has(c_definition(button), "bool pressed;"));
#undef IN_SYSTEM
#undef IN_VIEW
}

PURR_TEST(lsp_symbols)
{
    start();
    open_document(GAME_TYPES "view DrawBodies(Body body) { }\n");
    const char *reply = request("textDocument/documentSymbol");
    PURR_CHECK(has(reply, "\"name\":\"Body\""));
    PURR_CHECK(has(reply, "\"name\":\"radius\""));
    PURR_CHECK(has(reply, "\"name\":\"DrawBodies\",\"detail\":\"view\""));
}

PURR_TEST(lsp_semantic_tokens)
{
    start();
    open_document(GAME_TYPES);
    const char *reply = request("textDocument/semanticTokens/full");
    // `Body` on line 0, column 10: a struct (2), declared (1).
    PURR_CHECK(has(reply, "\"data\":[0,10,4,2,1,"));

    // Columns count UTF-16 units: `é` is two bytes but one unit.
    open_document("/* \xC3\xA9 */ component Body { float x; }\nsystem Main() { }\n");
    PURR_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,18,4,2,1,"));

    // Built-in value types are keywords (11), like C#'s float: `float3` 7 columns after `Body`.
    open_document("component Body { float3 p; }\nsystem Main() { }\n");
    PURR_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,10,4,2,1,0,7,6,11,0,"));
    // Sample and Sanitize are keywords too, and attributes decorators (12).
    open_document("input PlayerInput\n{\n    [Clamp(-1, 1)] float move;\n    Sample(Devices devices) { }\n}\nsystem Main() { }\n");
    const char *input = request("textDocument/semanticTokens/full");
    PURR_CHECK(has(input, "2,5,5,12,0,")); // Clamp: line +2, column 5
    PURR_CHECK(has(input, "1,4,6,11,0,")); // Sample: line +1, column 4
}

#define STRUCTS                                                                \
    "struct Stats\n"                                                           \
    "{\n"                                                                      \
    "    float health = 100;\n"                                                \
    "}\n"                                                                      \
    "\n"                                                                       \
    "component Unit\n"                                                         \
    "{\n"                                                                      \
    "    Stats stats;\n"                                                       \
    "}\n"                                                                      \
    "\n"                                                                       \
    "system Main()\n"                                                          \
    "{\n"                                                                      \
    "    Spawn(Unit);\n"                                                       \
    "}\n"

PURR_TEST(lsp_structs)
{
    start();
    open_document(STRUCTS);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    // `Stats` on line 0, column 7: a struct (2), declared (1).
    PURR_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,7,5,2,1,"));
    PURR_CHECK(has(request("textDocument/documentSymbol"), "\"name\":\"Stats\""));

    // As a field type, through members, and in values.
    PURR_CHECK(offers(complete("struct Stats { float health; }\ncomponent Unit\n{\n    $\n}\n"), "Stats"));
    PURR_CHECK(offers(complete(STRUCTS "system Hurt(mut Unit unit)\n{\n    unit.stats.$\n}\n"), "health"));
    PURR_CHECK(offers(complete(STRUCTS "system Hurt(mut Unit unit)\n{\n    unit.stats = Stats { $ };\n}\n"), "health"));

    open_document("struct St$ats\n{\n    float health = 100;\n}\nsystem Main() { }\n");
    const char *hover = request("textDocument/hover");
    PURR_CHECK(has(hover, "struct Stats"));
    PURR_CHECK(has(hover, "float health = 100;"));

    // Laid out like the other declarations.
    static const char messy[] = "struct Stats {\nfloat health = 100;\n}\nsystem Main() { }\n";
    format_reply(messy);
    PURR_CHECK(strcmp(apply_reply(messy, NULL), "struct Stats\n{\n    float health = 100;\n}\nsystem Main() { }\n") == 0);
}

#define USES_RADIUS                                                            \
    GAME_TYPES                                                                 \
    "system Grow(mut Body body)\n{\n    body.radius += 1;\n}\n"                  \
    "system Make()\n{\n    Spawn(Body { radius = 2 });\n}\n"

PURR_TEST(lsp_references)
{
    start();
    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.rad$ius, Color.red);\n}\n");
    // The declaration, `body.radius += 1`, the literal and the view.
    PURR_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":true}"), "\"uri\"") == 4);
    PURR_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":false}"), "\"uri\"") == 3);

    open_document(GAME_TYPES "system Move(mut Body bo$dy)\n{\n    body.radius = body.radius * 2;\n}\n");
    const char *highlights = request("textDocument/documentHighlight");
    PURR_CHECK(count(highlights, "\"range\"") == 3);
    PURR_CHECK(count(highlights, "\"kind\":3") == 1); // The declaration
}

PURR_TEST(lsp_rename)
{
    start();
    static const char program[] = USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.radius, Color.red);\n}\n";
    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.rad$ius, Color.red);\n}\n");
    PURR_CHECK(has(request("textDocument/prepareRename"), "\"result\":{\"start\""));
    request_with("textDocument/rename", "\"newName\":\"size\"");
    const char *renamed = apply_reply(program, "file:///test.purr");
    PURR_CHECK(has(renamed, "float size = 10;"));
    PURR_CHECK(has(renamed, "body.size += 1;"));
    PURR_CHECK(has(renamed, "Body { size = 2 }"));
    PURR_CHECK(has(renamed, "body.position, body.size,"));
    PURR_CHECK(!has(renamed, "radius"));
    // The result still compiles cleanly.
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", renamed);
    open_document(copy);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
}

PURR_TEST(lsp_rename_refusals)
{
    start();
    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.rad$ius, Color.red);\n}\n");
    PURR_CHECK(has(request_with("textDocument/rename", "\"newName\":\"position\""), "'position' is already declared"));
    PURR_CHECK(has(request_with("textDocument/rename", "\"newName\":\"return\""), "keyword"));
    PURR_CHECK(has(request_with("textDocument/rename", "\"newName\":\"float3\""), "built into the language"));
    PURR_CHECK(has(request_with("textDocument/rename", "\"newName\":\"2fast\""), "start with a letter"));

    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Cir$cle(body.position, body.radius, Color.red);\n}\n");
    PURR_CHECK(has(request("textDocument/prepareRename"), "Built-in names can't be renamed"));

    open_document(GAME_TYPES "system Mo$ve(mut Body body)\n{\n    body.radius = ;\n}\n");
    PURR_CHECK(has(request("textDocument/prepareRename"), "Fix the syntax errors first"));
}

PURR_TEST(lsp_signature_help)
{
    start();
    open_document(GAME_TYPES "view V(Body body)\n{\n    Draw.Circle(body.position, $\n}\n");
    const char *circle = request("textDocument/signatureHelp");
    PURR_CHECK(has(circle, "\"label\":\"Draw.Circle(float2 center, float radius, Color color)\""));
    PURR_CHECK(has(circle, "\"activeParameter\":1"));
    // "Draw.Circle(" is 12 characters: `float2 center` is 12 to 25.
    PURR_CHECK(has(circle, "\"parameters\":[{\"label\":[12,25]}"));

    open_document(GAME_TYPES "system S()\n{\n    var v = float3(float2(1, 2), $\n}\n");
    const char *vector = request("textDocument/signatureHelp");
    PURR_CHECK(has(vector, "float3(float2 xy, float z)"));
    PURR_CHECK(has(vector, "\"activeSignature\":0,\"activeParameter\":1"));

    // Commas inside a component literal don't count.
    open_document(GAME_TYPES "system S()\n{\n    Spawn(Body { position = float2(1, 2), radius = 3 }, $\n}\n");
    const char *spawn = request("textDocument/signatureHelp");
    PURR_CHECK(has(spawn, "Spawn(components...)"));
    PURR_CHECK(has(spawn, "\"activeParameter\":0"));

    open_document(GAME_TYPES "system S()\n{\n    $\n}\n");
    PURR_CHECK(has(request("textDocument/signatureHelp"), "\"result\":null"));
}

static const char *format_reply(const char *text)
{
    open_document(text);
    return request_with("textDocument/formatting", "\"options\":{\"tabSize\":4,\"insertSpaces\":true}");
}

PURR_TEST(lsp_format)
{
    start();
    static const char messy[] =
        "\n\ncomponent Body\n{\nfloat2 position;   // Where it is\n  float radius=10;\n}\n\n\n\n"
        "system Move( mut Body body,Time time )\n{\nif(body.radius>1)\nbody.radius-=1;\n"
        "  else if (body.radius <-5) { body.radius = -body.radius *2; }\n"
        "    var p = float2(1,-2)+body.position;\n"
        "var s=body.radius>1?-1:0;\n"
        "Spawn(Body{position=p},\nOwner);\n\n}\n"
        "/* a\n   block */\n"
        "system Main( ) { }";
    static const char expected[] =
        "component Body\n{\n    float2 position;   // Where it is\n    float radius = 10;\n}\n\n"
        "system Move(mut Body body, Time time)\n{\n    if (body.radius > 1)\n        body.radius -= 1;\n"
        "    else if (body.radius < -5) { body.radius = -body.radius * 2; }\n"
        "    var p = float2(1, -2) + body.position;\n"
        "    var s = body.radius > 1 ? -1 : 0;\n"
        "    Spawn(Body { position = p },\n        Owner);\n\n}\n"
        "/* a\n   block */\n"
        "system Main() { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);

    // Formatting what's formatted changes nothing.
    PURR_CHECK(has(format_reply(expected), "\"result\":[]"));

    // Continuation lines go one level deeper, or keep deeper alignment.
    static const char continued[] =
        "system Main()\n{\n    var x = 1\n    + 2;\n    Spawn(Owner,\n          Owner);\n}\n";
    format_reply(continued);
    PURR_CHECK(strcmp(apply_reply(continued, NULL),
                      "system Main()\n{\n    var x = 1\n        + 2;\n    Spawn(Owner,\n          Owner);\n}\n") == 0);
}

// C#-style braces: a block that spans lines has its braces on lines of their
// own. Blocks on one line and literals stay as they are.
PURR_TEST(lsp_format_braces)
{
    start();
    static const char js[] =
        "component Body {\n    float2 position;\n}\n"
        "input Keys {\n    bool fire;\n\n    Sample(Devices devices) {\n        fire = devices.keyboard.space.pressed; }\n}\n"
        "system Move(mut Body body) { // Every body\n"
        "    if (body.position.x > 1) {\n        body.position.x = 0;\n    } else {\n        body.position.x += 1;\n    }\n"
        "    if (body.position.y > 1) { return; } else {\n        body.position.y = 0;\n    }\n"
        "    if (body.position.y < 0) {return;}\n"
        "    var b = Spawn(Body { position = float2(1) });\n"
        "    Spawn(Body {\n        position = float2(2)\n    });\n"
        "}\n"
        "system Main() { }\n";
    static const char expected[] =
        "component Body\n{\n    float2 position;\n}\n"
        "input Keys\n{\n    bool fire;\n\n    Sample(Devices devices)\n    {\n        fire = devices.keyboard.space.pressed;\n    }\n}\n"
        "system Move(mut Body body)\n{ // Every body\n"
        "    if (body.position.x > 1)\n    {\n        body.position.x = 0;\n    }\n    else\n    {\n        body.position.x += 1;\n    }\n"
        "    if (body.position.y > 1) { return; }\n    else\n    {\n        body.position.y = 0;\n    }\n"
        "    if (body.position.y < 0) { return; }\n"
        "    var b = Spawn(Body { position = float2(1) });\n"
        "    Spawn(Body {\n        position = float2(2)\n    });\n" // A literal's fields: one level in, even in a call
        "}\n"
        "system Main() { }\n";
    format_reply(js);
    const char *formatted = apply_reply(js, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);

    // Formatting what's formatted changes nothing.
    char again[8192];
    snprintf(again, sizeof again, "%s", formatted);
    PURR_CHECK(has(format_reply(again), "\"result\":[]"));

    // New lines match the file's.
    static const char crlf[] = "system Main() {\r\n    return;\r\n}\r\n";
    format_reply(crlf);
    PURR_CHECK(strcmp(apply_reply(crlf, NULL), "system Main()\r\n{\r\n    return;\r\n}\r\n") == 0);
}

// Formatting keeps every token, in order: only whitespace changes.
PURR_TEST(lsp_format_keeps_tokens)
{
    start();
    static const char program[] =
        GAME_TYPES
        "system Move(PlayerInput input, Time time, mut Body body)\n{\n"
        "      mut var speed=Math.Length(body.position.xy)*2;\n"
        "  if (input.fire.down&&speed<10) { body.position+=input.move*time.dt; }\n"
        "else body.radius=Math.Clamp(body.radius,1,-2) ;\n"
        "    var e=Spawn(Body{position=float2(1,2)});e.Destroy();\n}\n"
        "view DrawBody(Body body, Arena arena)\n{\n"
        "Draw.Text(\"a  b   \\\"c\\\"\", body.position,12,Color(1,0.5,0));\n}\n";
    format_reply(program);
    char formatted[8192];
    snprintf(formatted, sizeof formatted, "%s", apply_reply(program, NULL));
    PURR_CHECK(strcmp(formatted, program) != 0);

    // Same text with all whitespace removed.
    char a[8192];
    char b[8192];
    size_t na = 0;
    size_t nb = 0;
    for (const char *p = program; *p; p++) if (*p != ' ' && *p != '\n') a[na++] = *p;
    for (const char *p = formatted; *p; p++) if (*p != ' ' && *p != '\n') b[nb++] = *p;
    a[na] = b[nb] = '\0';
    PURR_CHECK(strcmp(a, b) == 0);
    PURR_CHECK(has(formatted, "\"a  b   \\\"c\\\"\"")); // Text keeps its spaces

    // It still compiles as before, and formatting again changes nothing.
    open_document(formatted);
    PURR_CHECK(!has(last_sent(), "\"severity\":1"));
    PURR_CHECK(has(format_reply(formatted), "\"result\":[]"));
}

// Typing a program from scratch: every prefix is analysed and queried, as an
// editor does on each keystroke. Nothing may crash, whatever state it's in.
PURR_TEST(lsp_every_prefix_is_safe)
{
    static const char program[] =
        GAME_TYPES
        "input Controls\n{\n    float2 aim;\n\n    Controls(Devices devices)\n    {\n"
        "        var keys = devices.keyboard;\n        if (keys.w.pressed) aim.y += 1;\n    }\n}\n"
        "system Move(Controls controls, Time time, mut Body body, without Arena)\n{\n"
        "    mut var speed = Math.Length(body.position.xy) * 2;\n"
        "    if (controls.aim.x > 0 && speed < 10) { body.position += controls.aim * time.dt; }\n"
        "    else body.radius = Math.Clamp(body.radius, 1, 2);\n"
        "    var e = Spawn(Body { position = float2(1, 2) });\n    e.Destroy();\n}\n"
        "view DrawBody(Body body, Arena arena)\n{\n"
        "    Draw.Text(\"hi \\\"there\\\"\", body.position, 12, Color(1, 0.5, 0));\n"
        "    Draw.Circle(body.position, body.radius, Color.red);\n}\n";
    start();
    char text[sizeof program];
    for (size_t n = 0; n < sizeof program; n++) {
        memcpy(text, program, n);
        text[n] = '\0';
        open_document(text);
        int line = 0;
        int character = 0;
        for (size_t i = 0; i < n; i++) {
            if (text[i] == '\n') {
                line++;
                character = 0;
            } else {
                character++;
            }
        }
        cursor_line = line;
        cursor_character = character;
        request("textDocument/completion");
        request("textDocument/hover");
        request("textDocument/semanticTokens/full");
        request("textDocument/signatureHelp");
        request("textDocument/documentHighlight");
        if (n % 16 == 0) {
            request("textDocument/documentSymbol");
            request_with("textDocument/references", "\"context\":{\"includeDeclaration\":true}");
            request_with("textDocument/rename", "\"newName\":\"renamed\"");
            request_with("textDocument/formatting", "\"options\":{\"tabSize\":4,\"insertSpaces\":true}");
        }
    }
    PURR_CHECK(has(last_sent(), "\"result\""));
}

// ---------------------------------------------------------------------------
// A game of several files, found through the manifest purr_add_game writes

static char game_dir[512];

static void game_path(char *out, const size_t size, const char *name)
{
    snprintf(out, size, "%s/%s", game_dir, name);
}

static void write_file(const char *name, const char *text)
{
    char path[640];
    game_path(path, sizeof path, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void open_uri(const char *uri, const char *text)
{
    jbuf b = {0};
    jb_put(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":");
    jb_string(&b, uri);
    jb_put(&b, ",\"languageId\":\"purrlang\",\"version\":1,\"text\":");
    jb_string(&b, text);
    jb_put(&b, "}}}");
    clear_sent();
    handle(b.data);
    jb_free(&b);
}

static const char *request_at(const char *uri, const char *method, const int line, const int character,
                              const char *extra)
{
    char message[2048];
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"%s\"},"
             "\"position\":{\"line\":%d,\"character\":%d}%s%s}}",
             method, uri, line, character, extra[0] ? "," : "", extra);
    clear_sent();
    handle(message);
    return last_sent();
}

// Sets game_dir to the working directory, with forward slashes.
static bool find_game_dir(void)
{
    char buffer[512] = "";
#ifdef _WIN32
    if (!_getcwd(buffer, sizeof buffer)) return false;
#else
    if (!getcwd(buffer, sizeof buffer)) return false;
#endif
    for (char *p = buffer; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    snprintf(game_dir, sizeof game_dir, "%s", buffer);
    return true;
}

PURR_TEST(lsp_game_of_several_files)
{
    if (!find_game_dir()) return;

    static const char physics[] = "namespace Physics;\n"
                                  "component Body { float2 position; }\n"
                                  "system Gravity(mut Body body) { body.position.y -= 1; }\n";
    static const char main_file[] = "using Physics;\n"
                                    "system Main() { Spawn(Body); }\n"
                                    "[After(Physics.Gravity)]\n"
                                    "system Move(mut Body body) { body.position.x += 1; }\n";
    write_file("lsp_game_physics.purr", physics);
    write_file("lsp_game_main.purr", main_file);
    char a[640], b[640], manifest[640], manifest_text[1400], uri_a[700], uri_b[700];
    game_path(a, sizeof a, "lsp_game_physics.purr");
    game_path(b, sizeof b, "lsp_game_main.purr");
    game_path(manifest, sizeof manifest, "lsp_game_manifest.txt");
    snprintf(manifest_text, sizeof manifest_text, "other\t%s/elsewhere.purr\ngame\t%s\ngame\t%s\n", game_dir, a, b);
    write_file("lsp_game_manifest.txt", manifest_text);
    snprintf(uri_a, sizeof uri_a, "file:///%s", a[0] == '/' ? a + 1 : a);
    snprintf(uri_b, sizeof uri_b, "file:///%s", b[0] == '/' ? b + 1 : b);

    start();
    server.manifest = manifest;
    open_uri(uri_b, main_file);
    // Diagnostics for both files, in path order, none of them errors: Body and
    // Gravity come from the other file.
    PURR_CHECK(sent_count == 2);
    PURR_CHECK(count(sent[0], uri_b) == 1 && count(sent[1], uri_a) == 1);
    PURR_CHECK(has(sent[0], "\"diagnostics\":[]") && has(sent[1], "\"diagnostics\":[]"));

    // `Body` leads to the other file.
    PURR_CHECK(has(request_at(uri_b, "textDocument/definition", 1, 22, ""), uri_a));
    PURR_CHECK(count(request_at(uri_b, "textDocument/references", 1, 22, "\"context\":{\"includeDeclaration\":true}"),
                     uri_a) >= 2); // The declaration and Gravity's parameter
    PURR_CHECK(has(last_sent(), uri_b));

    // The hover shows where Move runs.
    PURR_CHECK(has(request_at(uri_b, "textDocument/hover", 3, 8, ""), "Runs 2nd of 2 systems each tick, after `Physics.Gravity`"));

    // Renaming Gravity from the attribute edits both files.
    const char *rename = request_at(uri_b, "textDocument/rename", 2, 17, "\"newName\":\"Fall\"");
    PURR_CHECK(has(rename, uri_a) && has(rename, uri_b));
    PURR_CHECK(count(rename, "\"newText\":\"Fall\"") == 2);

    // Completion after `Physics.` lists what's inside it.
    open_uri(uri_b, "using Physics;\nsystem Main() { Spawn(Physics.); }\n");
    const char *completion = request_at(uri_b, "textDocument/completion", 1, 30, "");
    PURR_CHECK(offers(completion, "Body"));
    PURR_CHECK(!offers(completion, "Gravity")); // Systems only in Before and After

    // An error in one file shows in the other: Main now spawns a Body nobody declares.
    open_uri(uri_b, main_file);
    open_uri(uri_a, "namespace Physics;\ncomponent Shape { float2 position; }\n");
    PURR_CHECK(sent_count == 2);
    PURR_CHECK(has(sent[1], uri_a) && has(sent[1], "\"diagnostics\":[]")); // The file that changed is fine...
    PURR_CHECK(has(sent[0], uri_b) && has(sent[0], "unknown component or singleton 'Body'")); // ...its user isn't

    remove(a);
    remove(b);
    remove(manifest);
    clear_sent();
    lsp_free(&server);
}

static void make_folder(const char *name)
{
    char path[640];
    game_path(path, sizeof path, name);
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

static void remove_folder(const char *name)
{
    char path[640];
    game_path(path, sizeof path, name);
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}

static void remove_game_file(const char *name)
{
    char path[640];
    game_path(path, sizeof path, name);
    remove(path);
}

// A game listed as a folder, as purr_add_game lists one without SOURCES: every
// .purr file in it and its subfolders, even one the editor hasn't saved yet.
PURR_TEST(lsp_game_folder)
{
    if (!find_game_dir()) return;
    make_folder("lsp_folder");
    make_folder("lsp_folder/sub");
    static const char physics[] = "namespace Physics;\n"
                                  "component Body { float2 position; }\n"
                                  "system Gravity(mut Body body) { body.position.y -= 1; }\n";
    static const char main_file[] = "using Physics;\n"
                                    "system Main() { Spawn(Body); }\n"
                                    "system Move(mut Body body) { body.position.x += 1; }\n";
    write_file("lsp_folder/sub/physics.purr", physics);
    write_file("lsp_folder/main.purr", main_file);
    write_file("lsp_folder/notes.txt", "not PurrLang");
    char manifest[640], manifest_text[1400], uri_main[700], uri_new[700], uri_physics[700];
    game_path(manifest, sizeof manifest, "lsp_folder_manifest.txt");
    snprintf(manifest_text, sizeof manifest_text, "other\t%s/elsewhere.purr\ngame\t%s/lsp_folder/\n", game_dir,
             game_dir);
    write_file("lsp_folder_manifest.txt", manifest_text);
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_folder/main.purr", dir);
    snprintf(uri_new, sizeof uri_new, "file:///%s/lsp_folder/new.purr", dir);
    snprintf(uri_physics, sizeof uri_physics, "file:///%s/lsp_folder/sub/physics.purr", dir);

    start();
    server.manifest = manifest;
    open_uri(uri_main, main_file);
    // Body comes from the file in the subfolder.
    PURR_CHECK(sent_count == 2);
    PURR_CHECK(has(sent[0], uri_main) && has(sent[0], "\"diagnostics\":[]"));
    PURR_CHECK(has(sent[1], uri_physics) && has(sent[1], "\"diagnostics\":[]"));
    // In path order, main.purr comes before sub/physics.purr.
    PURR_CHECK(has(request_at(uri_main, "textDocument/hover", 2, 8, ""), "Runs 1st of 2 systems each tick."));

    // A new file joins the game before it's saved.
    open_uri(uri_new, "using Physics;\nsystem Fall(mut Body body) { body.position.y -= 2; }\n");
    PURR_CHECK(sent_count == 3);
    for (int i = 0; i < sent_count; i++) PURR_CHECK(has(sent[i], "\"diagnostics\":[]"));
    PURR_CHECK(has(request_at(uri_new, "textDocument/hover", 1, 8, ""), "Runs 2nd of 3 systems each tick."));

    remove_game_file("lsp_folder/sub/physics.purr");
    remove_game_file("lsp_folder/main.purr");
    remove_game_file("lsp_folder/notes.txt");
    remove_folder("lsp_folder/sub");
    remove_folder("lsp_folder");
    remove(manifest);
    clear_sent();
    lsp_free(&server);
}

// Starts the server with `folder` open in the editor, as a URI.
static void start_in(const char *folder)
{
    clear_sent();
    lsp_free(&server);
    lsp_init(&server, capture, NULL);
    char message[1024];
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":"
             "{\"rootUri\":\"%s\",\"workspaceFolders\":[{\"uri\":\"%s\",\"name\":\"game\"}]}}",
             folder, folder);
    handle(message);
}

// A folder open in the editor is a game, as `purr run` builds it: every .purr
// file in it and its subfolders, with no manifest.
PURR_TEST(lsp_open_folder_is_a_game)
{
    if (!find_game_dir()) return;
    make_folder("lsp_open");
    make_folder("lsp_open/sub");
    static const char physics[] = "namespace Physics;\n"
                                  "component Body { float2 position; }\n"
                                  "system Gravity(mut Body body) { body.position.y -= 1; }\n";
    static const char main_file[] = "using Physics;\n"
                                    "system Main() { Spawn(Body); }\n";
    write_file("lsp_open/sub/physics.purr", physics);
    write_file("lsp_open/main.purr", main_file);
    char root[700], uri_main[700], uri_physics[700];
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_open", dir); // Editors send folders without the last '/'
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_open/main.purr", dir);
    snprintf(uri_physics, sizeof uri_physics, "file:///%s/lsp_open/sub/physics.purr", dir);

    start_in(root);
    open_uri(uri_main, main_file);
    PURR_CHECK(sent_count == 2);
    PURR_CHECK(has(sent[0], uri_main) && has(sent[0], "\"diagnostics\":[]"));
    PURR_CHECK(has(sent[1], uri_physics) && has(sent[1], "\"diagnostics\":[]"));

    // Without the folder open, the file stands alone.
    start();
    open_uri(uri_main, main_file);
    PURR_CHECK(sent_count == 1 && !has(sent[0], "\"diagnostics\":[]"));

    remove_game_file("lsp_open/sub/physics.purr");
    remove_game_file("lsp_open/main.purr");
    remove_folder("lsp_open/sub");
    remove_folder("lsp_open");
    clear_sent();
    lsp_free(&server);
}

// An open folder that builds its games with CMake has the manifest
// purr_add_game writes, in build/tools. Its games come from there, and its
// other files stand alone instead of making one game of the whole folder.
PURR_TEST(lsp_open_folder_with_manifest)
{
    if (!find_game_dir()) return;
    make_folder("lsp_cmake");
    make_folder("lsp_cmake/build");
    make_folder("lsp_cmake/build/tools");
    make_folder("lsp_cmake/game");
    make_folder("lsp_cmake/tests");
    static const char physics[] = "component Body { float2 position; }\n";
    static const char main_file[] = "system Main() { Spawn(Body); }\n";
    static const char alone[] = "component Body { float2 position; }\nsystem Main() { Spawn(Body); }\n";
    write_file("lsp_cmake/game/physics.purr", physics);
    write_file("lsp_cmake/game/main.purr", main_file);
    write_file("lsp_cmake/tests/alone.purr", alone);
    char manifest_text[700], root[700], uri_main[700], uri_alone[700];
    snprintf(manifest_text, sizeof manifest_text, "game\t%s/lsp_cmake/game/\n", game_dir);
    write_file("lsp_cmake/build/tools/games.txt", manifest_text);
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_cmake", dir);
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_cmake/game/main.purr", dir);
    snprintf(uri_alone, sizeof uri_alone, "file:///%s/lsp_cmake/tests/alone.purr", dir);

    start_in(root);
    open_uri(uri_main, main_file);
    PURR_CHECK(sent_count == 2);
    for (int i = 0; i < sent_count; i++) PURR_CHECK(has(sent[i], "\"diagnostics\":[]"));
    // Its own Body doesn't clash with the game's.
    open_uri(uri_alone, alone);
    PURR_CHECK(sent_count == 1 && has(sent[0], uri_alone) && has(sent[0], "\"diagnostics\":[]"));

    remove_game_file("lsp_cmake/game/physics.purr");
    remove_game_file("lsp_cmake/game/main.purr");
    remove_game_file("lsp_cmake/tests/alone.purr");
    remove_game_file("lsp_cmake/build/tools/games.txt");
    remove_folder("lsp_cmake/game");
    remove_folder("lsp_cmake/tests");
    remove_folder("lsp_cmake/build/tools");
    remove_folder("lsp_cmake/build");
    remove_folder("lsp_cmake");
    clear_sent();
    lsp_free(&server);
}

PURR_TEST(lsp_shutdown_and_exit)
{
    start();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\"}");
    PURR_CHECK(has(last_sent(), "\"id\":9,\"result\":null"));
    handle("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    PURR_CHECK(server.exited && server.exit_code == 0);
    clear_sent();
    lsp_free(&server);
}

// Above each system: its stage and why it waits. Access a system doesn't use
// gets a warning and a quick fix.
PURR_TEST(lsp_schedule_lenses_and_fixes)
{
    start();
    static const char program[] = "component Body { float2 position; float2 velocity; }\n"
                                  "singleton Score { int total; }\n"
                                  "system Main() { Spawn(Body); }\n"
                                  "system Move(mut Body body) { body.position += body.velocity; }\n"
                                  "system Look(mut Body body, mut Score s) { s.total = 0; if (body.position.x > 0) return; }\n"
                                  "system Tag(mut Score s, Body body) { s.total = 1; }\n";
    open_document(program);
    PURR_CHECK(has(sent[0], "'body' is declared mut but never written"));
    PURR_CHECK(has(sent[0], "'body' is never used"));

    const char *lenses = request("textDocument/codeLens");
    PURR_CHECK(has(lenses, "stage 1"));
    PURR_CHECK(has(lenses, "stage 2") && has(lenses, "after Move: both write Body"));
    PURR_CHECK(has(lenses, "stage 3") && has(lenses, "after Look: both write Score; it writes Body, which this reads"));
    PURR_CHECK(has(lenses, "nothing runs alongside"));

    const char *hover = request_at("file:///test.purr", "textDocument/hover", 5, 8, "");
    PURR_CHECK(has(hover, "**Stage 3.**") && has(hover, "`Move`: it writes `Body`, which this reads"));
    PURR_CHECK(has(hover, "No other system can run at the same time."));

    const char *remove_mut = request_at("file:///test.purr", "textDocument/codeAction", 4, 0,
                                        "\"range\":{\"start\":{\"line\":4,\"character\":0},\"end\":{\"line\":4,\"character\":0}}");
    PURR_CHECK(has(remove_mut, "Remove 'mut' from 'body'") && has(remove_mut, "\"newText\":\"\""));
    PURR_CHECK(has(remove_mut, "\"start\":{\"line\":4,\"character\":12}"));
    const char *use_with = request_at("file:///test.purr", "textDocument/codeAction", 5, 0,
                                      "\"range\":{\"start\":{\"line\":5,\"character\":0},\"end\":{\"line\":5,\"character\":0}}");
    PURR_CHECK(has(use_with, "\"newText\":\"with Body\""));

    open_document("component Body { float2 position; }\nsystem Main() { Spawn(Body); }\n"
         "system Push(Body body) { body.position.x = 1; }\n");
    const char *add_mut = request_at("file:///test.purr", "textDocument/codeAction", 2, 0,
                                     "\"range\":{\"start\":{\"line\":2,\"character\":25},\"end\":{\"line\":2,\"character\":29}}");
    PURR_CHECK(has(add_mut, "Declare 'body' as mut") && has(add_mut, "\"newText\":\"mut \""));
    PURR_CHECK(has(add_mut, "\"start\":{\"line\":2,\"character\":12}"));
}
