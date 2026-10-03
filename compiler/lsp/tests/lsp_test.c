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
#include "tide_test.h"
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
               "{\"uri\":\"file:///test.tide\",\"languageId\":\"tide\",\"version\":1,\"text\":");
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
             "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"%s\",\"params\":{\"textDocument\":{\"uri\":\"file:///test.tide\"},"
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

TIDE_TEST(lsp_initialize_advertises_features)
{
    start();
    const char *reply = last_sent();
    TIDE_CHECK(has(reply, "\"id\":1"));
    TIDE_CHECK(has(reply, "\"completionProvider\""));
    TIDE_CHECK(has(reply, "\"hoverProvider\":true"));
    TIDE_CHECK(has(reply, "\"tokenTypes\":[\"namespace\""));
}

TIDE_TEST(lsp_diagnostics)
{
    start();
    open_document(GAME_TYPES);
    TIDE_CHECK(has(last_sent(), "\"method\":\"textDocument/publishDiagnostics\""));
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.pos = 1;\n}\n");
    TIDE_CHECK(has(last_sent(), "Body has no field 'pos'"));
    TIDE_CHECK(has(last_sent(), "\"range\":{\"start\":{\"line\":24,\"character\":9},\"end\":{\"line\":24,\"character\":12}}"));
    TIDE_CHECK(has(last_sent(), "\"severity\":1"));
}

TIDE_TEST(lsp_diagnostics_keep_going_after_syntax_errors)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.position = ;\n    var x = 1 +;\n    body.radius = true;\n}\n");
    const char *reply = last_sent();
    TIDE_CHECK(has(reply, "expected an expression, found ';'"));
    TIDE_CHECK(has(reply, "\"line\":24")); // The second syntax error
    TIDE_CHECK(has(reply, "can't assign bool to float")); // And the checker still ran
}

TIDE_TEST(lsp_complete_fields_after_dot)
{
    start();
    const char *reply = complete(GAME_TYPES "system Move(mut Body body)\n{\n    body.$\n}\n");
    TIDE_CHECK(offers(reply, "position"));
    TIDE_CHECK(offers(reply, "radius"));
    TIDE_CHECK(!offers(reply, "halfSize"));
}

TIDE_TEST(lsp_complete_chains_and_swizzles)
{
    start();
    const char *reply = complete(GAME_TYPES "system Move(mut Body body)\n{\n    var p = body.position.$\n}\n");
    TIDE_CHECK(offers(reply, "x"));
    TIDE_CHECK(offers(reply, "y"));
    TIDE_CHECK(!offers(reply, "z"));
    TIDE_CHECK(offers(reply, "xy") == false); // A float2's only swizzle of all its components is itself
}

TIDE_TEST(lsp_complete_input_edges)
{
    start();
    const char *reply = complete(GAME_TYPES "system Fire(PlayerInput input)\n{\n    if (input.fire.$) return;\n}\n");
    TIDE_CHECK(offers(reply, "down"));
    TIDE_CHECK(offers(reply, "up"));
}

// Inside Sanitize, the input's fields are in scope; this machine's devices aren't.
TIDE_TEST(lsp_sanitize)
{
    start();
    static const char program[] = "input PlayerInput\n{\n    float move;\n\n    Sample() { }\n\n"
                                  "    Sanitize()\n    {\n        move = Math.Clamp($, -1, 1);\n    }\n}\n"
                                  "scene Main { }\n";
    const char *reply = complete(program);
    TIDE_CHECK(offers(reply, "move"));
    TIDE_CHECK(!offers(reply, "Devices"));

    open_document("input PlayerInput\n{\n    float move;\n    Sani$tize() { move = Math.Clamp(move, -1, 1); }\n}\n"
         "scene Main { }\n");
    TIDE_CHECK(has(sent[0], "\"diagnostics\":[]"));
    TIDE_CHECK(has(request("textDocument/hover"), "Runs on every input before the simulation reads it"));

    open_document("input PlayerInput\n{\n    bool jump;\n    Sam$ple() { jump = Devices.keyboard.space.pressed; }\n}\n"
         "scene Main { }\n");
    TIDE_CHECK(has(sent[0], "\"diagnostics\":[]"));
    TIDE_CHECK(has(request("textDocument/hover"), "Builds the player's input from this machine's `Devices`"));
}

static const char *format_reply(const char *text);
static const char *request_at(const char *uri, const char *method, int line, int character, const char *extra);
static const char *actions_at(int line);

// [Clamp], [Min] and [Max] on input fields: completion, and formatting.
TIDE_TEST(lsp_field_attributes)
{
    start();
    const char *names = complete("input PlayerInput\n{\n    [$] float move;\n}\nscene Main { }\n");
    TIDE_CHECK(offers(names, "Clamp") && offers(names, "Min") && offers(names, "Max"));
    TIDE_CHECK(!offers(names, "Before"));
    const char *bounds = complete("input PlayerInput\n{\n    [Clamp(float2(0, 0), $)] float2 aim;\n}\nscene Main { }\n");
    TIDE_CHECK(offers(bounds, "Math") && offers(bounds, "float2"));

    static const char messy[] = "input PlayerInput\n{\n[Clamp(-1,1)]float move;\n    [Min(0),Max(3)]   int gear;\n}\n"
                                "scene Main { }\n";
    static const char expected[] = "input PlayerInput\n{\n    [Clamp(-1, 1)] float move;\n    [Min(0), Max(3)] int gear;\n}\n"
                                   "scene Main { }\n";
    open_document(messy);
    TIDE_CHECK(has(sent[0], "\"diagnostics\":[]"));
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}

TIDE_TEST(lsp_complete_builtin_owners)
{
    start();
    const char *math = complete(GAME_TYPES "system S(mut Body body)\n{\n    body.radius = Math.$\n}\n");
    TIDE_CHECK(offers(math, "Sin"));
    TIDE_CHECK(offers(math, "PI"));
    TIDE_CHECK(offers(math, "Normalize"));

    const char *draw = complete(GAME_TYPES "view V(Body body)\n{\n    Draw.$\n}\n");
    TIDE_CHECK(offers(draw, "Circle"));
    TIDE_CHECK(has(draw, "Circle(${1:center}, ${2:radius}, ${3:color})"));

    const char *color = complete(GAME_TYPES "view V(Body body)\n{\n    Draw.Circle(body.position, 1, Color.$);\n}\n");
    TIDE_CHECK(offers(color, "red"));
}

TIDE_TEST(lsp_complete_draw_only_in_views)
{
    start();
    const char *system = complete(GAME_TYPES "system S(mut Body body)\n{\n    $\n}\n");
    TIDE_CHECK(!offers(system, "Draw"));
    TIDE_CHECK(offers(system, "Spawn"));
    const char *view = complete(GAME_TYPES "view V(Body body)\n{\n    $\n}\n");
    TIDE_CHECK(offers(view, "Draw"));
    TIDE_CHECK(!offers(view, "Body")); // Views spawn local entities, never the match's
}

TIDE_TEST(lsp_complete_names_in_scope)
{
    start();
    const char *reply = complete(GAME_TYPES "system S(Time time, mut Body body)\n{\n    var speed = 2.0;\n"
                                            "    if (true)\n    {\n        var inner = 1;\n    }\n    body.radius = $\n}\n");
    TIDE_CHECK(offers(reply, "speed"));
    TIDE_CHECK(offers(reply, "time"));
    TIDE_CHECK(offers(reply, "body"));
    TIDE_CHECK(!offers(reply, "inner")); // Out of scope
    TIDE_CHECK(offers(reply, "float3"));
    TIDE_CHECK(offers(reply, "Math"));
}

TIDE_TEST(lsp_complete_in_constructor)
{
    start();
    const char *devices = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n"
                                   "        var keys = Devices.$\n    }\n}\nscene Main { }\n");
    TIDE_CHECK(offers(devices, "keyboard"));
    TIDE_CHECK(offers(devices, "gamepad"));

    const char *keys = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n"
                                "        var keys = Devices.keyboard;\n        if (keys.$\n    }\n}\nscene Main { }\n");
    TIDE_CHECK(offers(keys, "space"));
    TIDE_CHECK(offers(keys, "leftShift"));

    // Sample takes local singletons; Devices is a name.
    const char *params = complete("local singleton Menu { bool open; }\ninput PlayerInput\n{\n    float2 move;\n\n"
                                  "    Sample($)\n    {\n    }\n}\nscene Main { }\n");
    TIDE_CHECK(offers(params, "Menu"));
    TIDE_CHECK(!offers(params, "Devices"));
    const char *names = complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n        move = $\n"
                                 "    }\n}\nscene Main { }\n");
    TIDE_CHECK(offers(names, "Devices"));

    // Views read them too, and systems take them.
    const char *view = complete("scene Main { }\nview Pause()\n{\n    if (Devices.keyboard.$\n}\n");
    TIDE_CHECK(offers(view, "escape"));
    // What's typed is a frame's: views and functions read it, not Sample or systems.
    TIDE_CHECK(offers(view, "text"));
    TIDE_CHECK(offers(complete("scene Main { }\nint Typed(Keyboard keys)\n{\n    return keys.$\n}\n"), "text"));
    TIDE_CHECK(!offers(complete("input PlayerInput\n{\n    float2 move;\n\n    Sample()\n    {\n"
                                "        if (Devices.keyboard.$\n    }\n}\nscene Main { }\n"),
                       "text"));
    TIDE_CHECK(!offers(complete("scene Main { }\ncomponent Body { float x; }\n"
                                "system Move(Devices devices, mut Body body)\n{\n    body.x += devices.keyboard.$\n}\n"),
                       "text"));
    TIDE_CHECK(offers(complete("scene Main { }\nview Chat()\n{\n    var n = Devices.keyboard.text.$\n}\n"), "length"));
    open_document("scene Main { }\nlocal singleton Chat { string line; }\n"
                  "view Type(mut Chat chat)\n{\n    chat.line += Devices.keyboard.te$xt;\n}\n");
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    const char *typed = request("textDocument/hover");
    TIDE_CHECK(has(typed, "string text"));
    TIDE_CHECK(has(typed, "What was typed since the last frame"));
    const char *system = complete("scene Main { }\ncomponent Body { float x; }\n"
                                  "system Move(Devices devices, mut Body body)\n{\n    body.x += devices.gamepad.$\n}\n");
    TIDE_CHECK(offers(system, "leftStick"));
    const char *header = complete("scene Main { }\ncomponent Body { float x; }\nsystem Move($)\n{\n}\n");
    TIDE_CHECK(offers(header, "Devices"));
}

TIDE_TEST(lsp_complete_declarations_and_headers)
{
    start();
    const char *top = complete(GAME_TYPES "\n$");
    TIDE_CHECK(offers(top, "component"));
    TIDE_CHECK(offers(top, "view"));

    const char *header = complete(GAME_TYPES "system S($)\n{\n}\n");
    TIDE_CHECK(offers(header, "mut"));
    TIDE_CHECK(offers(header, "without"));
    TIDE_CHECK(offers(header, "Body"));
    TIDE_CHECK(offers(header, "Arena"));
    TIDE_CHECK(offers(header, "PlayerInput"));

    const char *filter = complete(GAME_TYPES "system S(with $)\n{\n}\n");
    TIDE_CHECK(offers(filter, "Body"));
    TIDE_CHECK(!offers(filter, "Arena")); // Singletons aren't filters

    const char *name = complete(GAME_TYPES "system S(PlayerInput $)\n{\n}\n");
    TIDE_CHECK(offers(name, "playerInput"));
}

TIDE_TEST(lsp_complete_literal_fields)
{
    start();
    const char *reply = complete(GAME_TYPES "system S()\n{\n    Spawn(Body { $ });\n}\n");
    TIDE_CHECK(offers(reply, "position"));
    TIDE_CHECK(offers(reply, "radius"));
}

// What a function's or method's parameter can be, not an input's Sample's.
TIDE_TEST(lsp_complete_routine_params)
{
    start();
#define TYPES "struct Stats { float health; }\nlocal singleton Menu { bool open; }\n" GAME_TYPES
    const char *function = complete(TYPES "void Heal($)\n{\n}\n");
    TIDE_CHECK(offers(function, "mut") && offers(function, "int") && offers(function, "bool") && offers(function, "string"));
    TIDE_CHECK(offers(function, "List") && offers(function, "Entity") && offers(function, "Stats") && offers(function, "Body"));
    TIDE_CHECK(offers(function, "Action") && offers(function, "Keyboard"));
    TIDE_CHECK(!offers(function, "Menu") && !offers(function, "Session") && !offers(function, "Grid2"));
    const char *after_mut = complete(TYPES "void Heal(mut $)\n{\n}\n");
    TIDE_CHECK(offers(after_mut, "Stats") && offers(after_mut, "float2") && !offers(after_mut, "mut"));
    TIDE_CHECK(!offers(after_mut, "Action"));
    TIDE_CHECK(offers(complete(TYPES "async void Load(Stats s, $)\n{\n}\n"), "Arena")); // A task's singletons
    TIDE_CHECK(offers(complete(TYPES "void Heal(Stats $)\n{\n}\n"), "stats"));
#undef TYPES

    // A method's, in its type's body: no default values there.
    const char *method = complete("struct Stats\n{\n    float health;\n    bool Near($) { return true; }\n}\nscene Main { }\n");
    TIDE_CHECK(offers(method, "mut") && offers(method, "bool") && offers(method, "string") && offers(method, "Entity"));
    TIDE_CHECK(!offers(method, "Math") && !offers(method, "Action"));
    TIDE_CHECK(offers(complete("struct Stats\n{\n    float health;\n    mut void Set(mut $) { }\n}\nscene Main { }\n"), "float"));
    // The input's Sample still takes local singletons, and Sanitize nothing.
    TIDE_CHECK(offers(complete("local singleton Menu { bool open; }\ninput I\n{\n    bool fire;\n    Sample($) { }\n}\n"
                               "scene Main { }\n"),
                      "Menu"));
    TIDE_CHECK(!offers(complete("local singleton Menu { bool open; }\ninput I\n{\n    bool fire;\n    Sanitize($) { }\n}\n"
                                "scene Main { }\n"),
                       "Menu"));
}

// Locals in bodies without braces: an else if's, a loop's, and its variable.
TIDE_TEST(lsp_complete_locals_in_braceless_bodies)
{
    start();
    TIDE_CHECK(offers(complete("scene Main { }\nsystem S()\n{\n    var a = 1;\n    if (a > 1) { }\n"
                               "    else if (a > 0)\n    {\n        var inner = 2;\n        var x = $\n    }\n}\n"),
                      "inner"));
#define PARTS "struct Part { float size; }\ncomponent Kit { List<Part> parts; }\nscene Main { }\nvoid Use(float size) { }\n"
    // A whole statement, and one being typed, which doesn't parse yet
    TIDE_CHECK(offers(complete(PARTS "system S(Kit kit)\n{\n    foreach (var p in kit.parts) Use(p.$size);\n}\n"), "size"));
    TIDE_CHECK(offers(complete(PARTS "system S(Kit kit)\n{\n    foreach (var p in kit.parts) Use(p.$)\n}\n"), "size"));
    TIDE_CHECK(offers(complete(PARTS "system S(Kit kit)\n{\n    foreach (var p in kit.parts) Use($);\n}\n"), "p"));
    TIDE_CHECK(offers(complete(PARTS "system S(Kit kit)\n{\n    for (var i = 0; i < 3; i++) Use($);\n}\n"), "i"));
    TIDE_CHECK(offers(complete("singleton F { Grid2<int> cells; }\nscene Main { }\nvoid Use(int2 p) { }\n"
                               "system S(mut F f)\n{\n    parallel (var at in f.cells) Use($);\n}\n"),
                      "at"));
    // Not after the body ends
    TIDE_CHECK(!offers(complete(PARTS "system S(Kit kit)\n{\n    foreach (var p in kit.parts) Use(1);\n    var x = $\n}\n"), "p"));
#undef PARTS
}

// The types a local can be, where a statement starts.
TIDE_TEST(lsp_complete_statement_types)
{
    start();
    const char *system = complete("scene Main { }\nsystem S()\n{\n    $\n}\n");
    TIDE_CHECK(offers(system, "bool") && offers(system, "string") && offers(system, "Entity") && offers(system, "List"));
    TIDE_CHECK(!offers(system, "Grid2") && !offers(system, "LocalEntity"));
    TIDE_CHECK(offers(complete("scene Main { }\nview V()\n{\n    $\n}\n"), "LocalEntity"));
    // In a value: what makes one
    const char *value = complete("scene Main { }\nsystem S()\n{\n    var x = $\n}\n");
    TIDE_CHECK(offers(value, "float3") && !offers(value, "bool") && !offers(value, "List"));
    // After mut: var, or a type
    const char *after_mut = complete("scene Main { }\nsystem S()\n{\n    mut $\n}\n");
    TIDE_CHECK(offers(after_mut, "var") && offers(after_mut, "int") && offers(after_mut, "string"));
}

// Completion in the rest of the places that had none.
TIDE_TEST(lsp_complete_more_places)
{
    start();
    const char *constant = complete("struct Stats { int armor; }\nenum Page { Title }\nconst $\nscene Main { }\n");
    TIDE_CHECK(offers(constant, "int") && offers(constant, "string") && offers(constant, "Stats") && offers(constant, "Page"));
    TIDE_CHECK(!offers(constant, "Entity") && !offers(constant, "List"));
    const char *error = complete("enum ParseError { Empty }\nint Parse(string t) fails $\nscene Main { }\n");
    TIDE_CHECK(offers(error, "ParseError"));
    TIDE_CHECK(offers(complete("enum E { A }\nstruct S\n{\n    int x;\n    int Get() fails $\n}\nscene Main { }\n"), "E"));

    // An enum's body: names, and after '=', an int constant
    const char *members = complete("const int FIRST = 3;\nconst float HALF = 0.5;\nenum Page\n{\n    Title,\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(!offers(members, "float3") && !offers(members, "Math") && !offers(members, "FIRST"));
    const char *value = complete("const int FIRST = 3;\nconst float HALF = 0.5;\nenum Page : byte\n{\n    Title = $\n}\nscene Main { }\n");
    TIDE_CHECK(offers(value, "FIRST") && !offers(value, "HALF") && !offers(value, "float3"));

    // A loop's keywords
    TIDE_CHECK(offers(complete("scene Main { }\nsystem S()\n{\n    foreach (var x $\n}\n"), "in"));
    TIDE_CHECK(offers(complete("scene Main { }\nsystem S()\n{\n    foreach (var x i$\n}\n"), "in"));
#define FIELD "singleton F { Grid2<int> cells; }\nscene Main { }\n"
    const char *parallel = complete(FIELD "system S(mut F f)\n{\n    parallel (var at in f.cells $\n}\n");
    TIDE_CHECK(offers(parallel, "by") && offers(parallel, "offset"));
    const char *offset = complete(FIELD "system S(mut F f)\n{\n    parallel (var at in f.cells by 2 $\n}\n");
    TIDE_CHECK(!offers(offset, "by") && offers(offset, "offset"));
    TIDE_CHECK(!offers(complete(FIELD "system S(mut F f)\n{\n    parallel (var at in f.cells by $\n}\n"), "offset"));
#undef FIELD

    // A grid field's size
    TIDE_CHECK(offers(complete("singleton F { Grid2<int> cells = $ }\nscene Main { }\n"), "Grid2"));
    TIDE_CHECK(!offers(complete("singleton F { int cells = $ }\nscene Main { }\n"), "Grid2"));
    TIDE_CHECK(!offers(complete("struct S\n{\n    $\n}\nscene Main { }\n"), "Grid2")); // Structs can't hold one
}

TIDE_TEST(lsp_hover)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body$.radius = 1;\n}\n");
    const char *param = request("textDocument/hover");
    TIDE_CHECK(has(param, "mut Body body"));

    open_document(GAME_TYPES "system Move(mut B$ody body)\n{\n}\n");
    const char *type = request("textDocument/hover");
    TIDE_CHECK(has(type, "component Body"));
    TIDE_CHECK(has(type, "float radius = 10;"));

    open_document(GAME_TYPES "view V(Body body)\n{\n    Draw.Cir$cle(body.position, 1, Color.red);\n}\n");
    const char *function = request("textDocument/hover");
    TIDE_CHECK(has(function, "Draw.Circle(float2 center, float radius, Color color)"));
    TIDE_CHECK(has(function, "A filled circle."));
}

TIDE_TEST(lsp_definition)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    bo$dy.radius = 1;\n}\n");
    const char *param = request("textDocument/definition");
    TIDE_CHECK(has(param, "\"range\":{\"start\":{\"line\":22,\"character\":21}"));

    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.rad$ius = 1;\n}\n");
    const char *field = request("textDocument/definition");
    TIDE_CHECK(has(field, "\"range\":{\"start\":{\"line\":3,\"character\":10}"));
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

// Meshes: Draw.Mesh, Draw.Clip and Draw.Screen, the built-in Vertex and
// Filter, and the draw list a view passes to C.
TIDE_TEST(lsp_meshes)
{
    start();
#define MESH_TYPES                                                                 \
    "scene Main { }\n"                                                             \
    "extern void DrawUI(DrawList list);\n"                                         \
    "local singleton Art\n{\n    Grid2<Color> pixels = Grid2(4, 4);\n    List<Vertex> corners;\n}\n"
    open_document(MESH_TYPES "view V(Art art)\n{\n    Draw.Screen();\n    Draw.Clip(Rect(0, 0, 4, 4));\n"
                             "    Draw.Mesh(art.corners, [0, 1, 2], art.pixels, Filter.Point);\n"
                             "    Draw.Mesh([Vertex { position = float2(1, 2) }], [0, 0, 0]);\n"
                             "    Draw.Camera(float3(0, 0, -5), quaternion.identity, 60);\n"
                             "    Draw.Mesh([Vertex3 { position = float3(1, 2, 3) }], [0, 0, 0], float4x4.identity);\n"
                             "    Draw.Clip();\n    DrawUI(Draw.list);\n}\n");
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    const char *draw = complete(MESH_TYPES "view V(Art art)\n{\n    Draw.$\n}\n");
    TIDE_CHECK(offers(draw, "Mesh") && offers(draw, "Clip") && offers(draw, "Screen") && offers(draw, "list"));
    TIDE_CHECK(has(draw, "Mesh(${1:vertices}, ${2:indices})"));
    TIDE_CHECK(has(draw, "Draw.list: DrawList"));
    const char *filter = complete(MESH_TYPES "view V(Art art)\n{\n    Draw.Mesh(art.corners, [0, 1, 2], art.pixels, Filter.$);\n}\n");
    TIDE_CHECK(offers(filter, "Point") && offers(filter, "Bilinear"));
    const char *corner = complete(MESH_TYPES "view V(Art art)\n{\n    var v = Vertex { $ };\n}\n");
    TIDE_CHECK(offers(corner, "position") && offers(corner, "uv") && offers(corner, "color"));
    const char *corner3 = complete(MESH_TYPES "view V(Art art)\n{\n    var v = Vertex3 { $ };\n}\n");
    TIDE_CHECK(offers(corner3, "position") && offers(corner3, "uv") && offers(corner3, "color"));
    const char *param = complete(MESH_TYPES "extern void Paint($);\n");
    TIDE_CHECK(offers(param, "DrawList") && offers(param, "Vertex"));
    const char *tide_param = complete(MESH_TYPES "void Paint($) { }\n"); // Only C takes one
    TIDE_CHECK(!offers(tide_param, "DrawList") && offers(tide_param, "Vertex"));

    open_document(MESH_TYPES "view V(Art art)\n{\n    Draw.Me$sh(art.corners, [0, 1, 2], art.pixels);\n}\n");
    const char *mesh = request("textDocument/hover");
    TIDE_CHECK(has(mesh, "Draw.Mesh(List<Vertex> vertices, List<int> indices, Grid2<Color> texture, Filter filter)"));
    TIDE_CHECK(has(mesh, "Triangles: three of `indices` each"));
    // Each version's doc, 2D and 3D
    open_document(MESH_TYPES "view V(Art art)\n{\n    Draw.Cam$era(float3(0, 0, 0), quaternion.identity, 60);\n}\n");
    const char *camera = request("textDocument/hover");
    TIDE_CHECK(has(camera, "Draw.Camera(float3 position, quaternion rotation, float fieldOfView)"));
    TIDE_CHECK(has(camera, "Sets the 3D camera for the 3D meshes after it") && has(camera, "orthographic size"));
    open_document(MESH_TYPES "view V(Art art)\n{\n    DrawUI(Draw.li$st);\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Draw.list: DrawList"));
    open_document(MESH_TYPES "view V(Art art)\n{\n    Draw.Mesh(art.corners, $\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "Draw.Mesh(List<Vertex> vertices, List<int> indices)"));

    TIDE_CHECK(has(c_definition(MESH_TYPES "view V(Art art)\n{\n    Draw.Me$sh(art.corners, [0, 1, 2]);\n}\n"),
                   "void tide_draw_mesh_lists("));
    TIDE_CHECK(has(c_definition(MESH_TYPES "view V(Art art)\n{\n    Draw.Cl$ip();\n}\n"), "void tide_draw_no_clip("));
    TIDE_CHECK(has(c_definition(MESH_TYPES "view V(Art art)\n{\n    Draw.Scr$een();\n}\n"), "void tide_draw_screen("));
#undef MESH_TYPES
}

// The type of what's at the cursor, and an event's or the input's code.
TIDE_TEST(lsp_type_definition_and_implementation)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    bo$dy.radius = 1;\n}\n");
    TIDE_CHECK(has(request("textDocument/typeDefinition"), "\"range\":{\"start\":{\"line\":0,\"character\":10}"));
    open_document("struct Part { float size; }\ncomponent Kit { List<Part> parts; }\nscene Main { }\n"
                  "system S(Kit kit)\n{\n    var n = kit.pa$rts.count;\n}\n");
    TIDE_CHECK(has(request("textDocument/typeDefinition"), "\"range\":{\"start\":{\"line\":0,\"character\":7}")); // Part
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.rad$ius = 1;\n}\n");
    TIDE_CHECK(has(request("textDocument/typeDefinition"), "\"result\":null")); // A float

    open_document(GAME_TYPES "event Hit\n{\n    int damage = 1;\n}\n\nsystem Strike(with Body)\n{\n"
                  "    this.Send(Hit { damage = 2 });\n}\n\nevent(Hit hit) TakeHit(mut Body body)\n{\n"
                  "    body.radius -= hit.damage;\n}\n\nevent(Spawned) Grow(mut Body body)\n{\n    body.radius += 1;\n}\n");
    const char *handlers = request_at("file:///test.tide", "textDocument/implementation", 22, 7, ""); // event Hit
    TIDE_CHECK(has(handlers, "\"range\":{\"start\":{\"line\":32,\"character\":15}")); // TakeHit
    const char *spawned = request_at("file:///test.tide", "textDocument/implementation", 37, 8, ""); // event(Spawned)
    TIDE_CHECK(has(spawned, "{\"line\":18,\"character\":15}") && has(spawned, "{\"line\":37,\"character\":15}")); // Setup, Grow
    // While the game has an error elsewhere too, as it does while typing.
    open_document(GAME_TYPES "event Hit\n{\n    int damage = 1;\n}\n\nsystem Strike(with Body)\n{\n"
                  "    this.Send(Hit { damage = 2 });\n}\n\nevent(Hit hit) TakeHit(mut Body body)\n{\n"
                  "    body.radius -= hit.damge;\n}\n");
    TIDE_CHECK(has(last_sent(), "\"severity\":1"));
    const char *broken = request_at("file:///test.tide", "textDocument/implementation", 22, 7, "");
    TIDE_CHECK(has(broken, "\"range\":{\"start\":{\"line\":32,\"character\":15}"));
    open_document("input Ke$ys\n{\n    bool fire;\n    Sample() { }\n    Sanitize() { }\n}\nscene Main { }\n");
    const char *input = request("textDocument/implementation");
    TIDE_CHECK(has(input, "{\"start\":{\"line\":3,\"character\":4},\"end\":{\"line\":3,\"character\":10}}"));
    TIDE_CHECK(has(input, "{\"start\":{\"line\":4,\"character\":4},\"end\":{\"line\":4,\"character\":12}}"));
}

#define CALLS                                                                                                     \
    "struct Stats\n{\n    float health = 100;\n    mut void Hurt(float amount) { health -= amount; }\n}\n"    \
    "component Unit { Stats stats; }\n"                                                                           \
    "float Heal(mut Stats stats, float amount)\n{\n    stats.Hurt(-amount);\n    return stats.health;\n}\n"        \
    "scene Main { }\n"                                                                                            \
    "system Fight(mut Unit unit)\n{\n    Heal(unit.stats, 1);\n    Heal(unit.stats, 2);\n    unit.stats.Hurt(1);\n}\n"

// Who calls a function, and what a system calls.
TIDE_TEST(lsp_call_hierarchy)
{
    start();
    open_document(CALLS);
    TIDE_CHECK(!has(last_sent(), "\"severity\":1"));
    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/hover", 0, 0, ""), "\"result\":null")); // Analysed
    const char *item = request_at("file:///test.tide", "textDocument/prepareCallHierarchy", 15, 5, ""); // A call of Heal
    TIDE_CHECK(has(item, "{\"name\":\"Heal\",\"kind\":12,\"detail\":\"float Heal(mut Stats stats, float amount)\""));
    TIDE_CHECK(has(item, "\"selectionRange\":{\"start\":{\"line\":6,\"character\":6},\"end\":{\"line\":6,\"character\":10}}"));

    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"callHierarchy/incomingCalls\",\"params\":{\"item\":{\"name\":\"Heal\","
           "\"kind\":12,\"uri\":\"file:///test.tide\",\"range\":{\"start\":{\"line\":6,\"character\":0},\"end\":"
           "{\"line\":10,\"character\":1}},\"selectionRange\":{\"start\":{\"line\":6,\"character\":6},\"end\":"
           "{\"line\":6,\"character\":10}}}}}");
    TIDE_CHECK(has(last_sent(), "{\"from\":{\"name\":\"Fight\",\"kind\":12"));
    TIDE_CHECK(count(last_sent(), "\"from\":") == 1 && count(last_sent(), "\"character\":4},\"end\":{\"line\":") == 2);

    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"callHierarchy/outgoingCalls\",\"params\":{\"item\":{\"name\":\"Fight\","
           "\"kind\":12,\"uri\":\"file:///test.tide\",\"range\":{\"start\":{\"line\":12,\"character\":0},\"end\":"
           "{\"line\":17,\"character\":1}},\"selectionRange\":{\"start\":{\"line\":12,\"character\":7},\"end\":"
           "{\"line\":12,\"character\":12}}}}}");
    TIDE_CHECK(has(last_sent(), "{\"to\":{\"name\":\"Heal\"") && has(last_sent(), "{\"to\":{\"name\":\"Hurt\",\"kind\":6"));
    TIDE_CHECK(count(last_sent(), "\"to\":") == 2);

    // The input's Sample calls too
    open_document("float Twice(float x) { return x * 2; }\ninput Keys\n{\n    float aim;\n    Sample() { aim = Twice(1); }\n}\n"
                  "scene Main { }\n");
    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/prepareCallHierarchy", 4, 5, ""), "{\"name\":\"Sample\",\"kind\":6"));
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"callHierarchy/incomingCalls\",\"params\":{\"item\":{\"name\":\"Twice\","
           "\"kind\":12,\"uri\":\"file:///test.tide\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":"
           "{\"line\":0,\"character\":38}},\"selectionRange\":{\"start\":{\"line\":0,\"character\":6},\"end\":"
           "{\"line\":0,\"character\":11}}}}}");
    TIDE_CHECK(has(last_sent(), "{\"from\":{\"name\":\"Sample\",\"kind\":6,\"detail\":\"input Keys\""));
}

// Built-ins lead to their C definitions in the engine headers.
TIDE_TEST(lsp_definition_in_c)
{
    start();
#define IN_SYSTEM(code) GAME_TYPES "system S(mut Body body, Time time)\n{\n    " code "\n}\n"
#define IN_VIEW(code) GAME_TYPES "view V(Body body)\n{\n    " code "\n}\n"
    TIDE_CHECK(has(c_definition(IN_VIEW("Draw.Cir$cle(body.position, 1, Color.red);")), "void tide_draw_circle("));
    TIDE_CHECK(has(c_definition(IN_VIEW("Draw.Circle(body.position, 1, Color.r$ed);")), "#define TIDE_COLOR_RED"));
    TIDE_CHECK(has(c_definition(IN_VIEW("Dr$aw.Clear(Color.red);")), "#pragma once"));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("body.radius = Math.S$in(body.radius);")), "tide_sin_f("));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("body.position = Math.S$in(body.position);")), "TIDE_MAP1(float, f, sin, f)"));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("body.position = Math.Nor$malize(body.position);")), "tide_normalize_f##N("));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("body.radius = Math.D$ot(float3(1), float3(2));")), "tide_dot_f3("));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("body.radius = Math.P$I;")), "#define TIDE_PI_F"));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("var q = quaternion.ident$ity;")), "tide_identity_q(void)"));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("flo$at3 v = float3(1);")), "typedef struct tide_float3"));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("body.radius = body.position.x$;")), "typedef struct tide_float2 { float x"));
    TIDE_CHECK(has(c_definition(IN_SYSTEM("var c = Color.red.g$;")), "float r, g, b, a;"));
    TIDE_CHECK(strcmp(c_definition(GAME_TYPES "system S(Ti$me time) { }\n"), "") == 0); // Generated: no C definition

    static const char constructor[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                                      "        var keys = Devices.key$board;\n        fire = keys.space.pressed;\n    }\n}\n"
                                      "scene Main { }\n";
    TIDE_CHECK(has(c_definition(constructor), "tide_keyboard keyboard;"));
    static const char key[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                              "        var keys = Devices.keyboard;\n        fire = keys.spa$ce.pressed;\n    }\n}\n"
                              "scene Main { }\n";
    TIDE_CHECK(has(c_definition(key), "X(space)"));
    static const char button[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                                 "        fire = Devices.mouse.left.pre$ssed;\n    }\n}\nscene Main { }\n";
    TIDE_CHECK(has(c_definition(button), "bool pressed;"));
    static const char devices[] = "input PlayerInput\n{\n    bool fire;\n\n    Sample()\n    {\n"
                                  "        fire = Devi$ces.mouse.left.pressed;\n    }\n}\nscene Main { }\n";
    TIDE_CHECK(has(c_definition(devices), "typedef struct tide_devices"));
#undef IN_SYSTEM
#undef IN_VIEW
}

TIDE_TEST(lsp_symbols)
{
    start();
    open_document(GAME_TYPES "view DrawBodies(Body body) { }\n");
    const char *reply = request("textDocument/documentSymbol");
    TIDE_CHECK(has(reply, "\"name\":\"Body\""));
    TIDE_CHECK(has(reply, "\"name\":\"radius\""));
    TIDE_CHECK(has(reply, "\"name\":\"DrawBodies\",\"detail\":\"view\""));
}

TIDE_TEST(lsp_semantic_tokens)
{
    start();
    open_document(GAME_TYPES);
    const char *reply = request("textDocument/semanticTokens/full");
    // `Body` on line 0, column 10: a struct (2), declared (1).
    TIDE_CHECK(has(reply, "\"data\":[0,10,4,2,1,"));

    // Columns count UTF-16 units: `é` is two bytes but one unit.
    open_document("/* \xC3\xA9 */ component Body { float x; }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,18,4,2,1,"));

    // Built-in value types are keywords (11), like C#'s float: `float3` 7 columns after `Body`.
    open_document("component Body { float3 p; }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,10,4,2,1,0,7,6,11,0,"));
    // Sample and Sanitize are keywords too, and attributes decorators (12).
    open_document("input PlayerInput\n{\n    [Clamp(-1, 1)] float move;\n    Sample() { }\n}\nscene Main { }\n");
    const char *input = request("textDocument/semanticTokens/full");
    TIDE_CHECK(has(input, "2,5,5,12,0,")); // Clamp: line +2, column 5
    TIDE_CHECK(has(input, "1,4,6,11,0,")); // Sample: line +1, column 4
}

#define EVENTS                                                                 \
    GAME_TYPES                                                                 \
    "event Hit\n"                                                              \
    "{\n"                                                                      \
    "    int damage = 1;\n"                                                    \
    "}\n"                                                                      \
    "\n"                                                                       \
    "system Strike(with Body)\n"                                              \
    "{\n"                                                                      \
    "    this.Send(Hit { damage = 2 });\n"                                     \
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

TIDE_TEST(lsp_events)
{
    start();
    open_document(EVENTS);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // The handler: its trigger, and when it runs.
    open_document(GAME_TYPES "event Hit { int damage; }\nsystem S(with Body) { this.Send(Hit); }\n"
                  "event(Hit hit) Take$Hit(mut Body body) { body.radius -= hit.damage; }\n");
    const char *handler = request("textDocument/hover");
    TIDE_CHECK(has(handler, "event(Hit hit) TakeHit(mut Body body)"));
    TIDE_CHECK(has(handler, "Runs when a `Hit` is sent to an entity that matches its parameters"));

    open_document(GAME_TYPES "event(Spawn$ed) Grow(mut Body body) { body.radius += 1; }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Sent to each entity as it's spawned"));

    open_document(GAME_TYPES "event Hit { int damage; }\nsystem S(with Body) { this.Send(Hit); }\n"
                  "event(Hit hit) TakeHit(mut Body body) { body.radius -= h$it.damage; }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "The event being handled"));

    // Completion: declarations, the trigger, a handler's parameters, an event's fields, and Send.
    const char *top = complete(GAME_TYPES "\n$");
    TIDE_CHECK(offers(top, "event"));
    TIDE_CHECK(offers(top, "event handler"));
    const char *trigger = complete(GAME_TYPES "event Hit { int damage; }\nevent($)\n");
    TIDE_CHECK(offers(trigger, "Hit"));
    TIDE_CHECK(offers(trigger, "Spawned"));
    TIDE_CHECK(!offers(trigger, "Devices"));
    const char *params = complete(GAME_TYPES "event Hit { int damage; }\nevent(Hit hit) TakeHit($)\n{\n}\n");
    TIDE_CHECK(offers(params, "mut"));
    TIDE_CHECK(offers(params, "Body"));
    const char *fields = complete(GAME_TYPES "event Hit { int damage; }\nevent(Hit hit) TakeHit(mut Body body)\n{\n"
                                  "    body.radius -= hit.$\n}\n");
    TIDE_CHECK(offers(fields, "damage"));
    const char *send = complete(GAME_TYPES "event Hit { int damage; }\nsystem S(with Body)\n{\n    this.$\n}\n");
    TIDE_CHECK(offers(send, "Send"));
    const char *literal = complete(GAME_TYPES "event Hit { int damage; }\nsystem S(with Body)\n{\n    this.Send(Hit { $ });\n}\n");
    TIDE_CHECK(offers(literal, "damage"));

    // The outline: events are events (24), and handlers say what they are.
    open_document(EVENTS);
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "\"name\":\"Hit\",\"detail\":\"event\",\"kind\":24"));
    TIDE_CHECK(has(symbols, "\"name\":\"TakeHit\",\"detail\":\"event handler\""));
}

TIDE_TEST(lsp_local_state)
{
    start();
    static const char game[] = GAME_TYPES
        "local component Spark { int framesLeft = 2; }\n"
        "local singleton Menu { bool open; }\n"
        "view Trail(with Body, mut Menu menu) { Spawn(Spark); menu.open = true; }\n"
        "view Fade(mut Spark spark) { spark.framesLeft -= 1; if (spark.framesLeft < 0) this.Destroy(); }\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document(GAME_TYPES "local component Sp$ark { int framesLeft = 2; }\nview Fade(mut Spark spark) { spark.framesLeft -= 1; }\n");
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "local component Spark"));
    TIDE_CHECK(has(hover, "Local: this machine's own"));

    // In a view, Spawn makes local entities: the local components are offered, not the match's.
    const char *spawn = complete(GAME_TYPES "local component Spark { int framesLeft = 2; }\nview Trail(Body body)\n{\n    $\n}\n");
    TIDE_CHECK(offers(spawn, "Spawn"));
    TIDE_CHECK(offers(spawn, "Spark"));
    TIDE_CHECK(!offers(spawn, "Body"));
    const char *entity = complete(GAME_TYPES "local component Spark { int framesLeft = 2; }\n"
                                  "view Fade(Spark spark)\n{\n    this.$\n}\n");
    TIDE_CHECK(offers(entity, "Destroy"));
    TIDE_CHECK(offers(complete(GAME_TYPES "\n$"), "local component"));

    open_document(game);
    TIDE_CHECK(has(request("textDocument/documentSymbol"), "\"name\":\"Spark\",\"detail\":\"local component\""));
}


TIDE_TEST(lsp_enums_and_switch)
{
    start();
    static const char game[] = "enum Phase { Warmup, Playing = 5 }\nsingleton Match { Phase phase; int n; }\nscene Main { }\n"
                               "system S(mut Match match)\n{\n    switch (match.phase)\n    {\n        case Phase.Warmup:\n"
                               "            match.n = 1;\n            break;\n        default:\n            break;\n    }\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document("enum Pha$se { Warmup, Playing = 5 }\nscene Main { }\n");
    const char *type = request("textDocument/hover");
    TIDE_CHECK(has(type, "enum Phase"));
    TIDE_CHECK(has(type, "Playing = 5,"));

    open_document("enum Phase { Warmup, Playing = 5 }\nsingleton Match { Phase phase; }\nscene Main { }\n"
                  "system S(mut Match match) { match.phase = Phase.Play$ing; }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Phase.Playing = 5"));

    const char *members = complete("enum Phase { Warmup, Playing = 5 }\nsingleton Match { Phase phase; }\nscene Main { }\n"
                                   "system S(mut Match match)\n{\n    match.phase = Phase.$\n}\n");
    TIDE_CHECK(offers(members, "Warmup"));
    TIDE_CHECK(offers(members, "Playing"));

    open_document(game);
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "\"name\":\"Phase\",\"detail\":\"enum\",\"kind\":10"));
    TIDE_CHECK(has(symbols, "\"name\":\"Playing\",\"detail\":\"5\",\"kind\":22"));
}

TIDE_TEST(lsp_constants)
{
    start();
    static const char game[] = "const int MAX = 100;\nconst float2 ORIGIN = float2(1, 2);\nstruct Stats { int armor; }\n"
                               "const Stats START = Stats { armor = MAX / 2 };\nsingleton Match { int n = MAX; float x; }\n"
                               "scene Main { }\nsystem S(mut Match match)\n{\n    match.n += MAX;\n    match.x = ORIGIN.y;\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // Laid out as the formatter would

    open_document("const int MAX = 100;\nsingleton Match { int n; }\nscene Main { }\n"
                  "system S(mut Match match) { match.n += M$AX; }\n");
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "const int MAX = 100"));
    TIDE_CHECK(has(hover, "Constant: the same on every machine"));
    TIDE_CHECK(has(request("textDocument/definition"), "\"range\":{\"start\":{\"line\":0,\"character\":10}"));
    TIDE_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":true}"), "\"uri\"") == 2);

    // A value with braces in it shows whole.
    open_document("struct Stats { int armor; }\nconst Stats ST$ART = Stats { armor = 2 };\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "const Stats START = Stats { armor = 2 }"));

    const char *in_code = complete("const int MAX = 100;\nsingleton Match { int n; }\nscene Main { }\n"
                                   "system S(mut Match match) { match.n = $ }\n");
    TIDE_CHECK(offers(in_code, "MAX"));
    TIDE_CHECK(offers(complete("const int MAX = 100;\nsingleton Match { int n = $ }\nscene Main { }\n"), "MAX"));
    TIDE_CHECK(offers(complete("const float2 ORIGIN = float2(1, 2);\nsingleton Match { float x; }\nscene Main { }\n"
                               "system S(mut Match match) { match.x = ORIGIN.$ }\n"),
                      "y"));
    TIDE_CHECK(offers(complete("$\n"), "const"));

    open_document(game);
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "\"name\":\"MAX\",\"detail\":\"const\",\"kind\":14"));

    // Renaming one renames every use.
    static const char program[] = "const int MAX = 100;\nsingleton Match { int n = MAX; }\nscene Main { }\n"
                                  "system S(mut Match match) { match.n += MAX; }\n";
    open_document("const int M$AX = 100;\nsingleton Match { int n = MAX; }\nscene Main { }\n"
                  "system S(mut Match match) { match.n += MAX; }\n");
    TIDE_CHECK(has(request("textDocument/prepareRename"), "\"result\":{\"start\""));
    request_with("textDocument/rename", "\"newName\":\"LIMIT\"");
    const char *renamed = apply_reply(program, "file:///test.tide");
    TIDE_CHECK(has(renamed, "const int LIMIT = 100;"));
    TIDE_CHECK(has(renamed, "int n = LIMIT;"));
    TIDE_CHECK(has(renamed, "match.n += LIMIT;"));
    TIDE_CHECK(!has(renamed, "MAX"));
}

TIDE_TEST(lsp_settings)
{
    start();
    static const char game[] = "const int RATE = 30;\n\nsettings\n{\n    title = \"Asteroids\";\n    tickRate = RATE;\n}\n\n"
                               "scene Main { }\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // Laid out as the formatter would

    open_document("settings\n{\n    tick$Rate = 30;\n}\nscene Main { }\n");
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "int tickRate"));
    TIDE_CHECK(has(hover, "Without it: 60."));
    TIDE_CHECK(has(request("textDocument/prepareRename"), "Settings are the engine's"));
    open_document("settings\n{\n    hostMig$ration = true;\n}\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "bool hostMigration"));
    open_document("settings\n{\n    vers$ion = \"1.2.0\";\n}\nscene Main { }\n");
    hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "string version"));
    TIDE_CHECK(has(hover, "Without it: 1.0."));

    const char *empty = complete("settings\n{\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(offers(empty, "tickRate"));
    TIDE_CHECK(offers(empty, "title"));
    TIDE_CHECK(offers(empty, "appId"));
    TIDE_CHECK(offers(empty, "version"));
    // A version phones won't take is an error the editor shows
    open_document("settings { version = \"v2\"; }\nscene Main { }\n");
    TIDE_CHECK(has(last_sent(), "isn't a version that Android and iOS both take"));
    TIDE_CHECK(!offers(empty, "float3")); // Names, not values
    const char *rest = complete("settings\n{\n    tickRate = 30;\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(!offers(rest, "tickRate")); // Set already
    TIDE_CHECK(offers(rest, "title"));
    TIDE_CHECK(offers(complete("const int RATE = 30;\nsettings\n{\n    tickRate = $\n}\nscene Main { }\n"), "RATE"));
    TIDE_CHECK(offers(complete("scene Main { }\n$\n"), "settings"));

    open_document(game);
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "\"name\":\"settings\",\"detail\":\"the engine's\",\"kind\":19"));
    TIDE_CHECK(has(symbols, "\"name\":\"tickRate\",\"kind\":7"));

    // A name that isn't a setting says which one was meant.
    open_document("settings { tickRat = 30; }\nscene Main { }\n");
    TIDE_CHECK(has(last_sent(), "did you mean 'tickRate'?"));
}

TIDE_TEST(lsp_default_value)
{
    start();
    open_document("scene Main { }\nview Render(with Main)\n{\n    Draw.Circle(def$ault, 5, Color.yellow);\n}\n");
    TIDE_CHECK(has(sent[0], "\"diagnostics\":[]"));
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "default: float2"));
    TIDE_CHECK(has(hover, "All zeros: the default value of `float2`"));

    open_document("struct Stats { int armor = 3; }\nsingleton S { Stats stats; }\nscene Main { }\n"
                  "system Reset(mut S s) { s.stats = defa$ult; }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "`Stats { }`: each field's default"));

    TIDE_CHECK(offers(complete("singleton S { int n; }\nscene Main { }\nsystem Reset(mut S s) { s.n = $ }\n"), "default"));

    // `default` in ?: isn't a switch's label.
    static const char tidy[] = "singleton S { int n; bool on; }\nscene Main { }\n"
                               "system Pick(mut S s)\n{\n    s.n = s.on ? default : 2;\n    s.n = s.on ? 1 : default;\n}\n";
    TIDE_CHECK(has(format_reply(tidy), "\"result\":[]"));
}

TIDE_TEST(lsp_format_switch)
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
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
    TIDE_CHECK(has(format_reply(expected), "\"result\":[]"));
}


TIDE_TEST(lsp_gui)
{
    start();
    static const char game[] =
        "local singleton Menu { bool open; float volume; }\nscene Main { }\n\n"
        "void Section(string title, mut bool open, Action content)\n{\n    GUILayout.Toggle(title, open);\n"
        "    if (open) content();\n}\n\n"
        "view Options(mut Menu menu)\n{\n    GUILayout.Area(Anchor.MiddleCenter)\n    {\n"
        "        Section(\"Audio\", menu.open)\n        {\n            GUILayout.Slider(\"Volume\", menu.volume, 0, 1);\n"
        "        }\n    }\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // Actions after calls keep their braces on lines of their own

    const char *widgets = complete("scene Main { }\nview V()\n{\n    GUILayout.$\n}\n");
    TIDE_CHECK(offers(widgets, "Button"));
    TIDE_CHECK(offers(widgets, "Horizontal"));
    TIDE_CHECK(offers(widgets, "Area"));
    TIDE_CHECK(offers(complete("scene Main { }\nview V()\n{\n    $\n}\n"), "GUILayout"));
    TIDE_CHECK(!offers(complete("scene Main { }\nsystem S()\n{\n    $\n}\n"), "GUILayout"));
    TIDE_CHECK(offers(complete("scene Main { }\nvoid F()\n{\n    $\n}\n"), "Screen"));

    // Claims, for widgets a view draws itself
    const char *claims = complete("scene Main { }\nview V()\n{\n    GUI.$\n}\n");
    TIDE_CHECK(offers(claims, "ClaimPointer"));
    TIDE_CHECK(offers(claims, "ClaimKeyboard"));
    TIDE_CHECK(offers(claims, "ShowKeyboard"));
    open_document("scene Main { }\nview V()\n{\n    GUI.Claim$Pointer();\n    GUI.ClaimKeyboard();\n    GUI.ShowKeyboard();\n}\n");
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    const char *claim = request("textDocument/hover");
    TIDE_CHECK(has(claim, "GUI.ClaimPointer()"));
    TIDE_CHECK(has(claim, "hidden from the input's `Sample` and from the other views"));
    // At a rect: where a widget of the view's own is
    open_document("scene Main { }\nview V()\n{\n    GUI.Claim$Pointer(Rect(0, 0, 200, 200));\n}\n");
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(request("textDocument/hover"), "GUI.ClaimPointer(Rect rect)"));

    open_document("local singleton M { bool on; }\nscene Main { }\nview V(mut M m) { GUILayout.Tog$gle(\"On\", m.on); }\n");
    const char *toggle = request("textDocument/hover");
    TIDE_CHECK(has(toggle, "GUILayout.Toggle(string text, mut bool value) -> bool"));
    TIDE_CHECK(has(toggle, "Returns whether it changed it"));

    open_document("scene Main { }\nvoid Twice(Action content) { con$tent(); content(); }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Action content"));

    open_document("scene Main { }\nview V() { var w = Screen.wid$th; }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Screen.width: float"));

    // What an Action's caller writes is the caller's code: its names are the caller's.
    open_document("local singleton M { bool on; }\nscene Main { }\nvoid Twice(Action content) { content(); content(); }\n"
                  "view V(mut M m)\n{\n    Twice()\n    {\n        m.o$n = true;\n    }\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "bool on"));
}

TIDE_TEST(lsp_format_blocks)
{
    start();
    static const char messy[] = "scene Main { }\nview V()\n{\nGUILayout.Horizontal() {\nGUILayout.Label(\"a\");\n}\n"
                                "GUILayout.Vertical() { GUILayout.Label(\"b\"); }\n}\n";
    static const char expected[] = "scene Main { }\nview V()\n{\n    GUILayout.Horizontal()\n    {\n        GUILayout.Label(\"a\");\n"
                                   "    }\n    GUILayout.Vertical() { GUILayout.Label(\"b\"); }\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


TIDE_TEST(lsp_loops)
{
    start();
    static const char game[] = "component Tally { int n; }\nscene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Tally); }\n\n"
                               "system Count(mut Tally t)\n{\n    for (var i = 0; i < 10; i++)\n    {\n"
                               "        if (i % 2 == 1) continue;\n        t.n += i;\n    }\n"
                               "    mut var left = 3;\n    while (left > 0) { left--; }\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // Already formatted

    // The for's variable is in scope in its body.
    TIDE_CHECK(offers(complete("component Tally { int n; }\nscene Main { }\nsystem S(mut Tally t)\n{\n"
                               "    for (var index = 0; index < 3; index++)\n    {\n        t.n = $\n    }\n}\n"),
                      "index"));
    TIDE_CHECK(offers(complete("scene Main { }\nsystem S()\n{\n    $\n}\n"), "while"));
    open_document("component Tally { int n; }\nscene Main { }\nsystem S(mut Tally t)\n{\n"
                  "    for (var index = 0; index < 3; index++) t.n += ind$ex;\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "int index"));
}

TIDE_TEST(lsp_errors_as_values)
{
    start();
    // Formatted already, with no diagnostics but the warning for an ignored error.
    static const char game[] =
        "enum ParseError { Empty }\n\n"
        "int Parse(string text) fails ParseError\n{\n    if (text == \"\") fail ParseError.Empty;\n    return 1;\n}\n\n"
        "int? Find(int x)\n{\n    if (x > 0) return x;\n    return null;\n}\n\n"
        "int Twice(string text) fails ParseError\n{\n    var n = try Parse(text);\n"
        "    if (Parse(text) is int m && m > 0) return m;\n    return (Find(n) ?? 0) + Parse(text)! - 1;\n}\n\n"
        "scene Main { }\nsystem S()\n{\n    Parse(\"1\");\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "'Parse' can fail, and nothing handles its error here"));
    TIDE_CHECK(has(last_sent(), "\"severity\":2"));
    TIDE_CHECK(count(last_sent(), "\"severity\"") == 1);
    TIDE_CHECK(has(format_reply(game), "\"result\":[]"));

    // The name after `is` is in scope where the test is true.
    TIDE_CHECK(offers(complete("enum E { A }\nint P() fails E { return 1; }\nscene Main { }\n"
                               "system S()\n{\n    if (P() is int score)\n    {\n        var x = $\n    }\n}\n"),
                      "score"));
    TIDE_CHECK(!offers(complete("enum E { A }\nint P() fails E { return 1; }\nscene Main { }\n"
                                "system S()\n{\n    if (P() is int score) { }\n    var x = $\n}\n"),
                       "score"));
    // try and fail in a function that can fail, and only there.
    const char *in_failing = complete("enum E { A }\nint P() fails E\n{\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(offers(in_failing, "fail"));
    TIDE_CHECK(offers(in_failing, "try"));
    TIDE_CHECK(!offers(complete("scene Main { }\nsystem S()\n{\n    $\n}\n"), "fail"));

    // Hovers: what a call can fail with, the name `is` gives, and null.
    open_document("enum E { A }\nint P() fails E { return 1; }\nscene Main { }\nsystem S()\n{\n    var x = P$() ?? 0;\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "int P() fails E"));
    open_document("enum E { A }\nint P() fails E { return 1; }\nscene Main { }\n"
                  "system S()\n{\n    if (P() is int sc$ore) { }\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "int score"));
    open_document("int? F() { return nu$ll; }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "null: int?"));
}

TIDE_TEST(lsp_format_errors_as_values)
{
    start();
    static const char messy[] = "enum E { A }\nint ? Find(int x)\n{\nreturn null;\n}\n"
                                "int P(int ? y) fails E\n{\nint ? z = y;\nvar a = try P(1)+1;\n"
                                "return (z??Find(2) ??0)+P(3) ! -a;\n}\nscene Main { }\n";
    static const char expected[] = "enum E { A }\nint? Find(int x)\n{\n    return null;\n}\n"
                                   "int P(int? y) fails E\n{\n    int? z = y;\n    var a = try P(1) + 1;\n"
                                   "    return (z ?? Find(2) ?? 0) + P(3)! - a;\n}\nscene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
    TIDE_CHECK(has(format_reply(expected), "\"result\":[]"));
}

TIDE_TEST(lsp_async)
{
    start();
    // Formatted already, with no diagnostics
    static const char game[] =
        "singleton Log { int n; }\n\n"
        "async int Doubled(int x)\n{\n    await Wait.Ticks(1);\n    return x * 2;\n}\n\n"
        "async event(Spawned) Count(mut Log log)\n{\n    log.n = await Doubled(2) + 1;\n}\n\n"
        "scene Main { }\nsystem S(Log log)\n{\n    if (log.n == 0) Doubled(1);\n}\n";
    open_document(game);
    TIDE_CHECK(!has(last_sent(), "\"severity\""));
    if (has(last_sent(), "\"severity\"")) printf("%s\n", last_sent());
    TIDE_CHECK(has(format_reply(game), "\"result\":[]"));
    // A messy one: `async` stays where it is
    static const char messy[] = "async void F( )\n{\nawait Wait.Frames( 2 );\n}\nscene Main { }\n";
    static const char expected[] = "async void F()\n{\n    await Wait.Frames(2);\n}\nscene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);

    // await and Wait in async code, and only there
    const char *in_async = complete("async void F()\n{\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(offers(in_async, "await"));
    TIDE_CHECK(offers(in_async, "Wait"));
    TIDE_CHECK(!offers(complete("scene Main { }\nsystem S()\n{\n    $\n}\n"), "await"));
    const char *waits = complete("async void F()\n{\n    await Wait.$\n}\nscene Main { }\n");
    TIDE_CHECK(offers(waits, "Ticks") && offers(waits, "Frames") && offers(waits, "Seconds"));
    TIDE_CHECK(offers(complete("$"), "async function"));

    // Hovers: an async function's signature, and what Wait waits for
    open_document("async int D() { await Wait.Ticks(1); return 1; }\nscene Main { }\nsystem S()\n{\n    D$();\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "async int D()"));
    open_document("async void F()\n{\n    await Wait.Sec$onds(1);\n}\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "await Wait.Seconds(float seconds)"));
}

TIDE_TEST(lsp_format_loops)
{
    start();
    static const char messy[] = "scene Main { }\nsystem S()\n{\nmut var n = 0;\nfor(var i=0;i<3;i ++){\nn+=i;\n}\n"
                                "while (n > 0) {\nn --;\nif (n == 1) break;\n}\n}\n";
    static const char expected[] = "scene Main { }\nsystem S()\n{\n    mut var n = 0;\n    for (var i = 0; i < 3; i++)\n    {\n"
                                   "        n += i;\n    }\n    while (n > 0)\n    {\n        n--;\n        if (n == 1) break;\n"
                                   "    }\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


TIDE_TEST(lsp_text)
{
    start();
    static const char game[] = "local singleton Score { int points; float time; }\nscene Main { }\n\n"
                               "view Hud(Score score)\n{\n    var name = \"cat\";\n"
                               "    GUILayout.Label($$\"{name}: {score.points:D3} in {score.time:F1}s\" + \"!\");\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // Values sit against their braces

    // Completion works in a value, not in the text around it.
    TIDE_CHECK(offers(complete("local singleton Score { int points; }\nscene Main { }\nview V(Score score)\n{\n"
                               "    GUILayout.Label($$\"points: {sco$}\");\n}\n"),
                      "score"));
    TIDE_CHECK(!offers(complete("local singleton Score { int points; }\nscene Main { }\nview V(Score score)\n{\n"
                                "    GUILayout.Label($$\"poi$ {score.points}\");\n}\n"),
                       "score"));
    const char *methods = complete("scene Main { }\nview V()\n{\n    var name = \"cat\";\n    var n = name.$\n}\n");
    TIDE_CHECK(offers(methods, "length"));
    TIDE_CHECK(offers(methods, "Contains"));

    open_document("scene Main { }\nview V() { var n = \"cat\".Cont$ains(\"a\"); }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Whether `value` is in the text."));

    // Text in fields
    TIDE_CHECK(offers(complete("component Name\n{\n    $\n}\nscene Main { }\n"), "string"));
    open_document("component Name { string va$lue = \"cat\"; }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "string value"));
    const char *fields = complete("component Name { string value; }\nscene Main { }\nsystem S(Name name)\n{\n"
                                  "    var n = name.value.$\n}\n");
    TIDE_CHECK(offers(fields, "length"));
}

TIDE_TEST(lsp_format_text)
{
    start();
    static const char messy[] = "scene Main { }\nview V()\n{\nvar a = $$\"x { 1 + 2 :F2} y {3}\";\n}\n";
    static const char expected[] = "scene Main { }\nview V()\n{\n    var a = $\"x {1 + 2:F2} y {3}\";\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


TIDE_TEST(lsp_lists)
{
    start();
    static const char game[] = "component Inventory { List<int> scores = [1, 2]; }\nscene Main { }\n"
                               "event(Spawned) Setup(with Main) { Spawn(Inventory); }\n\n"
                               "system Count(mut Inventory inv)\n{\n    foreach (var score in inv.scores)\n    {\n"
                               "        if (score > 1) inv.scores.Add(score);\n    }\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // List<int> keeps no spaces

    const char *members = complete("component Inventory { List<int> scores; }\nscene Main { }\nsystem S(Inventory inv)\n{\n"
                                   "    var n = inv.scores.$\n}\n");
    TIDE_CHECK(offers(members, "count"));
    TIDE_CHECK(offers(members, "Add"));
    TIDE_CHECK(offers(complete("component Inventory\n{\n    $\n}\nscene Main { }\n"), "List"));

    // The loop's variable is in scope in its body.
    TIDE_CHECK(offers(complete("component Inventory { List<int> scores; }\nscene Main { }\nsystem S(mut Inventory inv)\n{\n"
                               "    foreach (var score in inv.scores)\n    {\n        var x = $\n    }\n}\n"),
                      "score"));
    open_document("component Inventory { List<int> sco$res; }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "List<int> scores"));
}

TIDE_TEST(lsp_grids)
{
    start();
    static const char game[] = "singleton Field { Grid2<int> cells = Grid2(64, 64); }\nscene Main { }\n\n"
                               "system Fall(mut Field field)\n{\n"
                               "    for (var y = 0; y < field.cells.size.y; y++) field.cells[0, y] = 1;\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]")); // Grid2<int> keeps no spaces

    const char *members = complete("singleton Field { Grid2<int> cells; }\nscene Main { }\nsystem S(Field field)\n{\n"
                                   "    var n = field.cells.$\n}\n");
    TIDE_CHECK(offers(members, "size"));
    TIDE_CHECK(offers(members, "Clear"));
    TIDE_CHECK(offers(complete("singleton Field\n{\n    $\n}\nscene Main { }\n"), "Grid2"));
    open_document("singleton Field { Grid2<int> ce$lls; }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Grid2<int> cells"));

    // Byte-sized cells: the enum keeps its backing as written, and hovers show it
    static const char voxels[] = "enum Voxel : byte\n{\n    Air,\n    Stone,\n}\n\nsingleton World { Grid3<Voxel> cells; }\nscene Main { }\n";
    open_document(voxels);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(voxels), "\"result\":[]"));
    open_document("enum Vox$el : byte { Air, Stone }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "enum Voxel : byte"));
    const char *backings = complete("enum Voxel : $\nscene Main { }\n");
    TIDE_CHECK(offers(backings, "byte"));
    TIDE_CHECK(offers(backings, "ushort"));
}

// Parallel loops: offered where they can go, formatted, and their cells' places
// hovered.
TIDE_TEST(lsp_parallel)
{
    start();
    static const char game[] = "singleton Field { Grid2<int> cells = Grid2(64, 64); }\nscene Main { }\n\n"
                               "system Fall(mut Field field, Time time)\n{\n"
                               "    parallel (var at in field.cells by 2 offset time.tick % 2)\n    {\n"
                               "        var below = field.cells[at];\n"
                               "        field.cells[at + int2(0, 1)] = below;\n    }\n"
                               "    foreach (var at in field.cells) field.cells[at] = 0;\n}\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(format_reply(game), "\"result\":[]"));
    static const char messy[] = "singleton Field { Grid2<int> cells; }\nscene Main { }\n"
                                "system S(mut Field field)\n{\n    parallel(var at in field.cells by 2 offset 1){ field.cells[at] = 1; }\n}\n";
    format_reply(messy);
    TIDE_CHECK(has(apply_reply(messy, NULL), "    parallel (var at in field.cells by 2 offset 1) { field.cells[at] = 1; }\n"));

    TIDE_CHECK(offers(complete("singleton Field { Grid2<int> cells; }\nscene Main { }\nsystem S(mut Field field)\n{\n    $\n}\n"),
                      "parallel"));
    TIDE_CHECK(!offers(complete("scene Main { }\nvoid F()\n{\n    $\n}\n"), "parallel")); // Only systems, views and handlers
    open_document("singleton Field { Grid2<int> cells; }\nscene Main { }\nsystem S(mut Field field)\n{\n"
                  "    parallel (var at in field.cells) field.cells[a$t] = 1;\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "int2 at"));
    open_document("singleton Field { Grid2<int> cells; }\nscene Main { }\nsystem S(mut Field field)\n{\n"
                  "    parallel (var at in field.cells) field.cells[at + int2(1, 0)] = 1;\n}\n");
    TIDE_CHECK(has(last_sent(), "a parallel loop's step changes only its own cell"));
}

TIDE_TEST(lsp_format_lists)
{
    start();
    static const char messy[] = "scene Main { }\nint F(List < int > xs)\n{\nmut List<int> ys = [ 1,2 ];\nys.Add(xs [0]);\nreturn ys.count;\n}\n";
    static const char expected[] = "scene Main { }\nint F(List<int> xs)\n{\n    mut List<int> ys = [1, 2];\n    ys.Add(xs[0]);\n"
                                   "    return ys.count;\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}


TIDE_TEST(lsp_scenes)
{
    start();
    static const char game[] = "scene Arena { int size = 20; }\nscene Main { }\n"
                               "event(Spawned) Setup(with Main) { Scene.Load(Arena { size = 30 }); }\n"
                               "system Close(Arena arena) { if (arena.size > 40) Scene.Unload(this); }\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    open_document("scene Are$na { int size = 20; }\nscene Main { }\nevent(Spawned) Setup(with Main) { Scene.Load(Arena); }\n");
    const char *type = request("textDocument/hover");
    TIDE_CHECK(has(type, "scene Arena"));
    TIDE_CHECK(has(type, "int size = 20;"));
    TIDE_CHECK(!has(type, "tide_players")); // The engine's own fields stay hidden

    open_document("scene Arena { }\nscene Main { }\nevent(Spawned) Setup(with Main) { Scene.Lo$ad(Arena); }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Loads a scene and returns its entity"));

    const char *calls = complete("scene Arena { }\nscene Main { }\nevent(Spawned) Setup(with Main)\n{\n    Scene.$\n}\n");
    TIDE_CHECK(offers(calls, "Load"));
    TIDE_CHECK(offers(calls, "Unload"));
    const char *fields = complete("scene Arena { int size; }\nscene Main { }\nsystem S(Arena arena)\n{\n    var x = arena.$\n}\n");
    TIDE_CHECK(offers(fields, "size"));
    TIDE_CHECK(!offers(fields, "tide_visibility"));

    open_document(game);
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "\"name\":\"Arena\",\"detail\":\"scene\""));
    TIDE_CHECK(!has(symbols, "tide_players"));
}

// Snap: on entities and singletons in match code. A type's Interpolate isn't for calling.
TIDE_TEST(lsp_snap)
{
    start();
    const char *entity = complete("component Body { float2 p; }\nscene Main { }\n"
                                  "system Respawn(mut Body body)\n{\n    this.$\n}\n");
    TIDE_CHECK(offers(entity, "Snap"));
    TIDE_CHECK(offers(entity, "Destroy"));
    const char *singleton = complete("singleton Camera { float2 center; }\nscene Main { }\n"
                                     "system Cut(mut Camera camera)\n{\n    camera.$\n}\n");
    TIDE_CHECK(offers(singleton, "Snap"));
    TIDE_CHECK(offers(singleton, "center"));
    const char *methods = complete("struct Angle\n{\n    float degrees;\n    Angle Interpolate(Angle from, Angle to, float t) { return to; }\n"
                                   "    float Radians() { return degrees; }\n}\ncomponent Body { Angle heading; }\nscene Main { }\n"
                                   "system S(Body body)\n{\n    var r = body.heading.$\n}\n");
    TIDE_CHECK(offers(methods, "Radians"));
    TIDE_CHECK(!offers(methods, "Interpolate"));
}

#define THIS_GAME                                                              \
    "component Health\n"                                                       \
    "{\n"                                                                      \
    "    int value;\n"                                                         \
    "    bool IsMine(Entity e) { return e == th$is; }\n"                       \
    "}\n"                                                                      \
    "local component Spark { int left; }\n"                                    \
    "scene Main { }\n"                                                         \
    "system Die(Health health) { if (health.IsMine(th$is)) this.Destroy(); }\n" \
    "view Fade(mut Spark spark) { spark.left -= 1; if (spark.left <= 0) th$is.Destroy(); }\n"

// this: the entity the code runs for, and whose component a method is called on.
TIDE_TEST(lsp_this)
{
    start();
    open_document(THIS_GAME);
    TIDE_CHECK(!has(last_sent(), "\"severity\":1")); // No errors

    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/hover", 3, 41, ""), "The entity whose `Health` this is."));
    const char *system = request_at("file:///test.tide", "textDocument/hover", 7, 48, "");
    TIDE_CHECK(has(system, "Entity this") && has(system, "The entity `Die` runs for."));
    const char *view = request_at("file:///test.tide", "textDocument/hover", 8, 68, "");
    TIDE_CHECK(has(view, "LocalEntity this"));
    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/prepareRename", 7, 48, ""), "'this' is a keyword"));

    // Offered where the code runs for an entity, and its methods after the dot.
    TIDE_CHECK(offers(complete("component Body { int x; }\nscene Main { }\nsystem S(Body body)\n{\n    $\n}\n"), "this"));
    TIDE_CHECK(!offers(complete("component Body { int x; }\nscene Main { }\nsystem S(Time time)\n{\n    $\n}\n"), "this"));
    TIDE_CHECK(offers(complete("component Body { int x; }\nscene Main { }\nsystem S(with Body)\n{\n    this.$\n}\n"),
                      "Destroy"));

    // The entity isn't a parameter: the quick fix removes it and writes `this`.
    open_document("component Lifetime { int ticks; }\nscene Main { }\n"
                  "system Expire(Entity self, mut Lifetime life)\n{\n    life.ticks -= 1;\n    if (life.ticks <= 0) self.Destroy();\n}\n");
    TIDE_CHECK(has(last_sent(), "the entity a system runs for is 'this', not a parameter"));
    const char *fix = actions_at(2);
    TIDE_CHECK(has(fix, "Use 'this' instead of 'self'"));
    TIDE_CHECK(has(fix, "{\"range\":{\"start\":{\"line\":2,\"character\":14},\"end\":{\"line\":2,\"character\":27}},\"newText\":\"\"}"));
    TIDE_CHECK(has(fix, "{\"range\":{\"start\":{\"line\":5,\"character\":25},\"end\":{\"line\":5,\"character\":29}},\"newText\":\"this\"}"));
}

// Session: its calls from local code, and its singleton and events.
TIDE_TEST(lsp_sessions)
{
    start();
    static const char game[] = "local scene Main { }\nscene Arena { int size = 20; }\n"
                               "view Menu(Session session)\n{\n"
                               "    if (GUILayout.Button(\"Play\") && session.state == SessionState.Offline) Session.Start(Arena);\n"
                               "    if (GUILayout.Button(\"Open\") && !session.open) Session.Open(7777);\n"
                               "    if (GUILayout.Button(\"Join\")) Session.Join(\"K7QF2M\");\n"
                               "    if (GUILayout.Button(\"Connect\")) Session.Connect(\"127.0.0.1\");\n}\n"
                               "local event(Disconnected gone) Lost() { }\n";
    open_document(game);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // The name itself, where a statement starts in local code, even inside a
    // container's block and a branch.
    const char *name = complete("local scene Main { }\nview Menu()\n{\n    GUILayout.Area(Anchor.MiddleCenter)\n    {\n"
                                "        if (GUILayout.Button(\"Host\"))\n        {\n            Sess$\n        }\n    }\n}\n");
    TIDE_CHECK(offers(name, "Session"));
    TIDE_CHECK(offers(complete("local scene Main { }\nlocal event(Disconnected gone) Lost()\n{\n    $\n}\n"), "Session"));
    TIDE_CHECK(!offers(complete("scene Main { }\nsystem Move()\n{\n    $\n}\n"), "Session"));
    TIDE_CHECK(!offers(complete("local scene Main { }\nview Menu()\n{\n    var x = $\n}\n"), "Session"));

    const char *calls = complete("local scene Main { }\nview Menu()\n{\n    Session.$\n}\n");
    TIDE_CHECK(offers(calls, "Start"));
    TIDE_CHECK(offers(calls, "Open"));
    TIDE_CHECK(offers(calls, "Close"));
    TIDE_CHECK(offers(calls, "Kick"));
    TIDE_CHECK(offers(calls, "KickAll"));
    TIDE_CHECK(offers(calls, "End"));
    TIDE_CHECK(offers(calls, "Join"));
    TIDE_CHECK(offers(calls, "Connect"));
    TIDE_CHECK(offers(calls, "Leave"));
    const char *gone = complete("local scene Main { }\nlocal event(Disconnected gone) Lost()\n{\n    var why = gone.$\n}\n");
    TIDE_CHECK(offers(gone, "reason"));
    TIDE_CHECK(offers(gone, "message"));
    const char *fields = complete("local scene Main { }\nview Menu(Session session)\n{\n    var s = session.$\n}\n");
    TIDE_CHECK(offers(fields, "state"));
    TIDE_CHECK(offers(fields, "ping"));
    TIDE_CHECK(offers(fields, "open"));
    TIDE_CHECK(offers(fields, "room"));

    open_document("local scene Main { }\nview Menu()\n{\n    Session.Le$ave();\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Leaves the match"));

    // Clipboard, where Session is: its one call
    TIDE_CHECK(offers(complete("local scene Main { }\nview Menu()\n{\n    $\n}\n"), "Clipboard"));
    TIDE_CHECK(!offers(complete("scene Main { }\nsystem Move()\n{\n    $\n}\n"), "Clipboard"));
    TIDE_CHECK(offers(complete("local scene Main { }\nview Menu()\n{\n    Clipboard.$\n}\n"), "Copy"));
    open_document("local scene Main { }\nview Menu(Session session)\n{\n"
                  "    if (GUILayout.Button(\"Copy\")) Clipboard.Co$py(session.room);\n}\n");
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    TIDE_CHECK(has(request("textDocument/hover"), "on this machine's clipboard"));
}


TIDE_TEST(lsp_format_events)
{
    start();
    static const char messy[] = "event Hit { int damage; }\nsystem Main() { Send(Hit); }\n"
                                "event (Hit hit) TakeHit( ) { }\nevent ( Spawned ) Grow() { }\n";
    static const char expected[] = "event Hit { int damage; }\nsystem Main() { Send(Hit); }\n"
                                   "event(Hit hit) TakeHit() { }\nevent(Spawned) Grow() { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
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

TIDE_TEST(lsp_structs)
{
    start();
    open_document(STRUCTS);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    // `Stats` on line 0, column 7: a struct (2), declared (1).
    TIDE_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,7,5,2,1,"));
    TIDE_CHECK(has(request("textDocument/documentSymbol"), "\"name\":\"Stats\""));

    // As a field type, through members, and in values.
    TIDE_CHECK(offers(complete("struct Stats { float health; }\ncomponent Unit\n{\n    $\n}\n"), "Stats"));
    TIDE_CHECK(offers(complete(STRUCTS "system Hurt(mut Unit unit)\n{\n    unit.stats.$\n}\n"), "health"));
    TIDE_CHECK(offers(complete(STRUCTS "system Hurt(mut Unit unit)\n{\n    unit.stats = Stats { $ };\n}\n"), "health"));

    open_document("struct St$ats\n{\n    float health = 100;\n}\nscene Main { }\n");
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "struct Stats"));
    TIDE_CHECK(has(hover, "float health = 100;"));

    // Laid out like the other declarations.
    static const char messy[] = "struct Stats {\nfloat health = 100;\n}\nscene Main { }\n";
    format_reply(messy);
    TIDE_CHECK(strcmp(apply_reply(messy, NULL), "struct Stats\n{\n    float health = 100;\n}\nscene Main { }\n") == 0);
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

TIDE_TEST(lsp_methods_and_functions)
{
    start();
    open_document(METHODS);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "\"name\":\"IsDead\",\"detail\":\"bool IsDead()\",\"kind\":6"));
    TIDE_CHECK(has(symbols, "\"name\":\"Heal\",\"detail\":\"function\",\"kind\":12"));

    // After a dot, methods with their signatures; in code, functions; in a method, its type's fields.
    const char *members = complete(METHODS "system Fight(mut Unit unit)\n{\n    unit.stats.$\n}\n");
    TIDE_CHECK(offers(members, "IsDead") && offers(members, "Hurt") && offers(members, "health"));
    TIDE_CHECK(has(members, "mut void Hurt(float amount)"));
    TIDE_CHECK(offers(complete(METHODS "system Fight(mut Unit unit)\n{\n    $\n}\n"), "Heal"));
    TIDE_CHECK(offers(complete("struct Stats\n{\n    float health;\n    bool IsDead() { return $ }\n}\nscene Main { }\n"),
                      "health"));

    // Hover, definition and references go to the method itself.
    open_document(METHODS "system Fight(mut Unit unit)\n{\n    unit.stats.Hu$rt(1);\n}\n");
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "mut void Hurt(float amount)"));
    TIDE_CHECK(has(hover, "Method of struct `Stats`"));
    TIDE_CHECK(has(request("textDocument/definition"), "\"range\":{\"start\":{\"line\":5,\"character\":13}"));

    open_document(METHODS "system Fight(mut Unit unit)\n{\n    He$al(unit.stats, 1);\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "float Heal(mut Stats stats, float amount)"));
    open_document(METHODS "system Fight(mut Unit unit)\n{\n    Heal(unit.stats, $\n}\n");
    const char *signature = request("textDocument/signatureHelp");
    TIDE_CHECK(has(signature, "float Heal(mut Stats stats, float amount)"));
    TIDE_CHECK(has(signature, "\"activeParameter\":1"));

    // Inlay hints: what a var is, and parameter names at literal arguments.
    open_document(METHODS "system Fight(mut Unit unit)\n{\n    var left = Heal(unit.stats, 2);\n}\n");
    const char *hints = request_at("file:///test.tide", "textDocument/inlayHint", 0, 0,
                                   "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":30,\"character\":0}}");
    TIDE_CHECK(has(hints, "{\"position\":{\"line\":20,\"character\":12},\"label\":\": float\",\"kind\":1"));
    TIDE_CHECK(has(hints, "{\"position\":{\"line\":20,\"character\":32},\"label\":\"amount:\",\"kind\":2"));
    TIDE_CHECK(!has(hints, "\"stats:\"")); // Not for arguments that already say what they are

    // Workspace symbols: every declaration of the game, methods with their type, by fuzzy name.
    open_document(METHODS);
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/symbol\",\"params\":{\"query\":\"hrt\"}}");
    TIDE_CHECK(has(last_sent(), "\"name\":\"Hurt\",\"kind\":6") && has(last_sent(), "\"containerName\":\"Stats\""));
    TIDE_CHECK(!has(last_sent(), "\"name\":\"Heal\""));
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/symbol\",\"params\":{\"query\":\"\"}}");
    TIDE_CHECK(has(last_sent(), "\"name\":\"Heal\",\"kind\":12") && has(last_sent(), "\"name\":\"Unit\",\"kind\":23"));

    // Laid out like other code: braces on their own lines, one-liners kept.
    static const char messy[] = "struct Stats {\nfloat health;\nbool IsDead() { return health <= 0; }\n"
                                "mut void Hurt() {\nhealth -= 1; }\n}\nvoid Reset(mut Stats s) {\ns.health = 0; }\n"
                                "scene Main { }\n";
    static const char expected[] = "struct Stats\n{\n    float health;\n    bool IsDead() { return health <= 0; }\n"
                                   "    mut void Hurt()\n    {\n        health -= 1;\n    }\n}\n"
                                   "void Reset(mut Stats s)\n{\n    s.health = 0;\n}\nscene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
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

TIDE_TEST(lsp_extern_functions)
{
    start();
    open_document(EXTERNS);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // Hover says it's C's, and which C function.
    open_document(EXTERNS "system Shape(mut Ground ground)\n{\n    ground.height = No$ise(1, 2);\n}\n");
    const char *hover = request("textDocument/hover");
    TIDE_CHECK(has(hover, "extern float Noise(float x, float y)"));
    TIDE_CHECK(has(hover, "[NativeName(\\\"stb_perlin_noise3\\\")]"));
    TIDE_CHECK(has(hover, "C function `stb_perlin_noise3`"));
    TIDE_CHECK(has(request("textDocument/definition"), "\"range\":{\"start\":{\"line\":1,\"character\":13}"));
    open_document(EXTERNS "system Shape(mut Ground ground)\n{\n    ground.height = tw$ice(2);\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "C function `twice`"));
    open_document(EXTERNS "system Shape(mut Ground ground)\n{\n    ground.height = We$igh(float3(1), [1], \"a\");\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "extern float Weigh(in float3 v, List<float> weights, string name)"));

    // Completion: the declaration, its return type and parameters, the attribute, and calls.
    TIDE_CHECK(offers(complete(EXTERNS "$"), "extern"));
    TIDE_CHECK(offers(complete(EXTERNS "extern $"), "float"));
    const char *params = complete(EXTERNS "extern float Sum($");
    TIDE_CHECK(offers(params, "in") && offers(params, "mut") && offers(params, "float3"));
    TIDE_CHECK(offers(complete(EXTERNS "extern float Sum(in $"), "float3"));
    TIDE_CHECK(offers(complete(EXTERNS "[$"), "NativeName"));
    TIDE_CHECK(offers(complete(EXTERNS "system Shape(mut Ground ground)\n{\n    $\n}\n"), "Noise"));

    // Errors come with the code around them.
    open_document("scene Main { }\nextern void Log(List<string> lines);\n");
    TIDE_CHECK(has(last_sent(), "C functions can't take lists of text yet"));

    // The formatter leaves an extern on its line.
    static const char messy[] = "extern  float Noise( float x ,float y ) ;\nscene Main { }\n";
    static const char expected[] = "extern float Noise(float x, float y);\nscene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
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

TIDE_TEST(lsp_operators)
{
    start();
    open_document(OPERATORS);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
    // `+` in code goes to the operator.
    const char *hover = request_at("file:///test.tide", "textDocument/hover", 13, 32, "");
    TIDE_CHECK(has(hover, "Money operator +(Money a, Money b)") && has(hover, "Operator of struct `Money`."));
    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/definition", 13, 32, ""),
                   "\"range\":{\"start\":{\"line\":4,\"character\":10}"));
    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/prepareRename", 13, 32, ""),
                   "Operators are named by their symbol"));
    // Methods are listed after a dot; operators aren't.
    const char *members = complete(OPERATORS "system S(Wallet wallet)\n{\n    var d = wallet.total.$\n}\n");
    TIDE_CHECK(offers(members, "Dollars") && !has(members, "operator"));

    // Methods rename, at their declaration and every call.
    open_document(OPERATORS "system S(Wallet wallet)\n{\n    var d = wallet.total.Dol$lars();\n}\n");
    request_with("textDocument/rename", "\"newName\":\"Whole\"");
    const char *renamed = apply_reply(OPERATORS "system S(Wallet wallet)\n{\n    var d = wallet.total.Dollars();\n}\n",
                                      "file:///test.tide");
    TIDE_CHECK(has(renamed, "int Whole() {") && has(renamed, "wallet.total.Whole();"));

    // Snippets for what goes where: functions at the top, methods in structs
    // and components, operators in structs.
    TIDE_CHECK(offers(complete("$"), "function"));
    const char *in_struct = complete("struct Money\n{\n    int cents;\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(offers(in_struct, "method") && offers(in_struct, "mut method") && offers(in_struct, "operator"));
    TIDE_CHECK(has(in_struct, "Money operator ${1:+}(Money a, Money b)"));
    const char *in_component = complete("component Unit\n{\n    int kills;\n    $\n}\nscene Main { }\n");
    TIDE_CHECK(offers(in_component, "method") && !offers(in_component, "operator"));
    TIDE_CHECK(offers(complete("struct Money\n{\n    int cents;\n    Money $\n}\nscene Main { }\n"), "operator"));

    // Laid out like C#: `operator +(`, and unary minus stays unary.
    static const char messy[] = "struct Money\n{\nint cents;\nMoney operator+(Money a,Money b) { return a; }\n"
                                "Money operator - (Money a) { return Money { cents = -a.cents }; }\n}\nscene Main { }\n";
    static const char expected[] = "struct Money\n{\n    int cents;\n    Money operator +(Money a, Money b) { return a; }\n"
                                   "    Money operator -(Money a) { return Money { cents = -a.cents }; }\n}\n"
                                   "scene Main { }\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
}

// #if and the rest: the formatter leaves directives and what they leave out as
// they are, which reads as comments, and each branch folds.
TIDE_TEST(lsp_directives)
{
    start();
    static const char messy[] = "scene Main { }\n"
                                "#if TIDE_0_1_OR_NEWER\n"
                                "const int A = 1;\n"
                                "#else\n"
                                "   this isn't /* code {{{\n"
                                "#endif\n"
                                "singleton Count { int n; }\n"
                                "system S(mut Count c)\n"
                                "{\n"
                                "#if TIDE_9999_0_OR_NEWER\n"
                                "        nonsense(\n"
                                "#else\n"
                                "c.n=A;\n"
                                "#endif\n"
                                "}\n";
    static const char expected[] = "scene Main { }\n"
                                   "#if TIDE_0_1_OR_NEWER\n"
                                   "const int A = 1;\n"
                                   "#else\n"
                                   "   this isn't /* code {{{\n"
                                   "#endif\n"
                                   "singleton Count { int n; }\n"
                                   "system S(mut Count c)\n"
                                   "{\n"
                                   "#if TIDE_9999_0_OR_NEWER\n"
                                   "        nonsense(\n"
                                   "#else\n"
                                   "    c.n = A;\n"
                                   "#endif\n"
                                   "}\n";
    open_document(messy);
    TIDE_CHECK(has(sent[0], "\"diagnostics\":[]")); // What's left out isn't read
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
    TIDE_CHECK(has(format_reply(expected), "\"result\":[]"));

    open_document(expected);
    const char *tokens = request("textDocument/semanticTokens/full");
    TIDE_CHECK(has(tokens, "1,0,3,11,0,0,4,17,15,0"));   // #if a keyword, its symbol a macro
    TIDE_CHECK(has(tokens, "1,3,22,14,0"));              // The line it leaves out a comment
    const char *folds = request("textDocument/foldingRange");
    TIDE_CHECK(has(folds, "{\"startLine\":1,\"endLine\":2,\"kind\":\"region\"}")); // #if to #else
    TIDE_CHECK(has(folds, "{\"startLine\":3,\"endLine\":4,\"kind\":\"region\"}")); // #else to #endif
    open_document("scene Main { }\n#if false\nthis is no$t code\n#endif\n");
    TIDE_CHECK(has(request("textDocument/completion"), "\"items\":[]"));
}

TIDE_TEST(lsp_folding)
{
    start();
    open_document("// One\n// Two\n// Three\ncomponent Body\n{\n    float x;\n}\nsystem Main()\n{\n    Spawn(Body);\n}\n");
    const char *folds = request("textDocument/foldingRange");
    TIDE_CHECK(has(folds, "{\"startLine\":0,\"endLine\":2,\"kind\":\"comment\"}"));
    TIDE_CHECK(has(folds, "{\"startLine\":3,\"endLine\":5}")); // From `component Body`, keeping `}` in sight
    TIDE_CHECK(has(folds, "{\"startLine\":7,\"endLine\":9}"));

    // Comments in /* */, lists of arguments and elements, and using lines
    open_document("using A;\nusing B;\n/* One\n   two */\nnamespace C;\nvoid Draw(int x,\n          int y)\n{\n}\n"
                  "void Call()\n{\n    Draw(\n        1,\n        2\n    );\n    List<int> xs = [\n        1,\n        2];\n}\n");
    folds = request("textDocument/foldingRange");
    TIDE_CHECK(has(folds, "{\"startLine\":0,\"endLine\":1,\"kind\":\"imports\"}"));
    TIDE_CHECK(has(folds, "{\"startLine\":2,\"endLine\":3,\"kind\":\"comment\"}"));
    TIDE_CHECK(has(folds, "{\"startLine\":5,\"endLine\":6}"));   // The parameters, with their `)`
    TIDE_CHECK(has(folds, "{\"startLine\":11,\"endLine\":13}")); // The arguments, keeping `);` in sight
    TIDE_CHECK(has(folds, "{\"startLine\":15,\"endLine\":17}")); // The list's elements
}

// Braceless bodies under loops keep their level, however deep; and an enum
// with a backing type gets its brace moved like any other.
TIDE_TEST(lsp_format_braceless_loops)
{
    start();
    static const char messy[] = "singleton F { List<int> xs; Grid2<int> cells; }\nscene Main { }\n"
                                "system S(mut F f)\n{\nwhile (f.xs.count > 3)\nif (f.xs.count > 4)\nreturn;\n"
                                "foreach (var x in f.xs)\nfor (var i = 0; i < x; i++)\nif (i > 2)\nreturn;\n"
                                "parallel (var at in f.cells)\nf.cells[at] = 1;\nreturn;\n}\n";
    static const char expected[] = "singleton F { List<int> xs; Grid2<int> cells; }\nscene Main { }\n"
                                   "system S(mut F f)\n{\n    while (f.xs.count > 3)\n        if (f.xs.count > 4)\n"
                                   "            return;\n    foreach (var x in f.xs)\n        for (var i = 0; i < x; i++)\n"
                                   "            if (i > 2)\n                return;\n    parallel (var at in f.cells)\n"
                                   "        f.cells[at] = 1;\n    return;\n}\n";
    format_reply(messy);
    const char *formatted = apply_reply(messy, NULL);
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);
    TIDE_CHECK(has(format_reply(expected), "\"result\":[]"));

    static const char backed[] = "enum Voxel : byte {\n    Air,\n    Stone,\n}\nenum Page {\n    Title,\n}\nscene Main { }\n";
    static const char braced[] = "enum Voxel : byte\n{\n    Air,\n    Stone,\n}\nenum Page\n{\n    Title,\n}\nscene Main { }\n";
    format_reply(backed);
    TIDE_CHECK(strcmp(apply_reply(backed, NULL), braced) == 0);
    TIDE_CHECK(has(format_reply(braced), "\"result\":[]"));
}

// A code action at a 0-based line.
static const char *actions_at(const int line)
{
    char range[160];
    snprintf(range, sizeof range, "\"range\":{\"start\":{\"line\":%d,\"character\":0},\"end\":{\"line\":%d,\"character\":0}}",
             line, line);
    return request_at("file:///test.tide", "textDocument/codeAction", line, 0, range);
}

TIDE_TEST(lsp_quick_fixes)
{
    start();
    open_document("struct Stats\n{\n    float health;\n    void Heal(float amount) { health += amount; }\n}\n"
                  "component Unit { Stats stats; Armor armor; }\n"
                  "void Bump(int count) { count += 1; }\n"
                  "event(Spawned) Setup(with Main) { var x = 1; x = 2; Spawn(Unit); Grow(x, 2.5); }\n"
                  "system Move(mut Velocity velocity) { }\n"
                  "scene Main { }\n");
    const char *method = actions_at(3);
    TIDE_CHECK(has(method, "Make 'Heal' mut") && has(method, "\"start\":{\"line\":3,\"character\":4}"));
    TIDE_CHECK(has(actions_at(5), "Create struct 'Armor'"));
    const char *param = actions_at(6);
    TIDE_CHECK(has(param, "Declare 'count' as mut") && has(param, "\"start\":{\"line\":6,\"character\":10}"));
    const char *main = actions_at(7);
    TIDE_CHECK(has(main, "Declare 'x' as mut") && has(main, "\"start\":{\"line\":7,\"character\":34}"));
    TIDE_CHECK(has(main, "Create function 'Grow'") && has(main, "\"newText\":\"\\nvoid Grow(int x, float value)\\n{\\n}\\n\""));
    TIDE_CHECK(has(main, "\"start\":{\"line\":10,\"character\":0}")); // At the end of the file
    TIDE_CHECK(has(actions_at(8), "Create component 'Velocity'"));
}

// The checker's "did you mean" is a quick fix, from where the name is, not
// from the message.
TIDE_TEST(lsp_quick_fixes_from_suggestions)
{
    start();
    static const char typo[] = GAME_TYPES "system Move(mut Body body)\n{\n    body.radus = 1;\n}\n";
    open_document(typo);
    TIDE_CHECK(has(last_sent(), "did you mean 'radius'?"));
    const char *fix = actions_at(24);
    TIDE_CHECK(has(fix, "\"title\":\"Change to 'radius'\",\"kind\":\"quickfix\",\"isPreferred\":true,\"diagnostics\":[{"));
    const json *reply = json_parse(last_sent(), strlen(last_sent()));
    const json *edits = json_path(json_get(reply, "result")->items[0], "edit", "changes", "file:///test.tide", NULL);
    char text[8192];
    snprintf(text, sizeof text, "%s", GAME_TYPES "system Move(mut Body body)\n{\n    body.radus = 1;\n}\n");
    apply_edits(text, sizeof text, edits);
    json_release();
    TIDE_CHECK(has(text, "    body.radius = 1;\n"));

    // One of the engine's: a member of Math
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.radius = Math.Clmp(body.radius, 0, 1);\n}\n");
    const char *math = actions_at(24);
    TIDE_CHECK(has(math, "Change to 'Clamp'"));
    TIDE_CHECK(has(math, "{\"range\":{\"start\":{\"line\":24,\"character\":23},\"end\":{\"line\":24,\"character\":27}},"
                         "\"newText\":\"Clamp\"}"));
    TIDE_CHECK(!has(actions_at(23), "Change to")); // Only on its line
}

// An error nothing handles: carry on with the default, or pass it on.
TIDE_TEST(lsp_quick_fixes_for_errors)
{
    start();
    open_document("enum ParseError { Empty }\nint Parse(string t) fails ParseError { return 1; }\n"
                  "int Twice(string t) fails ParseError\n{\n    Parse(t);\n    var n = Parse(t) + 1;\n    return n;\n}\n"
                  "scene Main { }\nsystem S()\n{\n    Parse(\"1\");\n}\n");
    const char *statement = actions_at(4);
    TIDE_CHECK(has(statement, "add '!'\",\"kind\":\"quickfix\",\"isPreferred\":true"));
    TIDE_CHECK(has(statement, "{\"range\":{\"start\":{\"line\":4,\"character\":12},\"end\":{\"line\":4,\"character\":12}},"
                              "\"newText\":\"!\"}"));
    TIDE_CHECK(has(statement, "Pass the error on to the caller of 'Twice': add 'try'"));
    TIDE_CHECK(has(statement, "{\"range\":{\"start\":{\"line\":4,\"character\":4},\"end\":{\"line\":4,\"character\":4}},"
                              "\"newText\":\"try \"}"));
    const char *value = actions_at(5);
    TIDE_CHECK(has(value, "\"newText\":\"!\"") && has(value, "{\"line\":5,\"character\":20}"));
    // A system has no caller to pass it to
    const char *system = actions_at(11);
    TIDE_CHECK(has(system, "add '!'") && !has(system, "add 'try'"));
}

// Notes about another place go there, and access a system doesn't use is
// marked as unnecessary.
TIDE_TEST(lsp_diagnostic_notes_and_tags)
{
    static const char twice[] = "settings { tickRate = 30; }\nsettings { title = \"x\"; }\nscene Main { }\n";
    start_with("{\"capabilities\":{\"textDocument\":{\"publishDiagnostics\":{\"relatedInformation\":true}}}}");
    open_document(twice);
    TIDE_CHECK(has(last_sent(), "\"message\":\"a game has one 'settings' block\",\"relatedInformation\":[{\"location\":"
                                "{\"uri\":\"file:///test.tide\",\"range\":{\"start\":{\"line\":0,\"character\":0}"));
    TIDE_CHECK(has(last_sent(), "\"message\":\"the other one is in /test.tide, on line 1\"}]"));
    TIDE_CHECK(!has(last_sent(), "\\nnote: the other one"));
    start(); // An editor that doesn't show them keeps them in the message
    open_document(twice);
    TIDE_CHECK(has(last_sent(), "a game has one 'settings' block\\nnote: the other one is in"));

    open_document("component Body { float2 position; }\nsingleton Score { int total; }\n"
                  "event(Spawned) Setup(with Main) { Spawn(Body); }\n"
                  "system Look(mut Body body, Score s) { if (body.position.x > 0) return; }\nscene Main { }\n");
    TIDE_CHECK(count(last_sent(), "\"tags\":[1]") == 2); // mut never written, and never used
}

TIDE_TEST(lsp_move_to_file)
{
    start();
    open_document("namespace Combat;\n\n// How much damage it takes.\ncomponent Health\n{\n    int value;\n}\n\n"
                  "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Health); }\n");
    const char *move = request_at("file:///test.tide", "textDocument/codeAction", 4, 0,
                                  "\"range\":{\"start\":{\"line\":4,\"character\":0},\"end\":{\"line\":4,\"character\":0}}");
    TIDE_CHECK(has(move, "\"title\":\"Move 'Health' to Health.tide\",\"kind\":\"refactor.move\""));
    TIDE_CHECK(has(move, "{\"kind\":\"create\",\"uri\":\"file:///Health.tide\""));
    // The new file: the namespace, then the declaration with its comment.
    TIDE_CHECK(has(move, "\"newText\":\"namespace Combat;\\n\\n// How much damage it takes.\\ncomponent Health\\n{\\n"
                         "    int value;\\n}\\n\""));
    // Removed here, with the blank line after it.
    TIDE_CHECK(has(move, "{\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":8,\"character\":0}},"
                         "\"newText\":\"\"}"));

    // Systems and handlers stay: files decide the order they run in.
    const char *system = request_at("file:///test.tide", "textDocument/codeAction", 9, 0,
                                    "\"range\":{\"start\":{\"line\":9,\"character\":0},\"end\":{\"line\":9,\"character\":0}}");
    TIDE_CHECK(!has(system, "Move '"));

    // An editor that can't create files isn't offered it.
    start_with("{}");
    open_document("component Health\n{\n    int value;\n}\nscene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Health); }\n");
    const char *unable = request_at("file:///test.tide", "textDocument/codeAction", 1, 0,
                                    "\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":0}}");
    TIDE_CHECK(!has(unable, "Move '"));
}

#define USES_RADIUS                                                            \
    GAME_TYPES                                                                 \
    "system Grow(mut Body body)\n{\n    body.radius += 1;\n}\n"                  \
    "system Make()\n{\n    Spawn(Body { radius = 2 });\n}\n"

TIDE_TEST(lsp_references)
{
    start();
    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.rad$ius, Color.red);\n}\n");
    // The declaration, `body.radius += 1`, the literal and the view.
    TIDE_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":true}"), "\"uri\"") == 4);
    TIDE_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":false}"), "\"uri\"") == 3);

    open_document(GAME_TYPES "system Move(mut Body bo$dy)\n{\n    body.radius = body.radius * 2;\n}\n");
    const char *highlights = request("textDocument/documentHighlight");
    TIDE_CHECK(count(highlights, "\"range\"") == 3);
    TIDE_CHECK(count(highlights, "\"kind\":3") == 1); // The declaration

    // Uses in what a method returns.
    open_document("struct Range\n{\n    float l$o;\n    float hi;\n    float Width() { return hi - lo; }\n}\n");
    TIDE_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":false}"), "\"uri\"") == 1);
}

TIDE_TEST(lsp_rename)
{
    start();
    static const char program[] = USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.radius, Color.red);\n}\n";
    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.rad$ius, Color.red);\n}\n");
    TIDE_CHECK(has(request("textDocument/prepareRename"), "\"result\":{\"start\""));
    request_with("textDocument/rename", "\"newName\":\"size\"");
    const char *renamed = apply_reply(program, "file:///test.tide");
    TIDE_CHECK(has(renamed, "float size = 10;"));
    TIDE_CHECK(has(renamed, "body.size += 1;"));
    TIDE_CHECK(has(renamed, "Body { size = 2 }"));
    TIDE_CHECK(has(renamed, "body.position, body.size,"));
    TIDE_CHECK(!has(renamed, "radius"));
    // The result still compiles cleanly.
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", renamed);
    open_document(copy);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));
}

// Main renames like anything else the game declares, though the program then
// has no entry point.
TIDE_TEST(lsp_rename_main)
{
    start();
    open_document("scene Ma$in { }\nevent(Spawned) Setup(with Main) { }\n");
    TIDE_CHECK(has(request("textDocument/prepareRename"), "\"result\":{\"start\""));
    request_with("textDocument/rename", "\"newName\":\"Menu\"");
    TIDE_CHECK(strcmp(apply_reply("scene Main { }\nevent(Spawned) Setup(with Main) { }\n", "file:///test.tide"),
                      "scene Menu { }\nevent(Spawned) Setup(with Menu) { }\n") == 0);
}

#define NAMESPACED                                                                                            \
    "component Health { int value; }\n"                                                                       \
    "scene Main { }\n"                                                                                        \
    "system Hurt(mut Game.Combat.Health health) { health.value -= 1; }\n"                                     \
    "event(Spawned) Setup(with Main) { Spawn(Game.Combat.Health { value = 3 }); Spawn(Game.Combat.Health); }\n"

// A namespace renames one part at a time, wherever its path is written.
TIDE_TEST(lsp_rename_namespace)
{
    start();
    open_document("namespace Game.Com$bat;\n" NAMESPACED);
    TIDE_CHECK(has(request("textDocument/prepareRename"), "\"result\":{\"start\""));
    request_with("textDocument/rename", "\"newName\":\"Fight\"");
    const char *renamed = apply_reply("namespace Game.Combat;\n" NAMESPACED, "file:///test.tide");
    TIDE_CHECK(strcmp(renamed, "namespace Game.Fight;\n"
                               "component Health { int value; }\n"
                               "scene Main { }\n"
                               "system Hurt(mut Game.Fight.Health health) { health.value -= 1; }\n"
                               "event(Spawned) Setup(with Main) { Spawn(Game.Fight.Health { value = 3 }); "
                               "Spawn(Game.Fight.Health); }\n") == 0);
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", renamed);
    open_document(copy);
    TIDE_CHECK(has(last_sent(), "\"diagnostics\":[]"));

    // The outer part, from code.
    open_document("namespace Game.Combat;\n"
                  "component Health { int value; }\n"
                  "scene Main { }\n"
                  "system Hurt(mut Game.Combat.Health health) { health.value -= 1; }\n"
                  "event(Spawned) Setup(with Main) { Spawn(Game.Combat.Health { value = 3 }); Spawn(Ga$me.Combat.Health); }\n");
    request_with("textDocument/rename", "\"newName\":\"Play\"");
    renamed = apply_reply("namespace Game.Combat;\n" NAMESPACED, "file:///test.tide");
    TIDE_CHECK(count(renamed, "Play.Combat") == 4 && !has(renamed, "Game"));
}

TIDE_TEST(lsp_rename_refusals)
{
    start();
    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Circle(body.position, body.rad$ius, Color.red);\n}\n");
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"position\""), "'position' is already declared"));
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"return\""), "keyword"));
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"float3\""), "built into the language"));
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"2fast\""), "start with a letter"));

    open_document(USES_RADIUS "view V(Body body)\n{\n    Draw.Cir$cle(body.position, body.radius, Color.red);\n}\n");
    TIDE_CHECK(has(request("textDocument/prepareRename"), "Built-in names can't be renamed"));

    open_document(GAME_TYPES "system Mo$ve(mut Body body)\n{\n    body.radius = ;\n}\n");
    TIDE_CHECK(has(request("textDocument/prepareRename"), "Fix the syntax errors first"));
}

#define BUILT_IN_METHODS                                                                                    \
    "singleton Field { Grid2<int> cells = Grid2(8, 8); List<int> items; string name; }\n"                   \
    "singleton Camera { float2 center; }\ncomponent Body { float x; }\nscene Main { }\n"

// Built-in methods are what they're called on's: a list's Clear isn't an
// entity's Destroy. Hovers, signatures and references say so.
TIDE_TEST(lsp_built_in_methods)
{
    start();
    open_document(BUILT_IN_METHODS "system S(mut Field field)\n{\n    field.items.Cl$ear();\n}\n");
    TIDE_CHECK(!has(last_sent(), "\"severity\":1"));
    const char *clear = request("textDocument/hover");
    TIDE_CHECK(has(clear, "List<int>.Clear()") && has(clear, "Removes every element.") && !has(clear, "Destroys"));
    open_document(BUILT_IN_METHODS "system S(mut Field field)\n{\n    var has = field.items.Conta$ins(3);\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "List<int>.Contains(int item) -> bool"));
    open_document(BUILT_IN_METHODS "system S(mut Field field)\n{\n    field.cells.Cl$ear();\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Grid2<int>.Clear()"));
    open_document(BUILT_IN_METHODS "system S(mut Body body)\n{\n    this.Sn$ap();\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "entity.Snap()"));
    open_document(BUILT_IN_METHODS "system S(mut Camera camera)\n{\n    camera.Sn$ap();\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "singleton.Snap()"));
    open_document(BUILT_IN_METHODS "system S(mut Body body)\n{\n    this.Dest$roy();\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "entity.Destroy()"));
    open_document(BUILT_IN_METHODS "system S(Field field)\n{\n    var n = field.name.len$gth;\n}\n");
    TIDE_CHECK(has(request("textDocument/hover"), "How many characters the text has."));
    // A grid made in a default: a type, as constructors are
    open_document("singleton Field { Grid2<int> cells = Gr$id2(8, 8); }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "Grid2<int>"));

    // Signatures, by what the call is on
    open_document(BUILT_IN_METHODS "system S(mut Field field)\n{\n    field.items.Insert($\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "List<int>.Insert(int index, int item)"));
    open_document(BUILT_IN_METHODS "system S(mut Field field)\n{\n    field.items.Add($\n}\n");
    const char *add = request("textDocument/signatureHelp");
    TIDE_CHECK(has(add, "List<int>.Add(int item)") && !has(add, "components"));
    open_document(BUILT_IN_METHODS "system S(mut Body body)\n{\n    this.Add($\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "entity.Add(components...)"));
    open_document(BUILT_IN_METHODS "system S(Field field)\n{\n    var t = field.name.Substring(1, $\n}\n");
    const char *substring = request("textDocument/signatureHelp");
    TIDE_CHECK(has(substring, "string.Substring(int start, int length) -> string") && has(substring, "\"activeSignature\":1"));
    open_document("scene Main { }\nview V()\n{\n    var t = \"cat\".ToUpper($\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "string.ToUpper()"));
    open_document("scene Main { }\nview V()\n{\n    Clipboard.Copy($\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "Clipboard.Copy(string text)"));
    open_document("singleton Field { Grid2<int> cells = Grid2(8, $ }\nscene Main { }\n");
    const char *grid = request("textDocument/signatureHelp");
    TIDE_CHECK(has(grid, "Grid2(int width, int height)") && has(grid, "\"activeParameter\":1"));
    open_document("scene Main { }\nview V()\n{\n    Draw.Text(\"a\", $\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "Draw.Text(string text, float2 position, float size, Color color)"));

    // References: a grid's Clear, not a list's; and Snap and Grid2 are found at all
    static const char program[] = BUILT_IN_METHODS "system S(mut Field field, with Body)\n{\n    field.cells.Clear();\n"
                                  "    field.items.Clear();\n    field.cells.Clear();\n    this.Snap();\n}\n";
    open_document(program);
    TIDE_CHECK(count(request_at("file:///test.tide", "textDocument/references", 6, 17,
                                "\"context\":{\"includeDeclaration\":true}"),
                     "\"uri\"") == 2);
    TIDE_CHECK(count(request_at("file:///test.tide", "textDocument/references", 7, 17,
                                "\"context\":{\"includeDeclaration\":true}"),
                     "\"uri\"") == 1);
    TIDE_CHECK(has(request_at("file:///test.tide", "textDocument/hover", 9, 10, ""), "entity.Snap()"));
}

// Inlay hints name the parameters literal arguments go to, the built-ins' too,
// but for one-letter names, which only say where the argument is.
TIDE_TEST(lsp_inlay_hints_for_built_ins)
{
    start();
    open_document("local singleton Menu { float volume; }\nscene Main { }\n"
                  "view V(mut Menu menu)\n{\n    Draw.Circle(float2(1, 2), 3, Color.red);\n"
                  "    menu.volume = Math.Clamp(0.5, 0, menu.volume);\n"
                  "    GUILayout.Slider(\"Volume\", menu.volume, 0, 1);\n    Session.Open(7777);\n"
                  "    var c = Color(1, 0.5, 0);\n}\n");
    TIDE_CHECK(!has(last_sent(), "\"severity\":1"));
    const char *hints = request_at("file:///test.tide", "textDocument/inlayHint", 0, 0,
                                   "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":20,\"character\":0}}");
    TIDE_CHECK(has(hints, "{\"position\":{\"line\":4,\"character\":30},\"label\":\"radius:\",\"kind\":2"));
    TIDE_CHECK(!has(hints, "\"label\":\"center:\"") && !has(hints, "\"label\":\"color:\"")); // Not literals
    TIDE_CHECK(has(hints, "\"label\":\"label:\"") && has(hints, "\"label\":\"min:\"") && has(hints, "\"label\":\"max:\""));
    TIDE_CHECK(has(hints, "\"label\":\"port:\""));
    // float2(1, 2), Math.Clamp(x, a, b) and Color(r, g, b)
    TIDE_CHECK(!has(hints, "\"label\":\"x:\"") && !has(hints, "\"label\":\"a:\"") && !has(hints, "\"label\":\"r:\""));
}

// A grid's cells[x, y] is cells[int2(x, y)] to the checker: x is still x.
TIDE_TEST(lsp_grid_positions_keep_their_names)
{
    start();
    open_document("singleton Field { Grid2<int> cells; }\nscene Main { }\n"
                  "system S(mut Field field)\n{\n    var x$ = 1;\n    field.cells[x, 2] = x;\n}\n");
    TIDE_CHECK(!has(last_sent(), "\"severity\":1"));
    TIDE_CHECK(count(request_with("textDocument/references", "\"context\":{\"includeDeclaration\":true}"), "\"uri\"") == 3);
    request_with("textDocument/rename", "\"newName\":\"column\"");
    TIDE_CHECK(has(apply_reply("singleton Field { Grid2<int> cells; }\nscene Main { }\n"
                               "system S(mut Field field)\n{\n    var x = 1;\n    field.cells[x, 2] = x;\n}\n",
                               "file:///test.tide"),
                   "field.cells[column, 2] = column;"));
}

// Go to definition on an enum's member, and hovers that had none or were stale.
TIDE_TEST(lsp_enum_members_and_hovers)
{
    start();
    open_document("enum Page { Title, Options }\nsingleton Menu { Page page; }\nscene Main { }\n"
                  "system S(mut Menu menu) { menu.page = Page.Opt$ions; }\n");
    TIDE_CHECK(has(request("textDocument/definition"), "\"range\":{\"start\":{\"line\":0,\"character\":19}"));
    // The engine's enums have no source to go to, and keep their names.
    open_document("scene Arena { }\nscene Main { }\n"
                  "event(Spawned) Setup(with Main) { Scene.Load(Arena, SceneVisibility.Priv$ate); }\n");
    TIDE_CHECK(has(request("textDocument/definition"), "\"result\":null"));
    TIDE_CHECK(has(request("textDocument/prepareRename"), "Members of the engine's enums can't be renamed."));

    // Session's calls, as the checker has them
    open_document("local scene Main { }\nview Menu()\n{\n    Sess$ion.Leave();\n}\n");
    const char *session = request("textDocument/hover");
    TIDE_CHECK(has(session, "Start, Open, Close, Kick, KickAll, Join, Connect, Leave and End") && !has(session, "Play"));
    open_document("[Native$Name(\"c_noise\")]\nextern float Noise(float x);\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "The C function the extern function after it calls"));
    open_document("enum Voxel : by$te { Air, Stone }\nscene Main { }\n");
    TIDE_CHECK(has(request("textDocument/hover"), "one byte, so its members go from 0 to 255"));
    // byte reads as a keyword (11), like int
    TIDE_CHECK(has(request("textDocument/semanticTokens/full"), "\"data\":[0,5,5,13,1,0,8,4,11,0,"));
}

// Renaming refuses every keyword, the lexer's and the parser's.
TIDE_TEST(lsp_rename_keywords)
{
    start();
    open_document(GAME_TYPES "system Mo$ve(mut Body body)\n{\n    body.radius = 1;\n}\n");
    static const char *const keywords[] = {"while", "for", "foreach", "parallel", "continue", "this", "await", "null",
                                           "struct", "event", "const", "async", "fails", "in", "void"};
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        char extra[64];
        snprintf(extra, sizeof extra, "\"newName\":\"%s\"", keywords[i]);
        TIDE_CHECK(has(request_with("textDocument/rename", extra), "keyword"));
    }
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"string\""), "built into the language"));
    // A local can take a word that only starts declarations, as the parser allows.
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    var spe$ed = 1;\n    body.radius = speed;\n}\n");
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"scene\""), "\"changes\""));
    TIDE_CHECK(has(request_with("textDocument/rename", "\"newName\":\"while\""), "keyword"));
}

// The input's Sample and Sanitize are in the outline.
TIDE_TEST(lsp_outline_of_input)
{
    start();
    open_document("local singleton Menu { bool open; }\ninput Keys\n{\n    bool fire;\n\n    Sample(Menu menu) { }\n"
                  "    Sanitize() { }\n}\nscene Main { }\n");
    const char *symbols = request("textDocument/documentSymbol");
    TIDE_CHECK(has(symbols, "{\"name\":\"Sample\",\"detail\":\"Sample(Menu menu)\",\"kind\":6,\"range\":{\"start\":{\"line\":5,"
                            "\"character\":4},\"end\":{\"line\":5,\"character\":25}}"));
    TIDE_CHECK(has(symbols, "\"name\":\"Sanitize\",\"detail\":\"Sanitize()\",\"kind\":6"));
}

// Highlights tell what an assignment changes from what it reads.
TIDE_TEST(lsp_highlights_writes)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    mut var n$ = 1;\n    n = 2;\n    n += body.radius;\n"
                  "    n++;\n    body.radius = n;\n}\n");
    const char *local = request("textDocument/documentHighlight");
    TIDE_CHECK(count(local, "\"kind\":3") == 4); // Declared, set, added to, incremented
    TIDE_CHECK(count(local, "\"kind\":2") == 1); // Read into body.radius
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    body.rad$ius = 1;\n    var r = body.radius;\n}\n");
    const char *field = request("textDocument/documentHighlight");
    TIDE_CHECK(count(field, "\"kind\":3") == 2 && count(field, "\"kind\":2") == 1); // Its declaration is in the file too
}

TIDE_TEST(lsp_signature_help)
{
    start();
    open_document(GAME_TYPES "view V(Body body)\n{\n    Draw.Circle(body.position, $\n}\n");
    const char *circle = request("textDocument/signatureHelp");
    TIDE_CHECK(has(circle, "\"label\":\"Draw.Circle(float2 center, float radius, Color color)\""));
    TIDE_CHECK(has(circle, "\"activeParameter\":1"));
    // "Draw.Circle(" is 12 characters: `float2 center` is 12 to 25.
    TIDE_CHECK(has(circle, "\"parameters\":[{\"label\":[12,25]}"));

    open_document(GAME_TYPES "system S()\n{\n    var v = float3(float2(1, 2), $\n}\n");
    const char *vector = request("textDocument/signatureHelp");
    TIDE_CHECK(has(vector, "float3(float2 xy, float z)"));
    TIDE_CHECK(has(vector, "\"activeSignature\":0,\"activeParameter\":1"));

    // Commas inside a component literal don't count.
    open_document(GAME_TYPES "system S()\n{\n    Spawn(Body { position = float2(1, 2), radius = 3 }, $\n}\n");
    const char *spawn = request("textDocument/signatureHelp");
    TIDE_CHECK(has(spawn, "Spawn(components...)"));
    TIDE_CHECK(has(spawn, "\"activeParameter\":0"));

    open_document(GAME_TYPES "system S()\n{\n    $\n}\n");
    TIDE_CHECK(has(request("textDocument/signatureHelp"), "\"result\":null"));
}

static const char *format_reply(const char *text)
{
    open_document(text);
    return request_with("textDocument/formatting", "\"options\":{\"tabSize\":4,\"insertSpaces\":true}");
}

TIDE_TEST(lsp_format)
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
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);

    // Formatting what's formatted changes nothing.
    TIDE_CHECK(has(format_reply(expected), "\"result\":[]"));

    // Continuation lines go one level deeper, or keep deeper alignment.
    static const char continued[] =
        "system Main()\n{\n    var x = 1\n    + 2;\n    Spawn(Owner,\n          Owner);\n}\n";
    format_reply(continued);
    TIDE_CHECK(strcmp(apply_reply(continued, NULL),
                      "system Main()\n{\n    var x = 1\n        + 2;\n    Spawn(Owner,\n          Owner);\n}\n") == 0);
}

// Selection grows from the word at the cursor through what holds it, out to
// its declaration.
TIDE_TEST(lsp_selection_ranges)
{
    start();
    open_document(GAME_TYPES "system Move(mut Body body)\n{\n    if (body.radius > 1)\n    {\n"
                  "        body.radius = Math.Max(body.ra$dius * 2, 1);\n    }\n}\n");
    const char *ranges = request_with("textDocument/selectionRange", "\"positions\":[{\"line\":26,\"character\":38}]");
    TIDE_CHECK(has(ranges, "\"result\":[{\"range\":{\"start\":{\"line\":26,\"character\":36},\"end\":{\"line\":26,\"character\":42}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":26,\"character\":31},\"end\":{\"line\":26,\"character\":42}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":26,\"character\":31},\"end\":{\"line\":26,\"character\":46}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":26,\"character\":22},\"end\":{\"line\":26,\"character\":50}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":26,\"character\":8},\"end\":{\"line\":26,\"character\":51}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":25,\"character\":4},\"end\":{\"line\":27,\"character\":5}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":24,\"character\":4},\"end\":{\"line\":27,\"character\":5}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":23,\"character\":0},\"end\":{\"line\":28,\"character\":1}},"
                           "\"parent\":{\"range\":{\"start\":{\"line\":22,\"character\":0},\"end\":{\"line\":28,\"character\":1}}}}}}}}}}}]"));
    if (!has(ranges, "\"parent\":{\"range\":{\"start\":{\"line\":22,")) printf("%s\n", ranges);
    // Every position gets one, even outside the code
    const char *two = request_with("textDocument/selectionRange", "\"positions\":[{\"line\":0,\"character\":0},{\"line\":30,\"character\":0}]");
    TIDE_CHECK(count(two, "{\"range\":") == 3); // `component` in its declaration, and the empty end
}

// Formatting a range changes its lines only, as formatting everything would.
TIDE_TEST(lsp_range_formatting)
{
    start();
    TIDE_CHECK(has(last_sent(), "\"documentRangeFormattingProvider\":true"));
    static const char messy[] = "scene Main { }\nsystem S()\n{\nmut var n = 0;\nif (n > 0)\nn+=1;\nn+=2;\n}\n";
    open_document(messy);
    request_with("textDocument/rangeFormatting", "\"range\":{\"start\":{\"line\":5,\"character\":0},\"end\":"
                                                 "{\"line\":6,\"character\":0}},\"options\":{\"tabSize\":4,\"insertSpaces\":true}");
    const char *formatted = apply_reply(messy, NULL);
    // Its indentation as the lines before it make it, though they stay as they are
    TIDE_CHECK(strcmp(formatted, "scene Main { }\nsystem S()\n{\nmut var n = 0;\nif (n > 0)\n        n += 1;\nn+=2;\n}\n") == 0);
    if (strcmp(formatted, "scene Main { }\nsystem S()\n{\nmut var n = 0;\nif (n > 0)\n        n += 1;\nn+=2;\n}\n") != 0) {
        printf("--- got:\n%s---\n", formatted);
    }
}

// C#-style braces: a block that spans lines has its braces on lines of their
// own. Actions on one line and literals stay as they are.
TIDE_TEST(lsp_format_braces)
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
    TIDE_CHECK(strcmp(formatted, expected) == 0);
    if (strcmp(formatted, expected) != 0) printf("--- got:\n%s---\n", formatted);

    // Formatting what's formatted changes nothing.
    char again[8192];
    snprintf(again, sizeof again, "%s", formatted);
    TIDE_CHECK(has(format_reply(again), "\"result\":[]"));

    // New lines match the file's.
    static const char crlf[] = "system Main() {\r\n    return;\r\n}\r\n";
    format_reply(crlf);
    TIDE_CHECK(strcmp(apply_reply(crlf, NULL), "system Main()\r\n{\r\n    return;\r\n}\r\n") == 0);
}

// Formatting keeps every token, in order: only whitespace changes.
TIDE_TEST(lsp_format_keeps_tokens)
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
    TIDE_CHECK(strcmp(formatted, program) != 0);

    // Same text with all whitespace removed.
    char a[8192];
    char b[8192];
    size_t na = 0;
    size_t nb = 0;
    for (const char *p = program; *p; p++) if (*p != ' ' && *p != '\n') a[na++] = *p;
    for (const char *p = formatted; *p; p++) if (*p != ' ' && *p != '\n') b[nb++] = *p;
    a[na] = b[nb] = '\0';
    TIDE_CHECK(strcmp(a, b) == 0);
    TIDE_CHECK(has(formatted, "\"a  b   \\\"c\\\"\"")); // Text keeps its spaces

    // It still compiles as before, and formatting again changes nothing.
    open_document(formatted);
    TIDE_CHECK(!has(last_sent(), "\"severity\":1"));
    TIDE_CHECK(has(format_reply(formatted), "\"result\":[]"));
}

// Typing a program from scratch: every prefix is analysed and queried, as an
// editor does on each keystroke. Nothing may crash, whatever state it's in.
TIDE_TEST(lsp_every_prefix_is_safe)
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
        "    Draw.Circle(body.position, body.radius, Color.red);\n}\n"
        "enum ParseError { Empty }\n"
        "int Parse(string t) fails ParseError\n{\n    if (t == \"\") fail ParseError.Empty;\n    return 1;\n}\n"
        "int? Find(int x)\n{\n    if (x > 0) return x;\n    return null;\n}\n"
        "int Twice(string t) fails ParseError\n{\n    var n = try Parse(t);\n"
        "    if (Parse(t) is int m && m > 0) return m;\n    return (Find(n) ?? 0) + Parse(t)!;\n}\n"
        "singleton Field { Grid2<int> cells = Grid2(8, 8); List<int> items; }\n"
        "system Fill(mut Field field)\n{\n    foreach (var i in field.items) field.items.Add(i);\n"
        "    parallel (var at in field.cells) field.cells[at] = 1;\n"
        "    field.cells[1, 2] = field.items.IndexOf($$\"{3}\".length);\n    field.cells.Clear();\n}\n";
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
        request("textDocument/typeDefinition");
        char positions[96];
        snprintf(positions, sizeof positions, "\"positions\":[{\"line\":%d,\"character\":%d}]", line, character);
        request_with("textDocument/selectionRange", positions);
        if (n % 16 == 0) {
            request("textDocument/documentSymbol");
            request_with("textDocument/references", "\"context\":{\"includeDeclaration\":true}");
            request_with("textDocument/rename", "\"newName\":\"renamed\"");
            request_with("textDocument/formatting", "\"options\":{\"tabSize\":4,\"insertSpaces\":true}");
            request_with("textDocument/rangeFormatting", "\"range\":{\"start\":{\"line\":3,\"character\":0},\"end\":"
                                                         "{\"line\":9,\"character\":0}},\"options\":{\"tabSize\":4}");
            request("textDocument/implementation");
            request("textDocument/prepareCallHierarchy");
            request("textDocument/foldingRange");
            char range[160];
            snprintf(range, sizeof range, "\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":%d,\"character\":0}}",
                     line + 1);
            request_with("textDocument/codeAction", range);
            request_with("textDocument/inlayHint", range);
        }
    }
    TIDE_CHECK(has(last_sent(), "\"result\""));
}

// ---------------------------------------------------------------------------
// A game of several files, found through the manifest tide_add_game writes

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
    jb_put(&b, ",\"languageId\":\"tide\",\"version\":1,\"text\":");
    jb_string(&b, text);
    jb_put(&b, "}}}");
    clear_sent();
    handle(b.data);
    jb_free(&b);
}

static void close_uri(const char *uri)
{
    jbuf b = {0};
    jb_put(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didClose\",\"params\":{\"textDocument\":{\"uri\":");
    jb_string(&b, uri);
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

TIDE_TEST(lsp_game_of_several_files)
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
    write_file("lsp_game_physics.tide", physics);
    write_file("lsp_game_main.tide", main_file);
    char a[640], b[640], manifest[640], manifest_text[1400], uri_a[700], uri_b[700];
    game_path(a, sizeof a, "lsp_game_physics.tide");
    game_path(b, sizeof b, "lsp_game_main.tide");
    game_path(manifest, sizeof manifest, "lsp_game_manifest.txt");
    snprintf(manifest_text, sizeof manifest_text, "other\t%s/elsewhere.tide\ngame\t%s\ngame\t%s\n", game_dir, a, b);
    write_file("lsp_game_manifest.txt", manifest_text);
    snprintf(uri_a, sizeof uri_a, "file:///%s", a[0] == '/' ? a + 1 : a);
    snprintf(uri_b, sizeof uri_b, "file:///%s", b[0] == '/' ? b + 1 : b);

    start();
    server.manifest = manifest;
    open_uri(uri_b, main_file);
    // Diagnostics for both files, in path order, none of them errors: Body and
    // Gravity come from the other file.
    TIDE_CHECK(sent_count == 2);
    TIDE_CHECK(count(sent[0], uri_b) == 1 && count(sent[1], uri_a) == 1);
    TIDE_CHECK(has(sent[0], "\"diagnostics\":[]") && has(sent[1], "\"diagnostics\":[]"));

    // `Body` leads to the other file.
    TIDE_CHECK(has(request_at(uri_b, "textDocument/definition", 1, 40, ""), uri_a));
    TIDE_CHECK(count(request_at(uri_b, "textDocument/references", 1, 40, "\"context\":{\"includeDeclaration\":true}"),
                     uri_a) >= 2); // The declaration and Gravity's parameter
    TIDE_CHECK(has(last_sent(), uri_b));

    // The hover shows where Move runs.
    TIDE_CHECK(has(request_at(uri_b, "textDocument/hover", 3, 8, ""), "Runs 2nd of 2 systems each tick, after `Physics.Gravity`"));

    // Renaming Gravity from the attribute edits both files.
    const char *rename = request_at(uri_b, "textDocument/rename", 2, 17, "\"newName\":\"Fall\"");
    TIDE_CHECK(has(rename, uri_a) && has(rename, uri_b));
    TIDE_CHECK(count(rename, "\"newText\":\"Fall\"") == 2);

    // So does renaming Physics: its `namespace` line, the `using` and the attribute.
    rename = request_at(uri_b, "textDocument/rename", 0, 8, "\"newName\":\"World\"");
    TIDE_CHECK(has(rename, uri_a) && has(rename, uri_b));
    TIDE_CHECK(count(rename, "\"newText\":\"World\"") == 3);
    TIDE_CHECK(has(request_at(uri_b, "textDocument/rename", 0, 8, "\"newName\":\"Main\""), "'Main' is already declared"));

    // Completion after `Physics.` lists what's inside it.
    open_uri(uri_b, "using Physics;\nevent(Spawned) Setup(with Main) { Spawn(Physics.); }\nscene Main { }\n");
    const char *completion = request_at(uri_b, "textDocument/completion", 1, 48, "");
    TIDE_CHECK(offers(completion, "Body"));
    TIDE_CHECK(!offers(completion, "Gravity")); // Systems only in Before and After

    // An error in one file shows in the other: Main now spawns a Body nobody declares.
    open_uri(uri_b, main_file);
    open_uri(uri_a, "namespace Physics;\ncomponent Shape { float2 position; }\n");
    TIDE_CHECK(sent_count == 2);
    TIDE_CHECK(has(sent[1], uri_a) && has(sent[1], "\"diagnostics\":[]")); // The file that changed is fine...
    TIDE_CHECK(has(sent[0], uri_b) && has(sent[0], "unknown component or singleton 'Body'")); // ...its user isn't

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

// A game listed as a folder, as tide_add_game lists one without SOURCES: every
// .tide file in it and its subfolders, even one the editor hasn't saved yet.
TIDE_TEST(lsp_game_folder)
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
    write_file("lsp_folder/sub/physics.tide", physics);
    write_file("lsp_folder/main.tide", main_file);
    write_file("lsp_folder/notes.txt", "not Tide");
    char manifest[640], manifest_text[1400], uri_main[700], uri_new[700], uri_physics[700];
    game_path(manifest, sizeof manifest, "lsp_folder_manifest.txt");
    snprintf(manifest_text, sizeof manifest_text, "other\t%s/elsewhere.tide\ngame\t%s/lsp_folder/\n", game_dir,
             game_dir);
    write_file("lsp_folder_manifest.txt", manifest_text);
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_folder/main.tide", dir);
    snprintf(uri_new, sizeof uri_new, "file:///%s/lsp_folder/new.tide", dir);
    snprintf(uri_physics, sizeof uri_physics, "file:///%s/lsp_folder/sub/physics.tide", dir);

    start();
    server.manifest = manifest;
    open_uri(uri_main, main_file);
    // Body comes from the file in the subfolder.
    TIDE_CHECK(sent_count == 2);
    TIDE_CHECK(has(sent[0], uri_main) && has(sent[0], "\"diagnostics\":[]"));
    TIDE_CHECK(has(sent[1], uri_physics) && has(sent[1], "\"diagnostics\":[]"));
    // In path order, main.tide comes before sub/physics.tide.
    TIDE_CHECK(has(request_at(uri_main, "textDocument/hover", 2, 8, ""), "Runs 1st of 2 systems each tick."));

    // A new file joins the game before it's saved.
    open_uri(uri_new, "using Physics;\nsystem Fall(mut Body body) { body.position.y -= 2; }\n");
    TIDE_CHECK(sent_count == 3);
    for (int i = 0; i < sent_count; i++) TIDE_CHECK(has(sent[i], "\"diagnostics\":[]"));
    TIDE_CHECK(has(request_at(uri_new, "textDocument/hover", 1, 8, ""), "Runs 2nd of 3 systems each tick."));

    remove_game_file("lsp_folder/sub/physics.tide");
    remove_game_file("lsp_folder/main.tide");
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

// A folder open in the editor is a game, as `tide run` builds it: every .tide
// file in it and its subfolders, with no manifest.
TIDE_TEST(lsp_open_folder_is_a_game)
{
    if (!find_game_dir()) return;
    make_folder("lsp_open");
    make_folder("lsp_open/sub");
    static const char physics[] = "namespace Physics;\n"
                                  "component Body { float2 position; }\n"
                                  "system Gravity(mut Body body) { body.position.y -= 1; }\n";
    static const char main_file[] = "using Physics;\n"
                                    "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
    write_file("lsp_open/sub/physics.tide", physics);
    write_file("lsp_open/main.tide", main_file);
    char root[700], uri_main[700], uri_physics[700];
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_open", dir); // Editors send folders without the last '/'
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_open/main.tide", dir);
    snprintf(uri_physics, sizeof uri_physics, "file:///%s/lsp_open/sub/physics.tide", dir);

    start_in(root);
    open_uri(uri_main, main_file);
    TIDE_CHECK(sent_count == 2);
    TIDE_CHECK(has(sent[0], uri_main) && has(sent[0], "\"diagnostics\":[]"));
    TIDE_CHECK(has(sent[1], uri_physics) && has(sent[1], "\"diagnostics\":[]"));

    // Without the folder open, the file stands alone.
    start();
    open_uri(uri_main, main_file);
    TIDE_CHECK(sent_count == 1 && !has(sent[0], "\"diagnostics\":[]"));

    remove_game_file("lsp_open/sub/physics.tide");
    remove_game_file("lsp_open/main.tide");
    remove_folder("lsp_open/sub");
    remove_folder("lsp_open");
    clear_sent();
    lsp_free(&server);
}

// A game whose tide.packages lists a package in a folder: its files are
// analyzed with the game's, in its namespace. A package's file is analyzed
// with the game that uses it, or else alone, with no Main; and a folder of
// the game with a tide.packages of its own isn't the game's.
TIDE_TEST(lsp_open_folder_with_packages)
{
    if (!find_game_dir()) return;
    make_folder("lsp_pk");
    make_folder("lsp_pk/game");
    make_folder("lsp_pk/game/other");
    make_folder("lsp_pk/physics");
    static const char body[] = "namespace Physics;\n"
                               "component Body { float2 position; }\n"
                               "system Gravity(mut Body body) { body.position.y -= 1; }\n";
    static const char main_file[] = "using Physics;\n"
                                    "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
    write_file("lsp_pk/game/tide.packages", "../physics\n");
    write_file("lsp_pk/game/main.tide", main_file);
    write_file("lsp_pk/game/other/tide.packages", "package Other\n");
    write_file("lsp_pk/game/other/loose.tide", "struct Pair { float a; }\n"); // Outside its namespace, but not the game's
    write_file("lsp_pk/physics/tide.packages", "package Physics\n");
    write_file("lsp_pk/physics/body.tide", body);
    char root[700], uri_main[700], uri_body[700], uri_loose[700];
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_pk/game", dir);
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_pk/game/main.tide", dir);
    snprintf(uri_body, sizeof uri_body, "file:///%s/lsp_pk/physics/body.tide", dir);
    snprintf(uri_loose, sizeof uri_loose, "file:///%s/lsp_pk/physics/loose.tide", dir);

    start_in(root);
    open_uri(uri_main, main_file);
    TIDE_CHECK(sent_count == 2);
    TIDE_CHECK(has(sent[0], uri_body) && has(sent[0], "\"diagnostics\":[]")); // The package's files come first
    TIDE_CHECK(has(sent[1], uri_main) && has(sent[1], "\"diagnostics\":[]"));
    // Opened from the game, the package's file is the game's.
    open_uri(uri_body, body);
    TIDE_CHECK(sent_count == 2 && has(sent[1], uri_main));

    // A file of the package outside its namespace says so.
    open_uri(uri_loose, "struct Pair { float a; }\n");
    TIDE_CHECK(has(sent[1], uri_loose) && has(sent[1], "what package Physics declares goes in its namespace"));
    close_uri(uri_loose);

    // Without the game open, the package stands alone: no Main is missing.
    start();
    open_uri(uri_body, body);
    TIDE_CHECK(sent_count == 1 && has(sent[0], "\"diagnostics\":[]"));

    remove_game_file("lsp_pk/game/tide.packages");
    remove_game_file("lsp_pk/game/main.tide");
    remove_game_file("lsp_pk/game/other/tide.packages");
    remove_game_file("lsp_pk/game/other/loose.tide");
    remove_game_file("lsp_pk/physics/tide.packages");
    remove_game_file("lsp_pk/physics/body.tide");
    remove_folder("lsp_pk/game/other");
    remove_folder("lsp_pk/game");
    remove_folder("lsp_pk/physics");
    remove_folder("lsp_pk");
    clear_sent();
    lsp_free(&server);
}

// An open folder that builds its games with CMake has the manifest
// tide_add_game writes, in build/tools. Its games come from there, and its
// other files stand alone instead of making one game of the whole folder.
TIDE_TEST(lsp_open_folder_with_manifest)
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
    write_file("lsp_cmake/game/physics.tide", physics);
    write_file("lsp_cmake/game/main.tide", main_file);
    write_file("lsp_cmake/tests/alone.tide", alone);
    char manifest_text[700], root[700], uri_main[700], uri_alone[700];
    snprintf(manifest_text, sizeof manifest_text, "game\t%s/lsp_cmake/game/\n", game_dir);
    write_file("lsp_cmake/build/tools/games.txt", manifest_text);
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_cmake", dir);
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_cmake/game/main.tide", dir);
    snprintf(uri_alone, sizeof uri_alone, "file:///%s/lsp_cmake/tests/alone.tide", dir);

    start_in(root);
    open_uri(uri_main, main_file);
    TIDE_CHECK(sent_count == 2);
    for (int i = 0; i < sent_count; i++) TIDE_CHECK(has(sent[i], "\"diagnostics\":[]"));
    // Its own Body doesn't clash with the game's.
    open_uri(uri_alone, alone);
    TIDE_CHECK(sent_count == 1 && has(sent[0], uri_alone) && has(sent[0], "\"diagnostics\":[]"));

    remove_game_file("lsp_cmake/game/physics.tide");
    remove_game_file("lsp_cmake/game/main.tide");
    remove_game_file("lsp_cmake/tests/alone.tide");
    remove_game_file("lsp_cmake/build/tools/games.txt");
    remove_folder("lsp_cmake/game");
    remove_folder("lsp_cmake/tests");
    remove_folder("lsp_cmake/build/tools");
    remove_folder("lsp_cmake/build");
    remove_folder("lsp_cmake");
    clear_sent();
    lsp_free(&server);
}

// The index of the message sent with `uri` in it, or -1.
static int sent_for(const char *uri)
{
    for (int i = 0; i < sent_count; i++) {
        if (has(sent[i], uri)) return i;
    }
    return -1;
}

// Files that change on disk, as the editor reports them through the watchers
// the server registers, and when it saves: the open documents' games are
// analysed again.
TIDE_TEST(lsp_files_change_on_disk)
{
    if (!find_game_dir()) return;
    make_folder("lsp_watch");
    static const char physics[] = "component Body { float2 position; }\n";
    static const char main_file[] = "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
    write_file("lsp_watch/physics.tide", physics);
    write_file("lsp_watch/main.tide", main_file);
    char root[700], uri_main[700], uri_physics[700], uri_new[700], manifest[700], message[2048];
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_watch", dir);
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_watch/main.tide", dir);
    snprintf(uri_physics, sizeof uri_physics, "file:///%s/lsp_watch/physics.tide", dir);
    snprintf(uri_new, sizeof uri_new, "file:///%s/lsp_watch/new.tide", dir);
    snprintf(manifest, sizeof manifest, "%s/elsewhere/build/tools/games.txt", game_dir);

    // Watchers, registered once the editor says it's ready, when it can take them
    clear_sent();
    lsp_free(&server);
    lsp_init(&server, capture, NULL);
    server.manifest = manifest;
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"rootUri\":\"%s\",\"workspaceFolders\":"
             "[{\"uri\":\"%s\",\"name\":\"game\"}],\"capabilities\":{\"workspace\":{\"didChangeWatchedFiles\":"
             "{\"dynamicRegistration\":true,\"relativePatternSupport\":true}}}}}",
             root, root);
    handle(message);
    TIDE_CHECK(has(last_sent(), "\"save\":{\"includeText\":false}"));
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    TIDE_CHECK(sent_count == 1 && has(sent[0], "\"method\":\"client/registerCapability\""));
    TIDE_CHECK(has(sent[0], "\"method\":\"workspace/didChangeWatchedFiles\""));
    TIDE_CHECK(has(sent[0], "{\"globPattern\":\"**/*.tide\"}") && has(sent[0], "{\"globPattern\":\"**/build/tools/games.txt\"}"));
    TIDE_CHECK(has(sent[0], "/elsewhere/build/tools\",\"pattern\":\"games.txt\"}")); // The server's own, outside the folder
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":\"tidels-watch\",\"result\":null}"); // The editor's answer needs none
    TIDE_CHECK(sent_count == 0);

    open_uri(uri_main, main_file);
    TIDE_CHECK(sent_count == 2 && has(sent[sent_for(uri_main)], "\"diagnostics\":[]"));

    // Another file changes on disk: Body is gone
    write_file("lsp_watch/physics.tide", "component Shape { float2 position; }\n");
    clear_sent();
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\",\"params\":{\"changes\":"
             "[{\"uri\":\"%s\",\"type\":2}]}}",
             uri_physics);
    handle(message);
    TIDE_CHECK(sent_for(uri_main) >= 0 && has(sent[sent_for(uri_main)], "unknown name 'Body'"));

    // A new one brings it back
    write_file("lsp_watch/new.tide", physics);
    clear_sent();
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\",\"params\":{\"changes\":"
             "[{\"uri\":\"%s\",\"type\":1}]}}",
             uri_new);
    handle(message);
    TIDE_CHECK(sent_count == 3 && has(sent[sent_for(uri_main)], "\"diagnostics\":[]"));

    // A deleted one takes its diagnostics with it
    write_file("lsp_watch/physics.tide", "component Body { float2 position; }\n"); // Declared twice now
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didSave\",\"params\":{\"textDocument\":{\"uri\":\"file:///none\"}}}");
    TIDE_CHECK(has(sent[sent_for(uri_physics)], "\"severity\":1")); // Saving looks at the disk again too
    remove_game_file("lsp_watch/physics.tide");
    clear_sent();
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWatchedFiles\",\"params\":{\"changes\":"
             "[{\"uri\":\"%s\",\"type\":3}]}}",
             uri_physics);
    handle(message);
    TIDE_CHECK(sent_for(uri_physics) >= 0 && has(sent[sent_for(uri_physics)], "\"diagnostics\":[]"));
    TIDE_CHECK(has(sent[sent_for(uri_main)], "\"diagnostics\":[]"));

    // An editor that can't watch gets no request
    start_with("{\"capabilities\":{}}");
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{}}");
    TIDE_CHECK(sent_count == 0);

    remove_game_file("lsp_watch/new.tide");
    remove_game_file("lsp_watch/main.tide");
    remove_folder("lsp_watch");
    clear_sent();
    lsp_free(&server);
}

// Folders opened and closed in the editor change which files make up a game.
TIDE_TEST(lsp_workspace_folders_change)
{
    if (!find_game_dir()) return;
    make_folder("lsp_folders");
    static const char main_file[] = "scene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
    write_file("lsp_folders/physics.tide", "component Body { float2 position; }\n");
    write_file("lsp_folders/main.tide", main_file);
    char root[700], uri_main[700], message[2048];
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_folders", dir);
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_folders/main.tide", dir);

    start();
    TIDE_CHECK(has(last_sent(), "\"workspaceFolders\":{\"supported\":true,\"changeNotifications\":true}"));
    open_uri(uri_main, main_file);
    TIDE_CHECK(sent_count == 1 && !has(sent[0], "\"diagnostics\":[]")); // Alone: no Body
    clear_sent();
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWorkspaceFolders\",\"params\":{\"event\":"
             "{\"added\":[{\"uri\":\"%s\",\"name\":\"game\"}],\"removed\":[]}}}",
             root);
    handle(message);
    TIDE_CHECK(sent_count == 2 && has(sent[sent_for(uri_main)], "\"diagnostics\":[]"));
    clear_sent();
    snprintf(message, sizeof message,
             "{\"jsonrpc\":\"2.0\",\"method\":\"workspace/didChangeWorkspaceFolders\",\"params\":{\"event\":"
             "{\"added\":[],\"removed\":[{\"uri\":\"%s\",\"name\":\"game\"}]}}}",
             root);
    handle(message);
    TIDE_CHECK(sent_count == 1 && !has(sent[0], "\"diagnostics\":[]"));

    remove_game_file("lsp_folders/physics.tide");
    remove_game_file("lsp_folders/main.tide");
    remove_folder("lsp_folders");
    clear_sent();
    lsp_free(&server);
}

// Workspace symbols come from every game in the open folders, with nothing open.
TIDE_TEST(lsp_workspace_symbols_of_every_game)
{
    if (!find_game_dir()) return;
    make_folder("lsp_symbols");
    make_folder("lsp_symbols/build");
    make_folder("lsp_symbols/build/tools");
    make_folder("lsp_symbols/a");
    make_folder("lsp_symbols/b");
    write_file("lsp_symbols/a/main.tide", "component Rocket { float fuel; }\nscene Main { }\n");
    write_file("lsp_symbols/b/main.tide", "component Rover { float speed; }\nscene Main { }\n");
    char manifest_text[1400], root[700];
    snprintf(manifest_text, sizeof manifest_text, "a\t%s/lsp_symbols/a/\nb\t%s/lsp_symbols/b/main.tide\n", game_dir, game_dir);
    write_file("lsp_symbols/build/tools/games.txt", manifest_text);
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_symbols", dir);

    start_in(root);
    clear_sent();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"workspace/symbol\",\"params\":{\"query\":\"ro\"}}");
    TIDE_CHECK(has(last_sent(), "\"name\":\"Rocket\"") && has(last_sent(), "\"name\":\"Rover\""));
    TIDE_CHECK(has(last_sent(), "lsp_symbols/a/main.tide") && has(last_sent(), "lsp_symbols/b/main.tide"));
    TIDE_CHECK(count(last_sent(), "\"name\":\"Main\"") == 0); // Not what the query asks for

    remove_game_file("lsp_symbols/a/main.tide");
    remove_game_file("lsp_symbols/b/main.tide");
    remove_game_file("lsp_symbols/build/tools/games.txt");
    remove_folder("lsp_symbols/a");
    remove_folder("lsp_symbols/b");
    remove_folder("lsp_symbols/build/tools");
    remove_folder("lsp_symbols/build");
    remove_folder("lsp_symbols");
    clear_sent();
    lsp_free(&server);
}

// A name another namespace declares: write it with it, or use it.
TIDE_TEST(lsp_quick_fixes_for_namespaces)
{
    if (!find_game_dir()) return;
    make_folder("lsp_using");
    static const char main_file[] = "namespace Game;\nusing Other;\n\nscene Main { }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n";
    write_file("lsp_using/physics.tide", "namespace Physics;\ncomponent Body { float2 position; }\n");
    write_file("lsp_using/main.tide", main_file);
    write_file("lsp_using/other.tide", "namespace Other;\ncomponent Shape { float2 position; }\n");
    char root[700], uri_main[700];
    const char *dir = game_dir[0] == '/' ? game_dir + 1 : game_dir;
    snprintf(root, sizeof root, "file:///%s/lsp_using", dir);
    snprintf(uri_main, sizeof uri_main, "file:///%s/lsp_using/main.tide", dir);

    start_in(root);
    open_uri(uri_main, main_file);
    const char *fixes = request_at(uri_main, "textDocument/codeAction", 4, 0,
                                   "\"range\":{\"start\":{\"line\":4,\"character\":0},\"end\":{\"line\":4,\"character\":0}}");
    TIDE_CHECK(has(fixes, "\"title\":\"Write 'Physics.Body'\""));
    TIDE_CHECK(has(fixes, "\"newText\":\"Physics.Body\""));
    TIDE_CHECK(has(fixes, "\"title\":\"Add 'using Physics;'\",\"kind\":\"quickfix\",\"isPreferred\":true"));
    TIDE_CHECK(has(fixes, "{\"range\":{\"start\":{\"line\":2,\"character\":0},\"end\":{\"line\":2,\"character\":0}},"
                          "\"newText\":\"using Physics;\\n\"}")); // After the usings

    remove_game_file("lsp_using/physics.tide");
    remove_game_file("lsp_using/main.tide");
    remove_game_file("lsp_using/other.tide");
    remove_folder("lsp_using");
    clear_sent();
    lsp_free(&server);
}

TIDE_TEST(lsp_shutdown_and_exit)
{
    start();
    handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"shutdown\"}");
    TIDE_CHECK(has(last_sent(), "\"id\":9,\"result\":null"));
    handle("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}");
    TIDE_CHECK(server.exited && server.exit_code == 0);
    clear_sent();
    lsp_free(&server);
}

// Above each system: its stage and why it waits. Access a system doesn't use
// gets a warning and a quick fix.
TIDE_TEST(lsp_schedule_lenses_and_fixes)
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
    TIDE_CHECK(has(sent[0], "'body' is declared mut but never written"));
    TIDE_CHECK(has(sent[0], "'body' is never used"));

    const char *lenses = request("textDocument/codeLens");
    TIDE_CHECK(has(lenses, "stage 1"));
    TIDE_CHECK(has(lenses, "stage 2") && has(lenses, "after Move: both write Body"));
    TIDE_CHECK(has(lenses, "stage 3") && has(lenses, "after Look: both write Score; it writes Body, which this reads"));
    TIDE_CHECK(has(lenses, "nothing runs alongside"));

    const char *hover = request_at("file:///test.tide", "textDocument/hover", 5, 8, "");
    TIDE_CHECK(has(hover, "**Stage 3.**") && has(hover, "`Move`: it writes `Body`, which this reads"));
    TIDE_CHECK(has(hover, "No other system can run at the same time."));

    const char *remove_mut = request_at("file:///test.tide", "textDocument/codeAction", 4, 0,
                                        "\"range\":{\"start\":{\"line\":4,\"character\":0},\"end\":{\"line\":4,\"character\":0}}");
    TIDE_CHECK(has(remove_mut, "Remove 'mut' from 'body'") && has(remove_mut, "\"newText\":\"\""));
    TIDE_CHECK(has(remove_mut, "\"start\":{\"line\":4,\"character\":12}"));
    const char *use_with = request_at("file:///test.tide", "textDocument/codeAction", 5, 0,
                                      "\"range\":{\"start\":{\"line\":5,\"character\":0},\"end\":{\"line\":5,\"character\":0}}");
    TIDE_CHECK(has(use_with, "\"newText\":\"with Body\""));

    open_document("component Body { float2 position; }\nevent(Spawned) Setup(with Main) { Spawn(Body); }\n"
         "system Push(Body body) { body.position.x = 1; }\nscene Main { }\n");
    const char *add_mut = request_at("file:///test.tide", "textDocument/codeAction", 2, 0,
                                     "\"range\":{\"start\":{\"line\":2,\"character\":25},\"end\":{\"line\":2,\"character\":29}}");
    TIDE_CHECK(has(add_mut, "Declare 'body' as mut") && has(add_mut, "\"newText\":\"mut \""));
    TIDE_CHECK(has(add_mut, "\"start\":{\"line\":2,\"character\":12}"));
}
