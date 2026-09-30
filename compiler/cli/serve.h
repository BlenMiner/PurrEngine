#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A small web server on this machine only (127.0.0.1), for purr run --web: it
// answers GET requests with what `answer` gives, and nothing else.

typedef struct serve_reply {
    int status;       // 200, or 404
    const char *type; // Its Content-Type
    const void *body;
    size_t size;
} serve_reply;

// Fills `reply` for a request of `path`, like "/" or "/build".
typedef void (*serve_fn)(void *user, const char *path, serve_reply *reply);

typedef struct serve serve;

// Listens on a free port of 127.0.0.1, which goes in `port`. NULL if it can't.
serve *serve_open(uint16_t *port);

// Waits up to `ms` milliseconds for requests, and answers each one that's
// whole.
void serve_poll(serve *s, int ms, serve_fn answer, void *user);
