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

// The id of our one request of the editor: to watch files for us.
#define WATCH_REQUEST "tidels-watch"

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
    for (int i = 0; i < s->root_count; i++) free(s->roots[i]);
    free(s->roots);
    s->roots = NULL;
    s->root_count = 0;
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

// "file:///D:/a%20b.tide" -> "D:/a b.tide", malloc'd.
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

// "D:/a b.tide" -> "file:///D:/a%20b.tide", malloc'd.
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

// Whether `path` is in `folder` (ending in '/') or its subfolders.
static bool under(const char *path, const char *folder)
{
    const size_t n = strlen(folder);
    if (strlen(path) <= n) return false;
    char *start = copy_string(path, n);
    const bool inside = same_path(start, folder);
    free(start);
    return inside;
}

// Whether `path` is a .tide file in `folder` (ending in '/') or its subfolders.
static bool in_folder(const char *path, const char *folder)
{
    const size_t len = strlen(path);
    return len >= 5 && same_path(path + len - 5, ".tide") && under(path, folder);
}

static int compare_paths(const void *a, const void *b)
{
    return path_compare(*(const char *const *)a, *(const char *const *)b);
}

// Every .tide file in `folder` and its subfolders, with new files the editor
// hasn't saved yet.
static void add_folder(const lsp_server *s, path_list *list, const char *folder)
{
    folder_find(folder, ".tide", add_path, list);
    for (int k = 0; k < s->doc_count; k++) {
        char *open = uri_to_path(s->docs[k].uri);
        if (in_folder(open, folder)) add_path(list, open);
        free(open);
    }
}

// A line of a manifest: a game and one of its files, or a folder (ending in
// '/') for every .tide file in it and its subfolders, as tide_add_game lists a
// game without SOURCES. Games of different manifests are different games.
typedef struct manifest_line {
    int manifest;
    const char *game;
    const char *path;
} manifest_line;

typedef struct manifests {
    char **sources; // Their paths
    char **texts;
    int count;
    manifest_line *lines;
    int line_count;
} manifests;

static void read_manifest(manifests *m, const char *source)
{
    for (int i = 0; i < m->count; i++)
        if (same_path(m->sources[i], source)) return;
    size_t len;
    char *text = read_all(source, &len);
    if (!text) return;
    m->sources = realloc(m->sources, sizeof(char *) * (size_t)(m->count + 1));
    m->texts = realloc(m->texts, sizeof(char *) * (size_t)(m->count + 1));
    m->sources[m->count] = copy_string(source, strlen(source));
    m->texts[m->count] = text;
    for (char *line = text; *line;) {
        char *end = line + strcspn(line, "\n");
        char *next = *end ? end + 1 : end;
        *end = '\0';
        if (end > line && end[-1] == '\r') end[-1] = '\0';
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            m->lines = realloc(m->lines, sizeof(manifest_line) * (size_t)(m->line_count + 1));
            m->lines[m->line_count++] = (manifest_line){m->count, line, tab + 1};
        }
        line = next;
    }
    m->count++;
}

static void free_manifests(manifests *m)
{
    for (int i = 0; i < m->count; i++) {
        free(m->sources[i]);
        free(m->texts[i]);
    }
    free(m->sources);
    free(m->texts);
    free(m->lines);
}

// The paths of the game `path` belongs to, in the order tidec compiles them.
// Returns how many; 0 if it's in no game. The paths are malloc'd.
//
// Games come from manifests: the one this server was built with, and the one
// in any open folder that builds games with CMake, where tide_add_game writes
// it. A file in none of them belongs to the open folder it's in, since that's
// the game `tide run` builds there; unless a manifest lists games in that
// folder, whose other files stand alone (tests, for example).
static int game_of(const lsp_server *s, const char *path, char ***out)
{
    *out = NULL;
    manifests m = {0};
    for (int i = 0; i < s->root_count; i++) {
        jbuf source = {0};
        jb_printf(&source, "%sbuild/tools/games.txt", s->roots[i]);
        read_manifest(&m, source.data);
        jb_free(&source);
    }
    if (s->manifest) read_manifest(&m, s->manifest);

    const manifest_line *game = NULL;
    for (int i = 0; i < m.line_count && !game; i++) {
        const char *file = m.lines[i].path;
        if (is_folder(file) ? in_folder(path, file) : same_path(file, path)) game = &m.lines[i];
    }

    path_list list = {0};
    if (game) {
        for (int i = 0; i < m.line_count; i++) {
            const manifest_line *line = &m.lines[i];
            if (line->manifest != game->manifest || strcmp(line->game, game->game) != 0) continue;
            if (is_folder(line->path)) add_folder(s, &list, line->path);
            else add_path(&list, line->path);
        }
    } else {
        const char *root = NULL; // The innermost open folder it's in
        for (int i = 0; i < s->root_count; i++) {
            if (in_folder(path, s->roots[i]) && (!root || strlen(s->roots[i]) > strlen(root))) root = s->roots[i];
        }
        for (int i = 0; root && i < m.line_count; i++) {
            if (under(m.lines[i].path, root)) root = NULL;
        }
        if (root) add_folder(s, &list, root);
    }
    free_manifests(&m);
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

static void keep(lsp_server *s, char *text)
{
    s->kept = realloc(s->kept, sizeof(char *) * (size_t)(s->kept_count + 1));
    if (!s->kept) abort();
    s->kept[s->kept_count++] = text;
}

// Analyses the files at `paths` (`count` of them, malloc'd, as game_of gives
// them) together, or with none, the file at `alone` by itself. Files open in
// the editor use its text; the others are read from disk. The analysis points
// to the paths, so they're kept until the next one, with `alone` (or NULL).
static void run_game(lsp_server *s, char **paths, const int count, char *alone)
{
    free_kept(s);
    const int n = count > 0 ? count : 1;
    analysis_file *files = calloc((size_t)n, sizeof(analysis_file));
    char **uris = calloc((size_t)n, sizeof(char *)); // Made here for files not open
    for (int i = 0; i < n; i++) {
        const char *path = count > 0 ? paths[i] : alone;
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
            keep(s, text);
            uris[i] = path_to_uri(path);
            files[i] = (analysis_file){uris[i], path, text ? text : "", text ? len : 0};
        }
    }
    analysis_run(files, n);
    s->analyzed = true;
    s->analyzed_changes = s->changes;

    // The analysis copied the texts; the URIs and paths must outlive it, so
    // they're kept with the disk texts.
    for (int i = 0; i < n; i++) {
        if (uris[i]) keep(s, uris[i]);
    }
    for (int i = 0; i < count; i++) keep(s, paths[i]);
    free(paths);
    if (alone) keep(s, alone);
    free(uris);
    free(files);
}

// Selects the analysed file at `uri`. Paths decide when URIs don't match: the
// editor and this server may spell one file's URI differently.
static bool select_uri(const char *uri)
{
    if (analysis_select(uri)) return true;
    char *path = uri_to_path(uri);
    bool found = false;
    for (int i = 0; i < analysis_file_count() && !found; i++) {
        found = same_path(analysis_file_path(i), path) && analysis_select(analysis_file_uri(i));
    }
    free(path);
    return found;
}

// Analyses the game of the file at `uri`, open in the editor or not, unless
// the last analysis is still current and includes it, and selects the file.
static void analyze(lsp_server *s, const char *uri)
{
    if (s->analyzed && s->analyzed_changes == s->changes && select_uri(uri)) return;
    char *path = uri_to_path(uri);
    char **paths;
    const int count = game_of(s, path, &paths);
    run_game(s, paths, count, path);
    select_uri(uri);
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

// A folder's path from its URI, ending in '/' as roots do: malloc'd, or NULL.
static char *folder_path(const char *uri)
{
    char *path = uri_to_path(uri);
    const size_t n = strlen(path);
    if (n == 0) {
        free(path);
        return NULL;
    }
    if (path[n - 1] != '/') {
        path = realloc(path, n + 2);
        if (!path) abort();
        path[n] = '/';
        path[n + 1] = '\0';
    }
    return path;
}

static void add_root(lsp_server *s, const char *uri)
{
    char *path = folder_path(uri);
    for (int i = 0; path && i < s->root_count; i++) {
        if (same_path(s->roots[i], path)) {
            free(path);
            return;
        }
    }
    if (!path) return;
    s->roots = realloc(s->roots, sizeof(char *) * (size_t)(s->root_count + 1));
    if (!s->roots) abort();
    s->roots[s->root_count++] = path;
}

static void remove_root(lsp_server *s, const char *uri)
{
    char *path = folder_path(uri);
    for (int i = 0; path && i < s->root_count; i++) {
        if (!same_path(s->roots[i], path)) continue;
        free(s->roots[i]);
        s->roots[i--] = s->roots[--s->root_count];
    }
    free(path);
}

// Analyses the games of the documents open in the editor again, and publishes
// their diagnostics: what's on disk, or which files make up each game,
// changed. Each game once.
static void refresh(lsp_server *s)
{
    s->changes++;
    bool *done = calloc((size_t)s->doc_count + 1, sizeof(bool));
    for (int i = 0; i < s->doc_count; i++) {
        if (done[i]) continue;
        analyze(s, s->docs[i].uri);
        publish_game_diagnostics(s);
        for (int k = i; k < s->doc_count; k++) done[k] = done[k] || select_uri(s->docs[k].uri);
    }
    free(done);
}

// Asks the editor to say when .tide files and the games' lists change on disk,
// whether the editor changed them or something else did: a build, git, another
// program. Those lists are build/tools/games.txt in an open folder, and the one
// this server was built with, which may be outside them.
static void register_watchers(lsp_server *s)
{
    jbuf b = {0};
    jb_put(&b, "{\"jsonrpc\":\"2.0\",\"id\":\"" WATCH_REQUEST "\",\"method\":\"client/registerCapability\","
               "\"params\":{\"registrations\":[{\"id\":\"tide-files\",\"method\":\"workspace/didChangeWatchedFiles\","
               "\"registerOptions\":{\"watchers\":[{\"globPattern\":\"**/*.tide\"},"
               "{\"globPattern\":\"**/build/tools/games.txt\"}");
    bool outside = s->manifest != NULL;
    for (int i = 0; outside && i < s->root_count; i++) outside = !under(s->manifest, s->roots[i]);
    const char *slash = outside ? strrchr(s->manifest, '/') : NULL;
    if (slash && s->relative_patterns) {
        char *folder = copy_string(s->manifest, (size_t)(slash - s->manifest));
        char *uri = path_to_uri(folder);
        jb_put(&b, ",{\"globPattern\":{\"baseUri\":");
        jb_string(&b, uri);
        jb_put(&b, ",\"pattern\":");
        jb_string(&b, slash + 1);
        jb_put(&b, "}}");
        free(uri);
        free(folder);
    } else if (outside) {
        jb_put(&b, ",{\"globPattern\":");
        jb_string(&b, s->manifest);
        jb_put(&b, "}");
    }
    jb_put(&b, "]}}]}}");
    send_buf(s, &b);
    jb_free(&b);
}

// Whether the editor applies edits that create files.
static bool client_creates_files(const json *params)
{
    const json *edit = json_path(params, "capabilities", "workspace", "workspaceEdit", NULL);
    const json *changes = json_get(edit, "documentChanges");
    const json *operations = json_get(edit, "resourceOperations");
    if (!changes || changes->kind != JSON_TRUE || !operations || operations->kind != JSON_ARRAY) return false;
    for (int i = 0; i < operations->count; i++) {
        const char *op = json_str(operations->items[i]);
        if (op && strcmp(op, "create") == 0) return true;
    }
    return false;
}

static void initialize(lsp_server *s, const json *id, const json *params)
{
    analysis_set_can_create_files(client_creates_files(params));
    const json *related = json_path(params, "capabilities", "textDocument", "publishDiagnostics", "relatedInformation", NULL);
    analysis_set_related_information(related && related->kind == JSON_TRUE);
    const json *watch = json_path(params, "capabilities", "workspace", "didChangeWatchedFiles", NULL);
    const json *dynamic = json_get(watch, "dynamicRegistration");
    const json *relative = json_get(watch, "relativePatternSupport");
    s->watch_files = dynamic && dynamic->kind == JSON_TRUE;
    s->relative_patterns = relative && relative->kind == JSON_TRUE;
    const json *folders = json_get(params, "workspaceFolders");
    if (folders && folders->kind == JSON_ARRAY && folders->count > 0) {
        for (int i = 0; i < folders->count; i++) {
            const char *uri = json_str(json_get(folders->items[i], "uri"));
            if (uri) add_root(s, uri);
        }
    } else if (json_str(json_get(params, "rootUri"))) {
        add_root(s, json_str(json_get(params, "rootUri")));
    }

    jbuf b = {0};
    reply_start(&b, id);
    jb_put(&b, "{\"capabilities\":{"
               "\"textDocumentSync\":{\"openClose\":true,\"change\":1,\"save\":{\"includeText\":false}},"
               "\"workspace\":{\"workspaceFolders\":{\"supported\":true,\"changeNotifications\":true}},"
               "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
               "\"hoverProvider\":true,"
               "\"definitionProvider\":true,"
               "\"typeDefinitionProvider\":true,"
               "\"implementationProvider\":true,"
               "\"callHierarchyProvider\":true,"
               "\"referencesProvider\":true,"
               "\"documentHighlightProvider\":true,"
               "\"renameProvider\":{\"prepareProvider\":true},"
               "\"documentFormattingProvider\":true,"
               "\"documentRangeFormattingProvider\":true,"
               "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"],\"retriggerCharacters\":[\",\"]},"
               "\"documentSymbolProvider\":true,"
               "\"workspaceSymbolProvider\":true,"
               "\"inlayHintProvider\":true,"
               "\"foldingRangeProvider\":true,"
               "\"selectionRangeProvider\":true,"
               "\"codeLensProvider\":{},"
               "\"codeActionProvider\":{\"codeActionKinds\":[\"quickfix\",\"refactor.move\"]},"
               "\"semanticTokensProvider\":{\"legend\":");
    analysis_semantic_legend(&b);
    jb_put(&b, ",\"full\":true}},\"serverInfo\":{\"name\":\"tidels\",\"version\":\"0.1\"}}}");
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
    analyze(s, d->uri);
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
    } else if (strcmp(method, "textDocument/typeDefinition") == 0) {
        analysis_type_definition(line, character, &b);
    } else if (strcmp(method, "textDocument/implementation") == 0) {
        analysis_implementation(line, character, &b);
    } else if (strcmp(method, "textDocument/prepareCallHierarchy") == 0) {
        analysis_prepare_call_hierarchy(line, character, &b);
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
    } else if (strcmp(method, "textDocument/formatting") == 0 || strcmp(method, "textDocument/rangeFormatting") == 0) {
        const json *spaces = json_path(params, "options", "insertSpaces", NULL);
        const json *range = json_get(params, "range");
        int first = 0;
        int last = 1 << 30;
        if (range) { // A range ending at a line's start leaves that line out
            first = json_int(json_path(range, "start", "line", NULL), 0);
            last = json_int(json_path(range, "end", "line", NULL), 0);
            if (last > first && json_int(json_path(range, "end", "character", NULL), 0) == 0) last--;
        }
        error = analysis_format(json_int(json_path(params, "options", "tabSize", NULL), 4),
                                !spaces || spaces->kind != JSON_FALSE, first, last, &b);
    } else if (strcmp(method, "textDocument/documentSymbol") == 0) {
        analysis_symbols(&b);
    } else if (strcmp(method, "textDocument/codeLens") == 0) {
        analysis_code_lenses(&b);
    } else if (strcmp(method, "textDocument/foldingRange") == 0) {
        analysis_folding_ranges(&b);
    } else if (strcmp(method, "textDocument/selectionRange") == 0) {
        analysis_selection_ranges(json_get(params, "positions"), &b);
    } else if (strcmp(method, "textDocument/inlayHint") == 0) {
        analysis_inlay_hints(json_int(json_path(params, "range", "start", "line", NULL), 0),
                             json_int(json_path(params, "range", "end", "line", NULL), 0), &b);
    } else if (strcmp(method, "textDocument/codeAction") == 0) {
        analysis_code_actions(json_int(json_path(params, "range", "start", "line", NULL), 0),
                              json_int(json_path(params, "range", "end", "line", NULL), 0), &b);
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

// workspace/symbol: the declarations of every game, each searched once.
typedef struct symbol_search {
    const char *query;
    jbuf *out;
    int written;
    path_list seen; // Files searched already, with their games
} symbol_search;

// Searches a game's files (`paths`, or `alone`, as run_game takes them),
// unless they were all searched already.
static void search_game(lsp_server *s, symbol_search *search, char **paths, const int count, char *alone)
{
    bool fresh = false;
    for (int i = 0; i < (count > 0 ? count : 1); i++) {
        const char *path = count > 0 ? paths[i] : alone;
        bool seen = false;
        for (int k = 0; k < search->seen.count && !seen; k++) seen = same_path(search->seen.items[k], path);
        fresh |= !seen;
        if (!seen) add_path(&search->seen, path);
    }
    if (!fresh) {
        for (int i = 0; i < count; i++) free(paths[i]);
        free(paths);
        free(alone);
        return;
    }
    run_game(s, paths, count, alone);
    analysis_workspace_symbols(search->query, search->out, &search->written);
}

// Every game in the open folders: those their manifests list, and an open
// folder that's a game itself; and the games of the documents open in the
// editor, wherever they are.
static void workspace_symbols(lsp_server *s, const json *id, const char *query)
{
    jbuf b = {0};
    reply_start(&b, id);
    jb_put(&b, "[");
    symbol_search search = {query, &b, 0, {0}};

    manifests m = {0};
    for (int i = 0; i < s->root_count; i++) {
        jbuf source = {0};
        jb_printf(&source, "%sbuild/tools/games.txt", s->roots[i]);
        read_manifest(&m, source.data);
        jb_free(&source);
    }
    if (s->manifest) read_manifest(&m, s->manifest);
    for (int i = 0; i < m.line_count; i++) {
        const manifest_line *game = &m.lines[i];
        bool first = true; // Each game at its first line
        for (int k = 0; k < i && first; k++) first = m.lines[k].manifest != game->manifest || strcmp(m.lines[k].game, game->game) != 0;
        bool open = false;
        for (int k = i; k < m.line_count && first && !open; k++) {
            const manifest_line *line = &m.lines[k];
            if (line->manifest != game->manifest || strcmp(line->game, game->game) != 0) continue;
            for (int r = 0; r < s->root_count && !open; r++) open = under(line->path, s->roots[r]);
        }
        if (!open) continue;
        path_list list = {0};
        for (int k = i; k < m.line_count; k++) {
            const manifest_line *line = &m.lines[k];
            if (line->manifest != game->manifest || strcmp(line->game, game->game) != 0) continue;
            if (is_folder(line->path)) add_folder(s, &list, line->path);
            else add_path(&list, line->path);
        }
        if (list.count > 0) qsort(list.items, (size_t)list.count, sizeof(char *), compare_paths);
        if (list.count > 0) search_game(s, &search, list.items, list.count, NULL);
        else free(list.items);
    }
    for (int r = 0; r < s->root_count; r++) {
        bool listed = false;
        for (int i = 0; i < m.line_count && !listed; i++) listed = under(m.lines[i].path, s->roots[r]);
        if (listed) continue;
        path_list list = {0};
        add_folder(s, &list, s->roots[r]);
        if (list.count > 0) qsort(list.items, (size_t)list.count, sizeof(char *), compare_paths);
        if (list.count > 0) search_game(s, &search, list.items, list.count, NULL);
        else free(list.items);
    }
    free_manifests(&m);
    for (int i = 0; i < s->doc_count; i++) {
        char *path = uri_to_path(s->docs[i].uri);
        char **paths;
        const int count = game_of(s, path, &paths);
        search_game(s, &search, paths, count, path);
    }

    for (int i = 0; i < search.seen.count; i++) free(search.seen.items[i]);
    free(search.seen.items);
    jb_put(&b, "]}");
    send_buf(s, &b);
    jb_free(&b);
}

static bool is_document_request(const char *method)
{
    static const char *const methods[] = {
        "textDocument/completion", "textDocument/hover", "textDocument/definition", "textDocument/references",
        "textDocument/documentHighlight", "textDocument/signatureHelp", "textDocument/prepareRename",
        "textDocument/rename", "textDocument/formatting", "textDocument/documentSymbol",
        "textDocument/codeLens", "textDocument/codeAction", "textDocument/inlayHint", "textDocument/foldingRange",
        "textDocument/semanticTokens/full", "textDocument/typeDefinition", "textDocument/implementation",
        "textDocument/prepareCallHierarchy", "textDocument/rangeFormatting", "textDocument/selectionRange",
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
        // A response to our request: the editor watches the files, or it
        // can't and says so. Nothing waits for it either way.
    } else if (strcmp(method, "initialize") == 0) {
        initialize(s, id, params);
    } else if (strcmp(method, "initialized") == 0) {
        if (s->watch_files) register_watchers(s);
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
            analyze(s, d->uri);
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
    } else if (strcmp(method, "textDocument/didSave") == 0) {
        // The editor's text is already ours, but saving is when other files
        // tend to change on disk too, for editors that watch none for us.
        const char *uri = json_str(json_path(params, "textDocument", "uri", NULL));
        const json *text = json_get(params, "text");
        if (uri && text && text->kind == JSON_STRING && find_doc(s, uri)) set_doc(s, uri, text);
        refresh(s);
    } else if (strcmp(method, "workspace/didChangeWatchedFiles") == 0) {
        // A deleted file's diagnostics go with it, unless the editor still has it open.
        const json *changes = json_get(params, "changes");
        for (int i = 0; changes && changes->kind == JSON_ARRAY && i < changes->count; i++) {
            const char *uri = json_str(json_get(changes->items[i], "uri"));
            if (!uri || json_int(json_get(changes->items[i], "type"), 0) != 3 || find_doc(s, uri)) continue;
            publish_diagnostics(s, uri, -1);
            char *path = uri_to_path(uri);
            char *ours = path_to_uri(path); // As this server spells it, if the editor spells it otherwise
            if (strcmp(ours, uri) != 0) publish_diagnostics(s, ours, -1);
            free(ours);
            free(path);
        }
        refresh(s);
    } else if (strcmp(method, "workspace/didChangeWorkspaceFolders") == 0) {
        const json *added = json_path(params, "event", "added", NULL);
        const json *removed = json_path(params, "event", "removed", NULL);
        for (int i = 0; removed && removed->kind == JSON_ARRAY && i < removed->count; i++) {
            const char *uri = json_str(json_get(removed->items[i], "uri"));
            if (uri) remove_root(s, uri);
        }
        for (int i = 0; added && added->kind == JSON_ARRAY && i < added->count; i++) {
            const char *uri = json_str(json_get(added->items[i], "uri"));
            if (uri) add_root(s, uri);
        }
        refresh(s); // Which files make up a game depends on the open folders
    } else if (is_document_request(method)) {
        document_request(s, method, id, params);
    } else if (strcmp(method, "workspace/symbol") == 0) {
        const char *query = json_str(json_get(params, "query"));
        workspace_symbols(s, id, query ? query : "");
    } else if (strcmp(method, "callHierarchy/incomingCalls") == 0 || strcmp(method, "callHierarchy/outgoingCalls") == 0) {
        // About an item, at its name, in a file the editor may not have open
        const char *uri = json_str(json_path(params, "item", "uri", NULL));
        const int line = json_int(json_path(params, "item", "selectionRange", "start", "line", NULL), 0);
        const int character = json_int(json_path(params, "item", "selectionRange", "start", "character", NULL), 0);
        jbuf b = {0};
        reply_start(&b, id);
        if (!uri) jb_put(&b, "[]");
        else analyze(s, uri);
        if (uri && strcmp(method, "callHierarchy/incomingCalls") == 0) analysis_incoming_calls(line, character, &b);
        else if (uri) analysis_outgoing_calls(line, character, &b);
        jb_put(&b, "}");
        send_buf(s, &b);
        jb_free(&b);
    } else if (id) {
        reply_error(s, id, METHOD_NOT_FOUND, method);
    }
    // Other notifications ($/cancelRequest, $/setTrace...) need nothing.

    json_release();
}
