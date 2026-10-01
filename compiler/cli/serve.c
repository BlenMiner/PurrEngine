#ifndef _WIN32
#define _DEFAULT_SOURCE // Sockets under strict C
#endif

#include "serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock;
#define NO_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock;
#define NO_SOCKET (-1)
#define close_socket close
#endif

// See serve.h. Connections are answered one request each, then closed; ones
// that don't send a whole request are closed when there's no room for more,
// the oldest first.

#define CONNECTIONS 16
#define REQUEST_BYTES 4096

typedef struct connection {
    sock s;
    uint64_t opened; // Its place among the connections, to close the oldest
    uint32_t got;
    char request[REQUEST_BYTES];
} connection;

struct serve {
    sock listener;
    connection connections[CONNECTIONS];
    uint64_t opened;
};

static void set_blocking(const sock s, const bool blocking)
{
#ifdef _WIN32
    u_long nonblocking = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK);
#endif
}

serve *serve_open(uint16_t *port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif
    const sock listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == NO_SOCKET) return NULL;
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // This machine only
    address.sin_port = 0;                             // Any free port
    socklen_t length = sizeof address;
    if (bind(listener, (struct sockaddr *)&address, sizeof address) != 0 || listen(listener, 16) != 0
        || getsockname(listener, (struct sockaddr *)&address, &length) != 0) {
        close_socket(listener);
        return NULL;
    }
    set_blocking(listener, false);
    serve *s = calloc(1, sizeof *s);
    if (!s) abort();
    s->listener = listener;
    for (int i = 0; i < CONNECTIONS; i++) s->connections[i].s = NO_SOCKET;
    *port = ntohs(address.sin_port);
    return s;
}

static void close_connection(connection *c)
{
    close_socket(c->s);
    c->s = NO_SOCKET;
    c->got = 0;
}

static bool send_all(const sock s, const char *data, size_t size)
{
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL; // A page that went away mustn't end tide
#else
    const int flags = 0;
#endif
    while (size > 0) {
        const int chunk = size > (1u << 20) ? (1 << 20) : (int)size;
        const int sent = (int)send(s, data, chunk, flags);
        if (sent <= 0) return false;
        data += sent;
        size -= (size_t)sent;
    }
    return true;
}

static void reply(connection *c, const serve_fn answer, void *user)
{
    char method[8] = "";
    char path[256] = "";
    serve_reply r = {404, "text/plain; charset=utf-8", "Not found\n", 10};
    if (sscanf(c->request, "%7s %255s", method, path) == 2 && strcmp(method, "GET") == 0) {
        char *query = strchr(path, '?');
        if (query) *query = '\0';
        answer(user, path, &r);
    }
    // The page is cross-origin isolated, so the game can share its memory with
    // workers and run ticks on threads (platform/web/tide.js), and editors
    // can still show it in a frame of theirs.
    char header[640];
    const int n = snprintf(header, sizeof header,
                           "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                           "Cross-Origin-Opener-Policy: same-origin\r\nCross-Origin-Embedder-Policy: require-corp\r\n"
                           "Cross-Origin-Resource-Policy: cross-origin\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                           r.status, r.status == 200 ? "OK" : "Not Found", r.type, (unsigned long)r.size);
    set_blocking(c->s, true);
    if (send_all(c->s, header, (size_t)n)) send_all(c->s, r.body, r.size);
    close_connection(c);
}

// Takes a connection waiting to be accepted, closing the oldest one when
// there's no room.
static void accept_one(serve *s)
{
    const sock accepted = accept(s->listener, NULL, NULL);
    if (accepted == NO_SOCKET) return;
    connection *slot = NULL;
    for (int i = 0; i < CONNECTIONS; i++) {
        connection *c = &s->connections[i];
        if (c->s == NO_SOCKET) {
            slot = c;
            break;
        }
        if (!slot || c->opened < slot->opened) slot = c;
    }
    if (slot->s != NO_SOCKET) close_connection(slot);
    set_blocking(accepted, false);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(accepted, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    slot->s = accepted;
    slot->opened = ++s->opened;
    slot->got = 0;
}

void serve_poll(serve *s, const int ms, const serve_fn answer, void *user)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s->listener, &readable);
    sock highest = s->listener;
    for (int i = 0; i < CONNECTIONS; i++) {
        const sock c = s->connections[i].s;
        if (c == NO_SOCKET) continue;
        FD_SET(c, &readable);
        if (c > highest) highest = c;
    }
    struct timeval wait = {ms / 1000, (ms % 1000) * 1000};
    if (select((int)highest + 1, &readable, NULL, NULL, &wait) <= 0) return;
    for (int i = 0; i < CONNECTIONS; i++) {
        connection *c = &s->connections[i];
        if (c->s == NO_SOCKET || !FD_ISSET(c->s, &readable)) continue;
        const int got = (int)recv(c->s, c->request + c->got, REQUEST_BYTES - 1u - c->got, 0);
        if (got <= 0) {
            close_connection(c);
            continue;
        }
        c->got += (uint32_t)got;
        c->request[c->got] = '\0';
        if (strstr(c->request, "\r\n\r\n")) reply(c, answer, user);
        else if (c->got >= REQUEST_BYTES - 1u) close_connection(c); // Too long for us
    }
    if (FD_ISSET(s->listener, &readable)) accept_one(s);
}
