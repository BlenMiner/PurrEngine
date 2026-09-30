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

// An editor like VS Code: it applies edits that create files.
#define EDITOR_CAPABILITIES                                                                                        \
    "{\"capabilities\":{\"workspace\":{\"workspaceEdit\":{\"documentChanges\":true,"                                \
    "\"resourceOperations\":[\"create\",\"rename\",\"delete\"]}}}}"

static void start_with(const char *params)
{
    clear_sent();
    lsp_free(&server);
    lsp_init(&server, capture, NULL);
    char message[512];
    snprintf(message, sizeof message, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":%s}", params);
    handle(message);
}

static void start(void)
{
    start_with(EDITOR_CAPABILITIES);
}

// Opens `text` as the document, first removing the `$` that marks the cursor.
static int cursor_line;
static int cursor_character;

// Documents are written with the cursor as `$`; `$$` is a `$` of the text,
// as in $$"score {score}".
static void open_document(const char *marked)
{
    char text[8192];
    size_t n = 0;
    int line = 0;
    int character = 0;
    cursor_line = cursor_character = -1;
    for (const char *p = marked; *p && n + 1 < sizeof text; p++) {
        if (*p == '$' && p[1] != '$') {
            cursor_line = line;
            cursor_character = character;
            continue;
        }
        if (*p == '$') p++;
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
    size_t n = 0;
    for (const char *p = original; *p && n + 1 < sizeof applied; p++) { // As open_document reads it
        if (*p == '$' && p[1] != '$') continue;
        if (*p == '$') p++;
        applied[n++] = *p;
    }
    applied[n] = '\0';
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
    "scene Main { }\n"                                                         \
    "event(Spawned) Setup(with Main)\n"                                        \
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
    PURR_CHECK(has(last_sent(), "\"range\":{\"start\":{\"line\":24,\"character\":9},\"end\":{\"line\":24,\"character\":12}}"));
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

// Inside Sanitize, the input's fields are in scope; this machine's devices aren't.
PURR_TEST(lsp_sanitize)
{
    start();
    static const char program[] = "input PlayerInput\n{\n    float move;\n\n    Sample() { }\n\n"
                                  "    Sanitize()\n    {\n        move = Math.Clamp($, -1, 1);\n    }\n}\n"
                                  "scene Main { }\n";
    const char *reply = complete(program);
    PURR_CHECK(offers(reply, "move"));
    PURR_CHECK(!offers(reply, "Devices"));

    open_document("input PlayerInput\n{\n    float move;\n    Sani$tize() { move = Math.Clamp(move, -1, 1); }\n}\n"
         "scene Main { }\n");
    PURR_CHECK(has(sent[0], "\"diagnostics\":[]"));
    PURR_CHECK(has(request("textDocument/hover"), "Runs on every input before the simulation reads it"));

    open_document("input PlayerInput\n{\n    bool jump;\n    Sam$ple() { jump = Devices.keyboard.space.pressed; }\n}\n"
         "scene Main { }\n");
    PURR_CHECK(has(sent[0], "\"diagnostics\":[]"));
    PURR_CHECK(has(request("textDocument/hover"), "Builds the player's input from this machine's `Devices`"));
}

static const char *format_reply(const char *text);
static const char *request_at(const char *uri, const char *method, int line, int character, const char *extra);

// [Clamp], [Min] and [Max] on input fields: completion, and formatting.
PURR_TEST(lsp_field_attributes)
{
    start();
    const char *names = complete("input PlayerInput\n{\n    [$] float move;\n}\nscene Main { }\n");
    PURR_CHECK(offers(names, "Clamp") && offers(names, "Min") && offers(names, "Max"));
    PURR_CHECK(!offers(names, "Before"));
    const char *bounds = complete("input PlayerInput\n{\n    [Clamp(float2(0, 0), $)] float2 aim;\n}\nscene Main { }\n");
    PURR_CHECK(offers(bounds, "Math") && offers(bounds, "float2"));

    static const char messy[] = "input PlayerInput\n{\n[Clamp(-1,1)]float move;\n    [Min(0),Max(3)]   int gear;\n}\n"
                                "scene Main { }\n";
    static const char expected[] = "input PlayerInput\n{\n    [Clamp(-1, 1)] float move;\n    [Min(0), Max(3)] int gear;\n}\n"
                                   "scene Main { }\n";
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
    PURR_CHECK(!offers(view, "Body")); // Views spawn local entities, never the match's
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
    const char *devices = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n"
                                   "        var keys = Devices.$\n    }\n}\nscene Main { }\n");
    PURR_CHECK(offers(devices, "keyboard"));
    PURR_CHECK(offers(devices, "gamepad"));

    const char *keys = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n"
                                "        var keys = Devices.keyboard;\n        if (keys.$\n    }\n}\nscene Main { }\n");
    PURR_CHECK(offers(keys, "space"));
    PURR_CHECK(offers(keys, "leftShift"));

    // Sample takes local singletons; Devices is a name.
    const char *params = complete("local singleton Menu { bool open; }\ninput PlayerInput\n{\n    float2 move;\n\n"
                                  "    Sample($)\n    {\n    }\n}\nscene Main { }\n");
    PURR_CHECK(offers(params, "Menu"));
    PURR_CHECK(!offers(params, "Devices"));
    const char *names = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n        move = $\n"
                                 "    }\n}\nscene Main { }\n");
    PURR_CHECK(offers(names, "Devices"));

    // Views read them too, and systems take them.
    const char *view = complete("scene Main { }\nview Pause()\n{\n    if (Devices.keyboard.$\n}\n");
    PURR_CHECK(offers(view, "escape"));
    const char *system = complete("scene Main { }\ncomponent Body { float x; }\n"
                                  "system Move(Devices devices, mut Body body)\n{\n    body.x += devices.gamepad.$\n}\n");
    PURR_CHECK(offers(system, "leftStick"));
    const char *header = complete("scene Main { }\ncomponent Body { float x; }\nsystem Move($)\n{\n}\n");
    PURR_CHECK(offers(header, "Devices"));
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
    PURR_CHECK(has(param, "\"range\":{\"start\":{\"line\":22,\"character\":21}"));

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

    static const char constructor[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                                      "        var keys = Devices.key$board;\n        fire = keys.space.pressed;\n    }\n}\n"
                                      "scene Main { }\n";
    PURR_CHECK(has(c_definition(constructor), "purr_keyboard keyboard;"));
    static const char key[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                              "        var keys = Devices.keyboard;\n        fire = keys.spa$ce.pressed;\n    }\n}\n"
                              "scene Main { }\n";
    PURR_CHECK(has(c_definition(key), "X(space)"));
    static const char button[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                                 "        fire = Devices.mouse.left.pre$ssed;\n    }\n}\nscene Main { }\n";
    PURR_CHECK(has(c_definition(button), "bool pressed;"));
    static const char devices[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                                  "        fire = Devi$ces.mouse.left.pressed;\n    }\n}\nscene Main { }\n";
    PURR_CHECK(has(c_definition(devices), "typedef struct purr_devices"));
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
    open_document("/* \xC3\xA9 */ component Body { float x; }\nscene Main { }\n");
    PURR_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,18,4,2,1,"));

    // Built-in value types are keywords (11), like C#'s float: `float3` 7 columns after `Body`.
    open_document("component Body { float3 p; }\nscene Main { }\n");
    PURR_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,10,4,2,1,0,7,6,11,0,"));
    // Sample and Sanitize are keywords too, and attributes decorators (12).
    open_document("input PlayerInput\n{\n    [Clamp(-1, 1)] float move;\n    Sample() { }\n}\nscene Main { }\n");
    const char *input = request("textDocument/semanticTokens/full");
    PURR_CHECK(has(input, "2,5,5,12,0,")); // Clamp: line +2, column 5
    PURR_CHECK(has(input, "1,4,6,11,0,")); // Sample: line +1, column 4
}

#define EVENTS                                                                 \
    GAME_TYPES                                                                 \
    "event Hit\n"                                                              \
    "{\n"                                                                      \
    "    int damage = 1;\n"                                                    \
    "}\n"                                                                      \
    "\n"                                                                       \
    "system Strike(Entity self, with Body)\n"                                  \
    "{\n"                                                                      \
    "    self.Send(Hit { damage = 2 });\n"                                     \
    "}\n"                                                                      \
    "\n"                                                                       \
    "event(Hit hit) TakeHit(mut Body body)\n"                                  \
    "{\n"                                                                      \
    "    body.radius -= hit.damage;\n"                                         \
    "}\n"                                                                      \
    "\n"                                                                       \
    "event(Spawned) Grow(mut Body body)\n"                                     \
    "{\n"                                                                      \
    "    body.radius += 1;\n"                                                  \
    "}\n"

PURR_TEST(lsp_events)
{
    start();
    open_document(EVENTS);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // The handler: its trigger, and when it runs.
    open_document(GAME_TYPES "event Hit { int damage; }\nsystem S(Entity e, with Body) { e.Send(Hit); }\n"
                  "event(Hit hit) Take$Hit(mut Body body) { body.radius -= hit.damage; }\n");
    const char *handler = request("textDocument/hover");
    PURR_CHECK(has(handler, "event(Hit hit) TakeHit(mut Body body)"));
    PURR_CHECK(has(handler, "Runs when a `Hit` is sent to an entity that matches its parameters"));

    open_document(GAME_TYPES "event(Spawn$ed) Grow(mut Body body) { body.radius += 1; }\n");
    PURR_CHECK(has(request("textDocument/hover"), "Sent to each entity as it's spawned"));

    open_document(GAME_TYPES "event Hit { int damage; }\nsystem S(Entity e, with Body) { e.Send(Hit); }\n"
                  "event(Hit hit) TakeHit(mut Body body) { body.radius -= h$it.damage; }\n");
    PURR_CHECK(has(request("textDocument/hover"), "The event being handled"));

    // Completion: declarations, the trigger, a handler's parameters, an event's fields, and Send.
    const char *top = complete(GAME_TYPES "\n$");
    PURR_CHECK(offers(top, "event"));
    PURR_CHECK(offers(top, "event handler"));
    const char *trigger = complete(GAME_TYPES "event Hit { int damage; }\nevent($)\n");
    PURR_CHECK(offers(trigger, "Hit"));
    PURR_CHECK(offers(trigger, "Spawned"));
    PURR_CHECK(!offers(trigger, "Devices"));
    const char *params = complete(GAME_TYPES "event Hit { int damage; }\nevent(Hit hit) TakeHit($)\n{\n}\n");
    PURR_CHECK(offers(params, "mut"));
    PURR_CHECK(offers(params, "Body"));
    const char *fields = complete(GAME_TYPES "event Hit { int damage; }\nevent(Hit hit) TakeHit(mut Body body)\n{\n"
                                  "    body.radius -= hit.$\n}\n");
    PURR_CHECK(offers(fields, "damage"));
    const char *send = complete(GAME_TYPES "event Hit { int damage; }\nsystem S(Entity e)\n{\n    e.$\n}\n");
    PURR_CHECK(offers(send, "Send"));
    const char *literal = complete(GAME_TYPES "event Hit { int damage; }\nsystem S(Entity e)\n{\n    e.Send(Hit { $ });\n}\n");
    PURR_CHECK(offers(literal, "damage"));

    // The outline: events are events (24), and handlers say what they are.
    open_document(EVENTS);
    const char *symbols = request("textDocument/documentSymbol");
    PURR_CHECK(has(symbols, "\"name\":\"Hit\",\"detail\":\"event\",\"kind\":24"));
    PURR_CHECK(has(symbols, "\"name\":\"TakeHit\",\"detail\":\"event handler\""));
}

PURR_TEST(lsp_local_state)
{
    start();
    static const char game[] = GAME_TYPES
        "local component Spark { int framesLeft = 2; }\n"
        "local singleton Menu { bool open; }\n"
        "view Trail(with Body, mut Menu menu) { Spawn(Spark); menu.open = true; }\n"
        "view Fade(LocalEntity self, mut Spark spark) { spark.framesLeft -= 1; }\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document(GAME_TYPES "local component Sp$ark { int framesLeft = 2; }\nview Fade(mut Spark spark) { spark.framesLeft -= 1; }\n");
    const char *hover = request("textDocument/hover");
    PURR_CHECK(has(hover, "local component Spark"));
    PURR_CHECK(has(hover, "Local: this machine's own"));

    // In a view, Spawn makes local entities: the local components are offered, not the match's.
    const char *spawn = complete(GAME_TYPES "local component Spark { int framesLeft = 2; }\nview Trail(Body body)\n{\n    $\n}\n");
    PURR_CHECK(offers(spawn, "Spawn"));
    PURR_CHECK(offers(spawn, "Spark"));
    PURR_CHECK(!offers(spawn, "Body"));
    const char *entity = complete(GAME_TYPES "local component Spark { int framesLeft = 2; }\n"
                                  "view Fade(LocalEntity self, Spark spark)\n{\n    self.$\n}\n");
    PURR_CHECK(offers(entity, "Destroy"));
    PURR_CHECK(offers(complete(GAME_TYPES "\n$"), "local component"));

    open_document(game);
    PURR_CHECK(has(request("textDocument/documentSymbol"), "\"name\":\"Spark\",\"detail\":\"local component\""));
}


PURR_TEST(lsp_enums_and_switch)
{
    start();
    static const char game[] = "enum Phase { Warmup, Playing = 5 }\nsingleton Match { Phase phase; int n; }\nscene Main { }\n"
                               "system S(mut Match match)\n{\n    switch (match.phase)\n    {\n        case Phase.Warmup:\n"
                               "            match.n = 1;\n            break;\n        default:\n            break;\n    }\n}\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document("enum Pha$se { Warmup, Playing = 5 }\nscene Main { }\n");
    const char *type = request("textDocument/hover");
    PURR_CHECK(has(type, "enum Phase"));
    PURR_CHECK(has(type, "Playing = 5,"));

    open_document("enum Phase { Warmup, Playing = 5 }\nsingleton Match { Phase phase; }\nscene Main { }\n"
                  "system S(mut Match match) { match.phase = Phase.Play$ing; }\n");
    PURR_CHECK(has(request("textDocument/hover"), "Phase.Playing = 5"));

    const char *members = complete("enum Phase { Warmup, Playing = 5 }\nsingleton Match { Phase phase; }\nscene Main { }\n"
                                   "system S(mut Match match)\n{\n    match.phase = Phase.$\n}\n");
    PURR_CHECK(offers(members, "Warmup"));
    PURR_CHECK(offers(members, "Playing"));

    open_document(game);
    const char *symbols = request("textDocument/documentSymbol");
    PURR_CHECK(has(symbols, "\"name\":\"Phase\",\"detail\":\"enum\",\"kind\":10"));
    PURR_CHECK(has(symbols, "\"name\":\"Playing\",\"detail\":\"5\",\"kind\":22"));
}

PURR_TEST(lsp_format_switch)
{
    start();
    static const char messy[] =
        "enum Phase { Warmup, Playing }\nsingleton Match { Phase phase; int n; }\nsystem Main() { }\n"
        "system S(mut Match match)\n{\nswitch (match.phase)\n{\ncase Phase.Warmup :\nmatch.n = 1;\nbreak;\n"
        "default:\n// Nothing to do\nif (match.n > 1) { match.n = 0; }\nbreak;\n}\n}\n";
    static const char expected[] =
        "enum Phase { Warmup, Playing }\nsingleton Match { Phase phase; int n; }\nsystem Main() { }\n"
        "system S(mut Match match)\n{\n    switch (match.phase)\n    {\n        case Phase.Warmup:\n            match.n = 1;\n"
        "            break;\n        default:\n            // Nothing to do\n            if (match.n > 1) { match.n = 0; }\n"
        "            break;\n    }\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
    PURR_CHECK(has(format_reply(expected), "\"result\":[]"));
}


PURR_TEST(lsp_gui)
{
    start();
    static const char game[] =
        "local singleton Menu { bool open; float volume; }\nscene Main { }\n\n"
        "void Section(string title, mut bool open, Block content)\n{\n    GUILayout.Toggle(title, open);\n"
        "    if (open) content();\n}\n\n"
        "view Options(mut Menu menu)\n{\n    GUILayout.Area(Anchor.MiddleCenter)\n    {\n"
        "        Section(\"Audio\", menu.open)\n        {\n            GUILayout.Slider(\"Volume\", menu.volume, 0, 1);\n"
        "        }\n    }\n}\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    PURR_CHECK(has(format_reply(game), "\"result\":[]")); // Blocks after calls keep their braces on lines of their own

    const char *widgets = complete("scene Main { }\nview V()\n{\n    GUILayout.$\n}\n");
    PURR_CHECK(offers(widgets, "Button"));
    PURR_CHECK(offers(widgets, "Horizontal"));
    PURR_CHECK(offers(widgets, "Area"));
    PURR_CHECK(offers(complete("scene Main { }\nview V()\n{\n    $\n}\n"), "GUILayout"));
    PURR_CHECK(!offers(complete("scene Main { }\nsystem S()\n{\n    $\n}\n"), "GUILayout"));
    PURR_CHECK(offers(complete("scene Main { }\nvoid F()\n{\n    $\n}\n"), "Screen"));

    open_document("local singleton M { bool on; }\nscene Main { }\nview V(mut M m) { GUILayout.Tog$gle(\"On\", m.on); }\n");
    const char *toggle = request("textDocument/hover");
    PURR_CHECK(has(toggle, "GUILayout.Toggle(string text, mut bool value) -> bool"));
    PURR_CHECK(has(toggle, "Returns whether it changed it"));

    open_document("scene Main { }\nvoid Twice(Block content) { con$tent(); content(); }\n");
    PURR_CHECK(has(request("textDocument/hover"), "Block content"));

    open_document("scene Main { }\nview V() { var w = Screen.wid$th; }\n");
    PURR_CHECK(has(request("textDocument/hover"), "Screen.width: float"));

    // What a Block's caller writes is the caller's code: its names are the caller's.
    open_document("local singleton M { bool on; }\nscene Main { }\nvoid Twice(Block content) { content(); content(); }\n"
                  "view V(mut M m)\n{\n    Twice()\n    {\n        m.o$n = true;\n    }\n}\n");
    PURR_CHECK(has(request("textDocument/hover"), "bool on"));
}

PURR_TEST(lsp_format_blocks)
{
    start();
    static const char messy[] = "scene Main { }\nview V()\n{\nGUILayout.Horizontal() {\nGUILayout.Label(\"a\");\n}\n"
                                "GUILayout.Vertical() { GUILayout.Label(\"b\"); }\n}\n";
    static const char expected[] = "scene Main { }\nview V()\n{\n    GUILayout.Horizontal()\n    {\n        GUILayout.Label(\"a\");\n"
                                   "    }\n    GUILayout.Vertical() { GUILayout.Label(\"b\"); }\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


PURR_TEST(lsp_loops)
{
    start();
    static const char game[] = "component Tally { int n; }\nscene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Tally); }\n\n"
                               "system Count(mut Tally t)\n{\n    for (var i = 0; i < 10; i++)\n    {\n"
                               "        if (i % 2 == 1) continue;\n        t.n += i;\n    }\n"
                               "    mut var left = 3;\n    while (left > 0) { left--; }\n}\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    PURR_CHECK(has(format_reply(game), "\"result\":[]")); // Already formatted

    // The for's variable is in scope in its body.
    PURR_CHECK(offers(complete("component Tally { int n; }\nscene Main { }\nsystem S(mut Tally t)\n{\n"
                               "    for (var index = 0; index < 3; index++)\n    {\n        t.n = $\n    }\n}\n"),
                      "index"));
    PURR_CHECK(offers(complete("scene Main { }\nsystem S()\n{\n    $\n}\n"), "while"));
    open_document("component Tally { int n; }\nscene Main { }\nsystem S(mut Tally t)\n{\n"
                  "    for (var index = 0; index < 3; index++) t.n += ind$ex;\n}\n");
    PURR_CHECK(has(request("textDocument/hover"), "int index"));
}

PURR_TEST(lsp_format_loops)
{
    start();
    static const char messy[] = "scene Main { }\nsystem S()\n{\nmut var n = 0;\nfor(var i=0;i<3;i ++){\nn+=i;\n}\n"
                                "while (n > 0) {\nn --;\nif (n == 1) break;\n}\n}\n";
    static const char expected[] = "scene Main { }\nsystem S()\n{\n    mut var n = 0;\n    for (var i = 0; i < 3; i++)\n    {\n"
                                   "        n += i;\n    }\n    while (n > 0)\n    {\n        n--;\n        if (n == 1) break;\n"
                                   "    }\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


PURR_TEST(lsp_text)
{
    start();
    static const char game[] = "local singleton Score { int points; float time; }\nscene Main { }\n\n"
                               "view Hud(Score score)\n{\n    var name = \"cat\";\n"
                               "    GUILayout.Label($$\"{name}: {score.points:D3} in {score.time:F1}s\" + \"!\");\n}\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    PURR_CHECK(has(format_reply(game), "\"result\":[]")); // Values sit against their braces

    // Completion works in a value, not in the text around it.
    PURR_CHECK(offers(complete("local singleton Score { int points; }\nscene Main { }\nview V(Score score)\n{\n"
                               "    GUILayout.Label($$\"points: {sco$}\");\n}\n"),
                      "score"));
    PURR_CHECK(!offers(complete("local singleton Score { int points; }\nscene Main { }\nview V(Score score)\n{\n"
                                "    GUILayout.Label($$\"poi$ {score.points}\");\n}\n"),
                       "score"));
    const char *methods = complete("scene Main { }\nview V()\n{\n    var name = \"cat\";\n    var n = name.$\n}\n");
    PURR_CHECK(offers(methods, "Length"));
    PURR_CHECK(offers(methods, "Contains"));

    open_document("scene Main { }\nview V() { var n = \"cat\".Cont$ains(\"a\"); }\n");
    PURR_CHECK(has(request("textDocument/hover"), "Whether `value` is in the text."));

    // Text in fields
    PURR_CHECK(offers(complete("component Name\n{\n    $\n}\nscene Main { }\n"), "string"));
    open_document("component Name { string va$lue = \"cat\"; }\nscene Main { }\n");
    PURR_CHECK(has(request("textDocument/hover"), "string value"));
    const char *fields = complete("component Name { string value; }\nscene Main { }\nsystem S(Name name)\n{\n"
                                  "    var n = name.value.$\n}\n");
    PURR_CHECK(offers(fields, "Length"));
}

PURR_TEST(lsp_format_text)
{
    start();
    static const char messy[] = "scene Main { }\nview V()\n{\nvar a = $$\"x { 1 + 2 :F2} y {3}\";\n}\n";
    static const char expected[] = "scene Main { }\nview V()\n{\n    var a = $\"x {1 + 2:F2} y {3}\";\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


PURR_TEST(lsp_lists)
{
    start();
    static const char game[] = "component Inventory { List<int> scores = [1, 2]; }\nscene Main { }\n"
                               "event(Spawned) Setup(with Main) { Spawn(Inventory); }\n\n"
                               "system Count(mut Inventory inv)\n{\n    foreach (var score in inv.scores)\n    {\n"
                               "        if (score > 1) inv.scores.Add(score);\n    }\n}\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    PURR_CHECK(has(format_reply(game), "\"result\":[]")); // List<int> keeps no spaces

    const char *members = complete("component Inventory { List<int> scores; }\nscene Main { }\nsystem S(Inventory inv)\n{\n"
                                   "    var n = inv.scores.$\n}\n");
    PURR_CHECK(offers(members, "Count"));
    PURR_CHECK(offers(members, "Add"));
    PURR_CHECK(offers(complete("component Inventory\n{\n    $\n}\nscene Main { }\n"), "List"));

    // The loop's variable is in scope in its body.
    PURR_CHECK(offers(complete("component Inventory { List<int> scores; }\nscene Main { }\nsystem S(mut Inventory inv)\n{\n"
                               "    foreach (var score in inv.scores)\n    {\n        var x = $\n    }\n}\n"),
                      "score"));
    open_document("component Inventory { List<int> sco$res; }\nscene Main { }\n");
    PURR_CHECK(has(request("textDocument/hover"), "List<int> scores"));
}

PURR_TEST(lsp_format_lists)
{
    start();
    static const char messy[] = "scene Main { }\nint F(List < int > xs)\n{\nmut List<int> ys = [ 1,2 ];\nys.Add(xs [0]);\nreturn ys.Count;\n}\n";
    static const char expected[] = "scene Main { }\nint F(List<int> xs)\n{\n    mut List<int> ys = [1, 2];\n    ys.Add(xs[0]);\n"
                                   "    return ys.Count;\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


PURR_TEST(lsp_scenes)
{
    start();
    static const char game[] = "scene Arena { int size = 20; }\nscene Main { }\n"
                               "event(Spawned) Setup(with Main) { Scene.Load(Arena { size = 30 }); }\n"
                               "system Close(Entity self, Arena arena) { if (arena.size > 40) Scene.Unload(self); }\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document("scene Are$na { int size = 20; }\nscene Main { }\nevent(Spawned) Setup(with Main) { Scene.Load(Arena); }\n");
    const char *type = request("textDocument/hover");
    PURR_CHECK(has(type, "scene Arena"));
    PURR_CHECK(has(type, "int size = 20;"));
    PURR_CHECK(!has(type, "purr_players")); // The engine's own fields stay hidden

    open_document("scene Arena { }\nscene Main { }\nevent(Spawned) Setup(with Main) { Scene.Lo$ad(Arena); }\n");
    PURR_CHECK(has(request("textDocument/hover"), "Loads a scene and returns its entity"));

    const char *calls = complete("scene Arena { }\nscene Main { }\nevent(Spawned) Setup(with Main)\n{\n    Scene.$\n}\n");
    PURR_CHECK(offers(calls, "Load"));
    PURR_CHECK(offers(calls, "Unload"));
    const char *fields = complete("scene Arena { int size; }\nscene Main { }\nsystem S(Arena arena)\n{\n    var x = arena.$\n}\n");
    PURR_CHECK(offers(fields, "size"));
    PURR_CHECK(!offers(fields, "purr_visibility"));

    open_document(game);
    const char *symbols = request("textDocument/documentSymbol");
    PURR_CHECK(has(symbols, "\"name\":\"Arena\",\"detail\":\"scene\""));
    PURR_CHECK(!has(symbols, "purr_players"));
}

// Snap: on entities and singletons in match code. A type's Interpolate isn't for calling.
PURR_TEST(lsp_snap)
{
    start();
    const char *entity = complete("component Body { float2 p; }\nscene Main { }\n"
                                  "system Respawn(Entity self, mut Body body)\n{\n    self.$\n}\n");
    PURR_CHECK(offers(entity, "Snap"));
    PURR_CHECK(offers(entity, "Destroy"));
    const char *singleton = complete("singleton Camera { float2 center; }\nscene Main { }\n"
                                     "system Cut(mut Camera camera)\n{\n    camera.$\n}\n");
    PURR_CHECK(offers(singleton, "Snap"));
    PURR_CHECK(offers(singleton, "center"));
    const char *methods = complete("struct Angle\n{\n    float degrees;\n    Angle Interpolate(Angle from, Angle to, float t) { return to; }\n"
                                   "    float Radians() { return degrees; }\n}\ncomponent Body { Angle heading; }\nscene Main { }\n"
                                   "system S(Body body)\n{\n    var r = body.heading.$\n}\n");
    PURR_CHECK(offers(methods, "Radians"));
    PURR_CHECK(!offers(methods, "Interpolate"));
}

// Session: its calls from local code, and its singleton and events.
PURR_TEST(lsp_sessions)
{
    start();
    static const char game[] = "local scene Main { }\nscene Arena { int size = 20; }\n"
                               "view Menu(Session session)\n{\n"
                               "    if (GUILayout.Button(\"Host\") && session.state == SessionState.Offline) Session.Host(Arena, 7777);\n"
                               "    if (GUILayout.Button(\"Join\")) Session.Join(\"127.0.0.1\");\n}\n"
                               "local event(Disconnected gone) Lost() { }\n";
    open_document(game);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // The name itself, where a statement starts in local code, even inside a
    // container's block and a branch.
    const char *name = complete("local scene Main { }\nview Menu()\n{\n    GUILayout.Area(Anchor.MiddleCenter)\n    {\n"
                                "        if (GUILayout.Button(\"Host\"))\n        {\n            Sess$\n        }\n    }\n}\n");
    PURR_CHECK(offers(name, "Session"));
    PURR_CHECK(offers(complete("local scene Main { }\nlocal event(Disconnected gone) Lost()\n{\n    $\n}\n"), "Session"));
    PURR_CHECK(!offers(complete("scene Main { }\nsystem Move()\n{\n    $\n}\n"), "Session"));
    PURR_CHECK(!offers(complete("local scene Main { }\nview Menu()\n{\n    var x = $\n}\n"), "Session"));

    const char *calls = complete("local scene Main { }\nview Menu()\n{\n    Session.$\n}\n");
    PURR_CHECK(offers(calls, "Play"));
    PURR_CHECK(offers(calls, "Join"));
    PURR_CHECK(offers(calls, "Leave"));
    const char *fields = complete("local scene Main { }\nview Menu(Session session)\n{\n    var s = session.$\n}\n");
    PURR_CHECK(offers(fields, "state"));
    PURR_CHECK(offers(fields, "ping"));

    open_document("local scene Main { }\nview Menu()\n{\n    Session.Le$ave();\n}\n");
    PURR_CHECK(has(request("textDocument/hover"), "Leaves the match"));
}


PURR_TEST(lsp_format_events)
{
    start();
    static const char messy[] = "event Hit { int damage; }\nsystem Main() { Send(Hit); }\n"
                                "event (Hit hit) TakeHit( ) { }\nevent ( Spawned ) Grow() { }\n";
    static const char expected[] = "event Hit { int damage; }\nsystem Main() { Send(Hit); }\n"
                                   "event(Hit hit) TakeHit() { }\nevent(Spawned) Grow() { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
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
    "scene Main { }\n"                                                         \
    "event(Spawned) Setup(with Main)\n"                                        \
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

    open_document("struct St$ats\n{\n    float health = 100;\n}\nscene Main { }\n");
    const char *hover = request("textDocument/hover");
    PURR_CHECK(has(hover, "struct Stats"));
    PURR_CHECK(has(hover, "float health = 100;"));

    // Laid out like the other declarations.
    static const char messy[] = "struct Stats {\nfloat health = 100;\n}\nscene Main { }\n";
    format_reply(messy);
    PURR_CHECK(strcmp(apply_reply(messy, NULL), "struct Stats\n{\n    float health = 100;\n}\nscene Main { }\n") == 0);
}

#define METHODS                                                                \
    "struct Stats\n"                                                           \
    "{\n"                                                                      \
    "    float health = 100;\n"                                                \
    "\n"                                                                       \
    "    bool IsDead() { return health <= 0; }\n"                              \
    "    mut void Hurt(float amount) { health -= amount; }\n"                  \
    "}\n"                                                                      \
    "\n"                                                                       \
    "component Unit { Stats stats; }\n"                                        \
    "\n"                                                                       \
    "float Heal(mut Stats stats, float amount)\n"                              \
    "{\n"                                                                      \
    "    stats.health += amount;\n"                                            \
    "    return stats.health;\n"                                               \
    "}\n"                                                                      \
    "\n"                                                                       \
    "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Unit); }\n"

PURR_TEST(lsp_methods_and_functions)
{
    start();
    open_document(METHODS);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    const char *symbols = request("textDocument/documentSymbol");
    PURR_CHECK(has(symbols, "\"name\":\"IsDead\",\"detail\":\"bool IsDead()\",\"kind\":6"));
    PURR_CHECK(has(symbols, "\"name\":\"Heal\",\"detail\":\"function\",\"kind\":12"));

    // After a dot, methods with their signatures; in code, functions; in a method, its type's fields.
    const char *members = complete(METHODS "system Fight(mut Unit unit)\n{\n    unit.stats.$\n}\n");
    PURR_CHECK(offers(members, "IsDead") && offers(members, "Hurt") && offers(members, "health"));
    PURR_CHECK(has(members, "mut void Hurt(float amount)"));
    PURR_CHECK(offers(complete(METHODS "system Fight(mut Unit unit)\n{\n    $\n}\n"), "Heal"));
    PURR_CHECK(offers(complete("struct Stats\n{\n    float health;\n    bool IsDead() { return $ }\n}\nscene Main { }\n"),
                      "health"));

    // Hover, definition and references go to the method itself.
    open_document(METHODS "system Fight(mut Unit unit)\n{\n    unit.stats.Hu$rt(1);\n}\n");
    const char *hover = request("textDocument/hover");
    PURR_CHECK(has(hover, "mut void Hurt(float amount)"));
    PURR_CHECK(has(hover, "Method of struct `Stats`"));
    PURR_CHECK(has(request("textDocument/definition"), "\"range\":{\"start\":{\"line\":5,\"character\":13}"));

    open_document(METHODS "system Fight(mut Unit unit)\n{\n    He$al(unit.stats, 1);\n}\n");
    PURR_CHECK(has(request("textDocument/hover"), "float Heal(mut Stats stats, float amount)"));
    open_document(METHODS "system Fight(mut Unit unit)\n{\n    Heal(unit.stats, $\n}\n");
    const char *signature = request("textDocument/signatureHelp");
    PURR_CHECK(has(signature, "float Heal(mut Stats stats, float amount)"));
    PURR_CHECK(has(signature, "\"activeParameter\":1"));

    // Inlay hints: what a var is, and parameter names at literal arguments.
    open_document(METHODS "system Fight(mut Unit unit)\n{\n    var left = Heal(unit.stats, 2);\n}\n");
    const char *hints = request_at("file:///test.purr", "textDocument/inlayHint", 0, 0,
                                   "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":30,\"character\":0}}");
    PURR_CHECK(has(hints, "{\"position\":{\"line\":20,\"character\":12},\"label\":\": float\",\"kind\":1"));
    PURR_CHECK(has(hints, "{\"position\":{\"line\":20,\"character\":32},\"label\":\"amount:\",\"kind\":2"));
    PURR_CHECK(!has(hints, "\"stats:\"")); // Not for arguments that already say what they are

    // Workspace symbols: every declaration of the game, methods with their type, by fuzzy name.
    open_document(METHODS);
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/symbol\",\"params\":{\"query\":\"hrt\"}}");
    PURR_CHECK(has(last_sent(), "\"name\":\"Hurt\",\"kind\":6") && has(last_sent(), "\"containerName\":\"Stats\""));
    PURR_CHECK(!has(last_sent(), "\"name\":\"Heal\""));
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/symbol\",\"params\":{\"query\":\"\"}}");
    PURR_CHECK(has(last_sent(), "\"name\":\"Heal\",\"kind\":12") && has(last_sent(), "\"name\":\"Unit\",\"kind\":23"));

    // Laid out like other code: braces on their own lines, one-liners kept.
    static const char messy[] = "struct Stats {\nfloat health;\nbool IsDead() { return health <= 0; }\n"
                                "mut void Hurt() {\nhealth -= 1; }\n}\nvoid Reset(mut Stats s) {\ns.health = 0; }\n"
                                "scene Main { }\n";
    static const char expected[] = "struct Stats\n{\n    float health;\n    bool IsDead() { return health <= 0; }\n"
                                   "    mut void Hurt()\n    {\n        health -= 1;\n    }\n}\n"
                                   "void Reset(mut Stats s)\n{\n    s.health = 0;\n}\nscene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}

#define EXTERNS                                                                \
    "[NativeName(\"stb_perlin_noise3\")]\n"                                    \
    "extern float Noise(float x, float y);\n"                                  \
    "extern int twice(int x);\n"                                               \
    "extern float Weigh(in float3 v, List<float> weights, string name);\n"    \
    "\n"                                                                       \
    "component Ground { float height; }\n"                                     \
    "scene Main { }\n"                                                         \
    "event(Spawned) Setup(with Main)\n"                                        \
    "{\n"                                                                      \
    "    Spawn(Ground);\n"                                                     \
    "}\n"

PURR_TEST(lsp_extern_functions)
{
    start();
    open_document(EXTERNS);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // Hover says it's C's, and which C function.
    open_document(EXTERNS "system Shape(mut Ground ground)\n{\n    ground.height = No$ise(1, 2);\n}\n");
    const char *hover = request("textDocument/hover");
    PURR_CHECK(has(hover, "extern float Noise(float x, float y)"));
    PURR_CHECK(has(hover, "[NativeName(\\\"stb_perlin_noise3\\\")]"));
    PURR_CHECK(has(hover, "C function `stb_perlin_noise3`"));
    PURR_CHECK(has(request("textDocument/definition"), "\"range\":{\"start\":{\"line\":1,\"character\":13}"));
    open_document(EXTERNS "system Shape(mut Ground ground)\n{\n    ground.height = tw$ice(2);\n}\n");
    PURR_CHECK(has(request("textDocument/hover"), "C function `twice`"));
    open_document(EXTERNS "system Shape(mut Ground ground)\n{\n    ground.height = We$igh(float3(1), [1], \"a\");\n}\n");
    PURR_CHECK(has(request("textDocument/hover"), "extern float Weigh(in float3 v, List<float> weights, string name)"));

    // Completion: the declaration, its return type and parameters, the attribute, and calls.
    PURR_CHECK(offers(complete(EXTERNS "$"), "extern"));
    PURR_CHECK(offers(complete(EXTERNS "extern $"), "float"));
    const char *params = complete(EXTERNS "extern float Sum($");
    PURR_CHECK(offers(params, "in") && offers(params, "mut") && offers(params, "float3"));
    PURR_CHECK(offers(complete(EXTERNS "extern float Sum(in $"), "float3"));
    PURR_CHECK(offers(complete(EXTERNS "[$"), "NativeName"));
    PURR_CHECK(offers(complete(EXTERNS "system Shape(mut Ground ground)\n{\n    $\n}\n"), "Noise"));

    // Errors come with the code around them.
    open_document("scene Main { }\nextern void Log(List<string> lines);\n");
    PURR_CHECK(has(last_sent(), "C functions can't take lists of text yet"));

    // The formatter leaves an extern on its line.
    static const char messy[] = "extern  float Noise( float x ,float y ) ;\nscene Main { }\n";
    static const char expected[] = "extern float Noise(float x, float y);\nscene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}

#define OPERATORS                                                              \
    "struct Money\n"                                                           \
    "{\n"                                                                      \
    "    int cents;\n"                                                         \
    "\n"                                                                       \
    "    Money operator +(Money a, Money b) { return Money { cents = a.cents + b.cents }; }\n" \
    "    int Dollars() { return cents / 100; }\n"                              \
    "}\n"                                                                      \
    "\n"                                                                       \
    "singleton Wallet { Money total; }\n"                                      \
    "\n"                                                                       \
    "scene Main { }\n"                                                         \
    "event(Spawned) Setup(with Main, mut Wallet wallet)\n"                     \
    "{\n"                                                                      \
    "    wallet.total = wallet.total + Money { cents = 5 };\n"                 \
    "}\n"

PURR_TEST(lsp_operators)
{
    start();
    open_document(OPERATORS);
    PURR_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    // `+` in code goes to the operator.
    const char *hover = request_at("file:///test.purr", "textDocument/hover", 13, 32, "");
    PURR_CHECK(has(hover, "Money operator +(Money a, Money b)") && has(hover, "Operator of struct `Money`."));
    PURR_CHECK(has(request_at("file:///test.purr", "textDocument/definition", 13, 32, ""),
                   "\"range\":{\"start\":{\"line\":4,\"character\":10}"));
    PURR_CHECK(has(request_at("file:///test.purr", "textDocument/prepareRename", 13, 32, ""),
                   "Operators are named by their symbol"));
    // Methods are listed after a dot; operators aren't.
    const char *members = complete(OPERATORS "system S(Wallet wallet)\n{\n    var d = wallet.total.$\n}\n");
    PURR_CHECK(offers(members, "Dollars") && !has(members, "operator"));

    // Methods rename, at their declaration and every call.
    open_document(OPERATORS "system S(Wallet wallet)\n{\n    var d = wallet.total.Dol$lars();\n}\n");
    request_with("textDocument/rename", "\"newName\":\"Whole\"");
    const char *renamed = apply_reply(OPERATORS "system S(Wallet wallet)\n{\n    var d = wallet.total.Dollars();\n}\n",
                                      "file:///test.purr");
    PURR_CHECK(has(renamed, "int Whole() {") && has(renamed, "wallet.total.Whole();"));

    // Snippets for what goes where: functions at the top, methods in structs
    // and components, operators in structs.
    PURR_CHECK(offers(complete("$"), "function"));
    const char *in_struct = complete("struct Money\n{\n    int cents;\n    $\n}\nscene Main { }\n");
    PURR_CHECK(offers(in_struct, "method") && offers(in_struct, "mut method") && offers(in_struct, "operator"));
    PURR_CHECK(has(in_struct, "Money operator ${1:+}(Money a, Money b)"));
    const char *in_component = complete("component Unit\n{\n    int kills;\n    $\n}\nscene Main { }\n");
    PURR_CHECK(offers(in_component, "method") && !offers(in_component, "operator"));
    PURR_CHECK(offers(complete("struct Money\n{\n    int cents;\n    Money $\n}\nscene Main { }\n"), "operator"));

    // Laid out like C#: `operator +(`, and unary minus stays unary.
    static const char messy[] = "struct Money\n{\nint cents;\nMoney operator+(Money a,Money b) { return a; }\n"
                                "Money operator - (Money a) { return Money { cents = -a.cents }; }\n}\nscene Main { }\n";
    static const char expected[] = "struct Money\n{\n    int cents;\n    Money operator +(Money a, Money b) { return a; }\n"
                                   "    Money operator -(Money a) { return Money { cents = -a.cents }; }\n}\n"
                                   "scene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    PURR_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}

PURR_TEST(lsp_folding)
{
    start();
    open_document("// One\n// Two\n// Three\ncomponent Body\n{\n    float x;\n}\nsystem Main()\n{\n    Spawn(Body);\n}\n");
    const char *folds = request("textDocument/foldingRange");
    PURR_CHECK(has(folds, "{\"startLine\":0,\"endLine\":2,\"kind\":\"comment\"}"));
    PURR_CHECK(has(folds, "{\"startLine\":3,\"endLine\":5}")); // From `component Body`, keeping `}` in sight
    PURR_CHECK(has(folds, "{\"startLine\":7,\"endLine\":9}"));
}

// A code action at a 0-based line.
static const char *actions_at(const int line)
{
    char range[160];
    snprintf(range, sizeof range, "\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":0}}",
             line, line);
    return request_at("file:///test.purr", "textDocument/codeAction", line, 0, range);
}

PURR_TEST(lsp_quick_fixes)
{
    start();
    open_document("struct Stats\n{\n    float health;\n    void Heal(float amount) { health += amount; }\n}\n"
                  "component Unit { Stats stats; Armor armor; }\n"
                  "void Bump(int count) { count += 1; }\n"
                  "event(Spawned) Setup(with Main) { var x = 1; x = 2; Spawn(Unit); Grow(x, 2.5); }\n"
                  "system Move(mut Velocity velocity) { }\n"
                  "scene Main { }\n");
    const char *method = actions_at(3);
    PURR_CHECK(has(method, "Make 'Heal' mut") && has(method, "\"start\":{\"line\":3,\"character\":4}"));
    PURR_CHECK(has(actions_at(5), "Create struct 'Armor'"));
    const char *param = actions_at(6);
    PURR_CHECK(has(param, "Declare 'count' as mut") && has(param, "\"start\":{\"line\":6,\"character\":10}"));
    const char *main = actions_at(7);
    PURR_CHECK(has(main, "Declare 'x' as mut") && has(main, "\"start\":{\"line\":7,\"character\":34}"));
    PURR_CHECK(has(main, "Create function 'Grow'") && has(main, "\"newText\":\"\\nvoid Grow(int x, float value)\\n{\\n}\\n\""));
    PURR_CHECK(has(main, "\"start\":{\"line\":10,\"character\":0}")); // At the end of the file
    PURR_CHECK(has(actions_at(8), "Create component 'Velocity'"));
}

PURR_TEST(lsp_move_to_file)
{
    start();
    open_document("namespace Combat;\n\n// How much damage it takes.\ncomponent Health\n{\n    int value;\n}\n\n"
                  "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Health); }\n");
    const char *move = request_at("file:///test.purr", "textDocument/codeAction", 4, 0,
                                  "\"range\":{\"start\":{\"line\":4,\"character\":0},\"end\":{\"line\":4,\"character\":0}}");
    PURR_CHECK(has(move, "\"title\":\"Move 'Health' to Health.purr\",\"kind\":\"refactor.move\""));
    PURR_CHECK(has(move, "{\"kind\":\"create\",\"uri\":\"file:///Health.purr\""));
    // The new file: the namespace, then the declaration with its comment.
    PURR_CHECK(has(move, "\"newText\":\"namespace Combat;\\n\\n// How much damage it takes.\\ncomponent Health\\n{\\n"
                         "    int value;\\n}\\n\""));
    // Removed here, with the blank line after it.
    PURR_CHECK(has(move, "{\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":8,\"character\":0}},"
                         "\"newText\":\"\"}"));

    // Systems and handlers stay: files decide the order they run in.
    const char *system = request_at("file:///test.purr", "textDocument/codeAction", 9, 0,
                                    "\"range\":{\"start\":{\"line\":9,\"character\":0},\"end\":{\"line\":9,\"character\":0}}");
    PURR_CHECK(!has(system, "Move '"));

    // An editor that can't create files isn't offered it.
    start_with("{}");
    open_document("component Health\n{\n    int value;\n}\nscene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Health); }\n");
    const char *unable = request_at("file:///test.purr", "textDocument/codeAction", 1, 0,
                                    "\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":0}}");
    PURR_CHECK(!has(unable, "Move '"));
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
        "input Keys {\n    bool fire;\n\n    Sample() {\n        fire = Devices.keyboard.space.pressed; }\n}\n"
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
        "input Keys\n{\n    bool fire;\n\n    Sample()\n    {\n        fire = Devices.keyboard.space.pressed;\n    }\n}\n"
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
        "struct Range\n{\n    float lo;\n    float hi = 1;\n\n    float Width() { return hi - lo; }\n"
        "    mut void Scale(float by) { lo *= by; hi *= by; }\n}\n"
        "float Grow(mut Range range, float by)\n{\n    range.Scale(by);\n    return range.Width();\n}\n"
        "[NativeName(\"c_noise\")]\nextern float Noise(float x, mut Range range);\n"
        "input Controls\n{\n    float2 aim;\n\n    Sample()\n    {\n"
        "        var keys = Devices.keyboard;\n        if (keys.w.pressed) aim.y += 1;\n    }\n}\n"
        "system Move(Controls controls, Devices devices, Time time, mut Body body, without Arena)\n{\n"
        "    if (devices.gamepad.buttonSouth.down) body.radius += 1;\n"
        "    mut var speed = Math.Length(body.position.xy) * 2;\n"
        "    if (controls.aim.x > 0 && speed < 10) { body.position += controls.aim * time.dt; }\n"
        "    else body.radius = Math.Clamp(body.radius, 1, 2);\n"
        "    var e = Spawn(Body { position = float2(1, 2) });\n    e.Send(Hit { damage = 2 });\n    e.Destroy();\n}\n"
        "event Hit\n{\n    int damage = 1;\n}\n"
        "event(Hit hit) TakeHit(mut Body body)\n{\n    body.radius -= hit.damage;\n}\n"
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
                                    "event(Spawned) Setup(with Main) { Spawn(Body); }\n"
                                    "[After(Physics.Gravity)]\n"
                                    "system Move(mut Body body) { body.position.x += 1; }\n"
                                    "scene Main { }\n";
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
    PURR_CHECK(has(request_at(uri_b, "textDocument/definition", 1, 40, ""), uri_a));
    PURR_CHECK(count(request_at(uri_b, "textDocument/references", 1, 40, "\"context\":{\"includeDeclaration\":true}"),
                     uri_a) >= 2); // The declaration and Gravity's parameter
    PURR_CHECK(has(last_sent(), uri_b));

    // The hover shows where Move runs.
    PURR_CHECK(has(request_at(uri_b, "textDocument/hover", 3, 8, ""), "Runs 2nd of 2 systems each tick, after `Physics.Gravity`"));

    // Renaming Gravity from the attribute edits both files.
    const char *rename = request_at(uri_b, "textDocument/rename", 2, 17, "\"newName\":\"Fall\"");
    PURR_CHECK(has(rename, uri_a) && has(rename, uri_b));
    PURR_CHECK(count(rename, "\"newText\":\"Fall\"") == 2);

    // Completion after `Physics.` lists what's inside it.
    open_uri(uri_b, "using Physics;\nevent(Spawned) Setup(with Main) { Spawn(Physics.); }\nscene Main { }\n");
    const char *completion = request_at(uri_b, "textDocument/completion", 1, 48, "");
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
                                    "event(Spawned) Setup(with Main) { Spawn(Body); }\n"
                                    "system Move(mut Body body) { body.position.x += 1; }\n"
                                    "scene Main { }\n";
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
                                    "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
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
    static const char main_file[] = "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
    static const char alone[] = "component Body { float2 position; }\nscene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
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
                                  "event(Spawned) Setup(with Main) { Spawn(Body); }\n"
                                  "system Move(mut Body body) { body.position += body.velocity; }\n"
                                  "system Look(mut Body body, mut Score s) { s.total = 0; if (body.position.x > 0) return; }\n"
                                  "system Tag(mut Score s, Body body) { s.total = 1; }\n"
                                  "scene Main { }\n";
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

    open_document("component Body { float2 position; }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n"
         "system Push(Body body) { body.position.x = 1; }\nscene Main { }\n");
    const char *add_mut = request_at("file:///test.purr", "textDocument/codeAction", 2, 0,
                                     "\"range\":{\"start\":{\"line\":2,\"character\":25},\"end\":{\"line\":2,\"character\":29}}");
    PURR_CHECK(has(add_mut, "Declare 'body' as mut") && has(add_mut, "\"newText\":\"mut \""));
    PURR_CHECK(has(add_mut, "\"start\":{\"line\":2,\"character\":12}"));
}
