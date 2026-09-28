#pragma once

#include <stdbool.h>
#include <stddef.h>

// The language server protocol, minus the transport: lsp_handle takes one
// JSON-RPC message and sends replies and notifications through `send`.

typedef struct lsp_document {
    char *uri;
    char *text;
    size_t len;
} lsp_document;

typedef void (*lsp_send_fn)(void *user, const char *message, size_t len);

typedef struct lsp_server {
    lsp_send_fn send;
    void *user;
    // Which files make up each game: lines of "<game>\t<path>", written by
    // purr_add_game. Open folders can have their own (see game_of). May be NULL.
    const char *manifest;
    char **roots; // The folders open in the editor, ending in '/'
    int root_count;
    lsp_document *docs;
    int doc_count;
    int doc_cap;
    unsigned changes;           // Bumped whenever a document changes
    unsigned analyzed_changes;  // `changes` when the last analysis ran
    bool analyzed;
    char **kept;                // Strings the analysis points to: texts read from disk, URIs, paths
    int kept_count;
    bool shutdown;
    bool exited;
    int exit_code;
} lsp_server;

void lsp_init(lsp_server *s, lsp_send_fn send, void *user);
void lsp_handle(lsp_server *s, const char *message, size_t len);
void lsp_free(lsp_server *s);
