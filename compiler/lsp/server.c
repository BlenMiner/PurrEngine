#include "server.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis.h"
#include "common.h"
#include "folders.h"
#include "json.h"

enum { METHOD_NOT_FOUND = -32601, PARSE_ERROR = -32700, INVALID_PARAMS = -32602, REQUEST_FAILED = -32803 };

void lsp_init(lsp_server *s, const lsp_send_fn send, void *user)
{
    memset(s, 0, sizeof *s);
    s->send = send;
    s->user = user;
}

static void free_kept(lsp_server *s);

void lsp_free(lsp_server *s)
{
    for (int i = 0; i < s->doc_count; i++) {
        free(s->docs[i].uri);
        free(s->docs[i].text);
    }
    free(s->docs);
    s->docs = NULL;
    s->doc_count = s->doc_cap = 0;
    free_kept(s);
    s->analyzed = false;
}

static char *copy_string(const char *text, const size_t len)
{
    char *out = malloc(len + 1);
    if (!out) abort();
    memcpy(out, text, len);
    out[len] = '\0';
    return out;
}

// ---------------------------------------------------------------------------
// Documents

static lsp_document *find_doc(lsp_server *s, const char *uri)
{
    for (int i = 0; i < s->doc_count; i++) {
        if (strcmp(s->docs[i].uri, uri) == 0) return &s->docs[i];
    }
    return NULL;
}

static lsp_document *set_doc(lsp_server *s, const char *uri, const json *text)
{
    lsp_document *d = find_doc(s, uri);
    if (!d) {
        if (s->doc_count == s->doc_cap) {
            s->doc_cap = s->doc_cap ? s->doc_cap * 2 : 8;
            lsp_document *docs = realloc(s->docs, sizeof(lsp_document) * (size_t)s->doc_cap);
            if (!docs) abort();
            s->docs = docs;
        }
        d = &s->docs[s->doc_count++];
        d->uri = copy_string(uri, strlen(uri));
        d->text = NULL;
    }
    free(d->text);
    d->text = copy_string(text->string, text->string_len);
    d->len = text->string_len;
    return d;
}

static void close_doc(lsp_server *s, const char *uri)
{
    lsp_document *d = find_doc(s, uri);
    if (!d) return;
    free(d->uri);
    free(d->text);
    *d = s->docs[--s->doc_count];
}

// Runs the analysis on `d` unless it's already the analysed one.
// ---------------------------------------------------------------------------
// Games: the files analysed together

// "file:///D:/a%20b.purr" -> "D:/a b.purr", malloc'd.
static char *uri_to_path(const char *uri)
{
    const char *p = strncmp(uri, "file://", 7) == 0 ? uri + 7 : uri;
    // file:///D:/x and file:///d%3A/x on Windows: drop the slash before the drive.
    if (p[0] == '/' && isalpha((unsigned char)p[1]) && (p[2] == ':' || (p[2] == '%' && p[3] == '3'))) p++;
    char *out = malloc(strlen(p) + 1);
    if (!out) abort();
    size_t n = 0;
    for (; *p; p++) {
        if (p[0] == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            const char hex[3] = {p[1], p[2], 0};
            out[n++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            out[n++] = *p == '\\' ? '/' : *p;
        }
    }
    out[n] = '\0';
    return out;
}

// "D:/a b.purr" -> "file:///D:/a%20b.purr", malloc'd.
static char *path_to_uri(const char *path)
{
    jbuf b = {0};
    jb_put(&b, path[0] == '/' ? "file://" : "file:///");
    for (const char *p = path; *p; p++) {
        if (*p == '\\') jb_put(&b, "/");
        else if (*p == ' ' || *p == '%' || *p == '#' || *p == '?') jb_printf(&b, "%%%02X", (unsigned char)*p);
        else jb_putn(&b, p, 1);
    }
    return b.data;
}

static bool same_path(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a == '\\' ? '/' : *a;
        char y = *b == '\\' ? '/' : *b;
#ifdef _WIN32
        x = (char)tolower((unsigned char)x); // Windows paths ignore case
        y = (char)tolower((unsigned char)y);
#endif
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t)(size > 0 ? size : 0) + 1);
    const size_t n = text ? fread(text, 1, (size_t)(size > 0 ? size : 0), f) : 0;
    fclose(f);
    if (text) text[n] = '\0';
    *len = n;
    return text;
}

// A game's files, without repeats.
typedef struct path_list {
    char **items;
    int count;
} path_list;

static void add_path(void *user, const char *path)
{
    path_list *list = user;
    for (int i = 0; i < list->count; i++)
        if (same_path(list->items[i], path)) return;
    list->items = realloc(list->items, sizeof(char *) * (size_t)(list->count + 1));
    list->items[list->count++] = copy_string(path, strlen(path));
}

static bool is_folder(const char *path)
{
    const size_t n = strlen(path);
    return n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\');
}

// Whether `path` is a .purr file in `folder` (ending in '/') or its subfolders.
static bool in_folder(const char *path, const char *folder)
{
    const size_t n = strlen(folder);
    const size_t len = strlen(path);
    if (len <= n || len < 5 || !same_path(path + len - 5, ".purr")) return false;
    char *start = copy_string(path, n);
    const bool inside = same_path(start, folder);
    free(start);
    return inside;
}

static int compare_paths(const void *a, const void *b)
{
    return path_compare(*(const char *const *)a, *(const char *const *)b);
}

// A line of the manifest: a game and one of its files, or a folder (ending in
// '/') for every .purr file in it and its subfolders, as purr_add_game lists a
// game without SOURCES.
typedef struct manifest_line {
    const char *game;
    const char *path;
} manifest_line;

// The paths of the game `path` belongs to, from the manifest, in the order
// purrc compiles them. Returns how many; 0 if it's in no game. The paths are
// malloc'd.
static int game_of(const lsp_server *s, const char *path, char ***out)
{
    *out = NULL;
    size_t len;
    char *manifest = s->manifest ? read_all(s->manifest, &len) : NULL;
    if (!manifest) return 0;

    manifest_line *lines = NULL;
    int line_count = 0;
    for (char *line = manifest; *line;) {
        char *end = line + strcspn(line, "\n");
        char *next = *end ? end + 1 : end;
        *end = '\0';
        if (end > line && end[-1] == '\r') end[-1] = '\0';
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            lines = realloc(lines, sizeof(manifest_line) * (size_t)(line_count + 1));
            lines[line_count++] = (manifest_line){line, tab + 1};
        }
        line = next;
    }

    const char *game = NULL;
    for (int i = 0; i < line_count && !game; i++) {
        const char *file = lines[i].path;
        if (is_folder(file) ? in_folder(path, file) : same_path(file, path)) game = lines[i].game;
    }

    path_list list = {0};
    for (int i = 0; game && i < line_count; i++) {
        if (strcmp(lines[i].game, game) != 0) continue;
        const char *file = lines[i].path;
        if (!is_folder(file)) {
            add_path(&list, file);
            continue;
        }
        folder_find(file, ".purr", add_path, &list);
        // New files the editor hasn't saved yet
        for (int k = 0; k < s->doc_count; k++) {
            char *open = uri_to_path(s->docs[k].uri);
            if (in_folder(open, file)) add_path(&list, open);
            free(open);
        }
    }
    free(lines);
    free(manifest);
    if (list.count > 0) qsort(list.items, (size_t)list.count, sizeof(char *), compare_paths);
    *out = list.items;
    return list.count;
}

static void free_kept(lsp_server *s)
{
    for (int i = 0; i < s->kept_count; i++) free(s->kept[i]);
    free(s->kept);
    s->kept = NULL;
    s->kept_count = 0;
}

// Analyses the game `d` belongs to, unless the last analysis is still current
// and includes it. Files open in the editor use its text; the others are read
// from disk.
static void analyze(lsp_server *s, const lsp_document *d)
{
    if (s->analyzed && s->analyzed_changes == s->changes && analysis_select(d->uri)) return;

    char *doc_path = uri_to_path(d->uri);
    char **paths;
    const int count = game_of(s, doc_path, &paths);
    free_kept(s);

    const int n = count > 0 ? count : 1;
    analysis_file *files = calloc((size_t)n, sizeof(analysis_file));
    char **uris = calloc((size_t)n, sizeof(char *)); // Made here for files not open
    s->kept = calloc((size_t)n, sizeof(char *));
    for (int i = 0; i < n; i++) {
        const char *path = count > 0 ? paths[i] : doc_path;
        const lsp_document *open = NULL;
        for (int k = 0; k < s->doc_count && !open; k++) {
            char *open_path = uri_to_path(s->docs[k].uri);
            if (same_path(open_path, path)) open = &s->docs[k];
            free(open_path);
        }
        if (open) {
            files[i] = (analysis_file){open->uri, path, open->text, open->len};
        } else {
            size_t len = 0;
            char *text = read_all(path, &len);
            s->kept[s->kept_count++] = text;
            uris[i] = path_to_uri(path);
            files[i] = (analysis_file){uris[i], path, text ? text : "", text ? len : 0};
        }
    }
    analysis_run(files, n);
    analysis_select(d->uri);
    s->analyzed = true;
    s->analyzed_changes = s->changes;

    // The analysis copied the texts; the URIs and paths must outlive it, so
    // they're kept with the disk texts.
    for (int i = 0; i < n; i++) {
        if (uris[i]) {
            s->kept = realloc(s->kept, sizeof(char *) * (size_t)(s->kept_count + 1));
            s->kept[s->kept_count++] = uris[i];
        }
    }
    if (count > 0) {
        s->kept = realloc(s->kept, sizeof(char *) * (size_t)(s->kept_count + (size_t)count + 1));
        for (int i = 0; i < count; i++) s->kept[s->kept_count++] = paths[i];
        free(paths);
    }
    s->kept = realloc(s->kept, sizeof(char *) * (size_t)(s->kept_count + 1));
    s->kept[s->kept_count++] = doc_path;
    free(uris);
    free(files);
}

// ---------------------------------------------------------------------------
// Messages

static void send_buf(lsp_server *s, const jbuf *b)
{
    s->send(s->user, b->data, b->len);
}

static void reply_start(jbuf *b, const json *id)
{
    jb_put(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    jb_json(b, id);
    jb_put(b, ",\"result\":");
}

static void reply_error(lsp_server *s, const json *id, const int code, const char *message)
{
    jbuf b = {0};
    jb_put(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    jb_json(&b, id);
    jb_printf(&b, ",\"error\":{\"code\":%d,\"message\":", code);
    jb_string(&b, message);
    jb_put(&b, "}}");
    send_buf(s, &b);
    jb_free(&b);
}

// Diagnostics for one file: of the analysed game, or none (`file` < 0).
static void publish_diagnostics(lsp_server *s, const char *uri, const int file)
{
    jbuf b = {0};
    jb_put(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    jb_string(&b, uri);
    jb_put(&b, ",\"diagnostics\":");
    if (file < 0) jb_put(&b, "[]");
    else analysis_diagnostics(file, &b);
    jb_put(&b, "}}");
    send_buf(s, &b);
    jb_free(&b);
}

// Every file of the analysed game: an edit in one file can fix or break another.
static void publish_game_diagnostics(lsp_server *s)
{
    for (int i = 0; i < analysis_file_count(); i++) publish_diagnostics(s, analysis_file_uri(i), i);
}

static void initialize(lsp_server *s, const json *id)
{
    jbuf b = {0};
    reply_start(&b, id);
    jb_put(&b, "{\"capabilities\":{"
               "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
               "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
               "\"hoverProvider\":true,"
               "\"definitionProvider\":true,"
               "\"referencesProvider\":true,"
               "\"documentHighlightProvider\":true,"
               "\"renameProvider\":{\"prepareProvider\":true},"
               "\"documentFormattingProvider\":true,"
               "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"],\"retriggerCharacters\":[\",\"]},"
               "\"documentSymbolProvider\":true,"
               "\"semanticTokensProvider\":{\"legend\":");
    analysis_semantic_legend(&b);
    jb_put(&b, ",\"full\":true}},\"serverInfo\":{\"name\":\"purrls\",\"version\":\"0.1\"}}}");
    send_buf(s, &b);
    jb_free(&b);
}

// Requests about one position or one document.
static void document_request(lsp_server *s, const char *method, const json *id, const json *params)
{
    const char *uri = json_str(json_path(params, "textDocument", "uri", NULL));
    const lsp_document *d = uri ? find_doc(s, uri) : NULL;
    if (!d) {
        reply_error(s, id, INVALID_PARAMS, "unknown document");
        return;
    }
    analyze(s, d);
    const int line = json_int(json_path(params, "position", "line", NULL), 0);
    const int character = json_int(json_path(params, "position", "character", NULL), 0);

    jbuf b = {0};
    reply_start(&b, id);
    const size_t result_start = b.len;
    const char *error = NULL;
    if (strcmp(method, "textDocument/completion") == 0) {
        analysis_completion(line, character, &b);
    } else if (strcmp(method, "textDocument/hover") == 0) {
        analysis_hover(line, character, &b);
    } else if (strcmp(method, "textDocument/definition") == 0) {
        analysis_definition(uri, line, character, &b);
    } else if (strcmp(method, "textDocument/references") == 0) {
        const json *declaration = json_path(params, "context", "includeDeclaration", NULL);
        analysis_references(uri, line, character, !declaration || declaration->kind == JSON_TRUE, &b);
    } else if (strcmp(method, "textDocument/documentHighlight") == 0) {
        analysis_highlights(line, character, &b);
    } else if (strcmp(method, "textDocument/signatureHelp") == 0) {
        analysis_signature_help(line, character, &b);
    } else if (strcmp(method, "textDocument/prepareRename") == 0) {
        error = analysis_prepare_rename(line, character, &b);
    } else if (strcmp(method, "textDocument/rename") == 0) {
        const char *new_name = json_str(json_get(params, "newName"));
        error = new_name ? analysis_rename(uri, line, character, new_name, &b) : "No new name given.";
    } else if (strcmp(method, "textDocument/formatting") == 0) {
        const json *spaces = json_path(params, "options", "insertSpaces", NULL);
        error = analysis_format(json_int(json_path(params, "options", "tabSize", NULL), 4),
                                !spaces || spaces->kind != JSON_FALSE, &b);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        analysis_symbols(&b);
    } else {
        analysis_semantic_tokens(&b);
    }
    if (error) {
        reply_error(s, id, REQUEST_FAILED, error);
    } else {
        if (b.len == result_start) jb_put(&b, "null");
        jb_put(&b, "}");
        send_buf(s, &b);
    }
    jb_free(&b);
}

static bool is_document_request(const char *method)
{
    static const char *const methods[] = {
        "textDocument/completion", "textDocument/hover", "textDocument/definition", "textDocument/references",
        "textDocument/documentHighlight", "textDocument/signatureHelp", "textDocument/prepareRename",
        "textDocument/rename", "textDocument/formatting", "textDocument/documentSymbol",
        "textDocument/semanticTokens/full",
    };
    for (size_t i = 0; i < sizeof methods / sizeof methods[0]; i++) {
        if (strcmp(method, methods[i]) == 0) return true;
    }
    return false;
}

void lsp_handle(lsp_server *s, const char *message, const size_t len)
{
    const json *msg = json_parse(message, len);
    if (!msg) {
        json null_id = {.kind = JSON_NULL};
        reply_error(s, &null_id, PARSE_ERROR, "invalid JSON");
        json_release();
        return;
    }
    const char *method = json_str(json_get(msg, "method"));
    const json *id = json_get(msg, "id");
    const json *params = json_get(msg, "params");

    if (!method) {
        // A response to a request we never make.
    } else if (strcmp(method, "initialize") == 0) {
        initialize(s, id);
    } else if (strcmp(method, "shutdown") == 0) {
        s->shutdown = true;
        jbuf b = {0};
        reply_start(&b, id);
        jb_put(&b, "null}");
        send_buf(s, &b);
        jb_free(&b);
    } else if (strcmp(method, "exit") == 0) {
        s->exited = true;
        s->exit_code = s->shutdown ? 0 : 1;
    } else if (strcmp(method, "textDocument/didOpen") == 0 || strcmp(method, "textDocument/didChange") == 0) {
        const char *uri = json_str(json_path(params, "textDocument", "uri", NULL));
        const json *text = json_path(params, "textDocument", "text", NULL);
        const json *changes = json_path(params, "contentChanges", NULL);
        if (!text && changes && changes->kind == JSON_ARRAY && changes->count > 0) {
            text = json_get(changes->items[changes->count - 1], "text"); // Full sync: the last change is the whole text
        }
        if (uri && text && text->kind == JSON_STRING) {
            const lsp_document *d = set_doc(s, uri, text);
            s->changes++;
            analyze(s, d);
            publish_game_diagnostics(s);
        }
    } else if (strcmp(method, "textDocument/didClose") == 0) {
        const char *uri = json_str(json_path(params, "textDocument", "uri", NULL));
        if (uri) {
            publish_diagnostics(s, uri, -1);
            close_doc(s, uri);
            s->changes++;
            s->analyzed = false; // The analysis may point at the closed document's text
        }
    } else if (is_document_request(method)) {
        document_request(s, method, id, params);
    } else if (id) {
        reply_error(s, id, METHOD_NOT_FOUND, method);
    }
    // Other notifications (initialized, didSave, $/cancelRequest...) need nothing.

    json_release();
}
