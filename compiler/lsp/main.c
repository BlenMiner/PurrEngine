// tidels: the Tide language server. Editors start it and talk to it over
// stdin and stdout (LSP's base protocol: a Content-Length header, then JSON).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "cdefs.h"
#include "folders.h"
#include "server.h"

// Set by the build: the list of games and their files.
#ifndef TIDE_GAMES_MANIFEST
#define TIDE_GAMES_MANIFEST NULL
#endif

static void send_message(void *user, const char *message, const size_t len)
{
    (void)user;
    fprintf(stdout, "Content-Length: %zu\r\n\r\n", len);
    fwrite(message, 1, len, stdout);
    fflush(stdout);
}

// Reads one message's headers. Returns its Content-Length, or -1 at the end of input.
static long read_headers(void)
{
    long length = -1;
    char line[256];
    for (;;) {
        if (!fgets(line, sizeof line, stdin)) return -1;
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) return length;
        if (strncmp(line, "Content-Length:", 15) == 0) length = strtol(line + 15, NULL, 10);
    }
}

// Installed, tidels is in <install>/bin and the engine headers are in
// <install>/include. NULL if they aren't there, as in a build of the repo.
static char *installed_include_dir(void)
{
    char *bin = folder_of_program();
    if (!bin) return NULL;
    // "<install>/bin/" -> "<install>"
    char *last = strrchr(bin, '/');
    *last = '\0';
    last = strrchr(bin, '/');
    if (!last) {
        free(bin);
        return NULL;
    }
    *last = '\0';
    const size_t len = strlen(bin) + 32;
    char *dir = malloc(len);
    char *probe = malloc(len);
    if (!dir || !probe) abort();
    snprintf(dir, len, "%s/include", bin);
    snprintf(probe, len, "%s/tide/math.h", dir);
    free(bin);
    FILE *f = fopen(probe, "rb");
    free(probe);
    if (!f) {
        free(dir);
        return NULL;
    }
    fclose(f);
    return dir;
}

int main(void)
{
#ifdef _WIN32
    // Content-Length counts bytes: no \n to \r\n translation.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    lsp_server server;
    lsp_init(&server, send_message, NULL);
    server.manifest = TIDE_GAMES_MANIFEST; // Written by tide_add_game (see cmake/Tide.cmake)
    char *include_dir = installed_include_dir();
    if (include_dir) cdefs_set_include_dir(include_dir);

    char *body = NULL;
    size_t cap = 0;
    while (!server.exited) {
        const long length = read_headers();
        if (length < 0) break;
        if ((size_t)length + 1 > cap) {
            cap = (size_t)length + 1;
            char *grown = realloc(body, cap);
            if (!grown) return 1;
            body = grown;
        }
        if (fread(body, 1, (size_t)length, stdin) != (size_t)length) break;
        body[length] = '\0';
        lsp_handle(&server, body, (size_t)length);
    }

    free(body);
    lsp_free(&server);
    free(include_dir);
    return server.exited ? server.exit_code : 1;
}
