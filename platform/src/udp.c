// UDP for sessions: purr_platform_udp_open and purr_platform_resolve (see
// purr/platform.h). A file of its own, since it includes the operating
// system's headers, which raylib's clash with.

#if !defined(_WIN32) && !defined(__wasi__)
#define _DEFAULT_SOURCE // getaddrinfo and struct addrinfo under strict C
#endif

#include "purr/platform.h"

#include <stdlib.h>
#include <string.h>

#if defined(__wasi__)

// The web has no UDP; a browser will reach servers another way.
bool purr_platform_udp_open(const uint16_t port, purr_transport *out)
{
    (void)port;
    (void)out;
    return false;
}

bool purr_platform_udp_open_local(const uint16_t port, purr_transport *out)
{
    return purr_platform_udp_open(port, out);
}

bool purr_platform_resolve(const char *text, const uint16_t default_port, purr_address *out)
{
    return purr_address_parse(text, default_port, out);
}

#else

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET udp_socket;
#define BAD_SOCKET INVALID_SOCKET
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int udp_socket;
#define BAD_SOCKET (-1)
#define close_socket close
#endif

typedef struct udp {
    udp_socket socket;
} udp;

static bool started(void)
{
#ifdef _WIN32
    static bool ready;
    if (!ready) {
        WSADATA data;
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    return ready;
#else
    return true;
#endif
}

static void udp_send(void *self, const purr_address to, const void *data, const uint32_t size)
{
    const udp *u = self;
    if (to.kind != PURR_ADDRESS_IPV4) return;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(to.port);
    a.sin_addr.s_addr = htonl(to.host);
    sendto(u->socket, (const char *)data, (int)size, 0, (const struct sockaddr *)&a, sizeof a);
}

static uint32_t udp_receive(void *self, purr_address *from, void *data, const uint32_t capacity)
{
    const udp *u = self;
    for (;;) {
        struct sockaddr_in a;
        socklen_t length = sizeof a;
        const int n = (int)recvfrom(u->socket, (char *)data, (int)capacity, 0, (struct sockaddr *)&a, &length);
        if (n > 0 && a.sin_family == AF_INET) {
            *from = (purr_address){PURR_ADDRESS_IPV4, ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
            return (uint32_t)n;
        }
        if (n > 0) continue; // Not IPv4
#ifdef _WIN32
        // Windows reports a datagram that couldn't be delivered on the next
        // read, and one too long for `data` as an error: skip both.
        const int error = WSAGetLastError();
        if (n < 0 && (error == WSAECONNRESET || error == WSAEMSGSIZE)) continue;
#endif
        return 0;
    }
}

static void udp_close(void *self)
{
    udp *u = self;
    close_socket(u->socket);
    free(u);
}

static bool udp_open(const uint32_t host, const uint16_t port, purr_transport *out)
{
    if (!started()) return false;
    const udp_socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCKET) return false;
#ifdef _WIN32
    u_long nonblocking = 1;
    const bool configured = ioctlsocket(s, FIONBIO, &nonblocking) == 0;
#else
    const int flags = fcntl(s, F_GETFL, 0);
    const bool configured = flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(host);
    udp *u = configured && bind(s, (const struct sockaddr *)&a, sizeof a) == 0 ? malloc(sizeof *u) : NULL;
    if (!u) {
        close_socket(s);
        return false;
    }
    u->socket = s;
    *out = (purr_transport){u, udp_send, udp_receive, udp_close};
    return true;
}

bool purr_platform_udp_open(const uint16_t port, purr_transport *out)
{
    return udp_open(INADDR_ANY, port, out);
}

bool purr_platform_udp_open_local(const uint16_t port, purr_transport *out)
{
    return udp_open(INADDR_LOOPBACK, port, out);
}

bool purr_platform_resolve(const char *text, const uint16_t default_port, purr_address *out)
{
    if (purr_address_parse(text, default_port, out)) return true;
    if (!started()) return false;
    // name or name:port
    char name[256];
    const char *colon = strrchr(text, ':');
    const size_t length = colon ? (size_t)(colon - text) : strlen(text);
    if (length == 0 || length >= sizeof name) return false;
    memcpy(name, text, length);
    name[length] = '\0';
    uint32_t port = default_port;
    if (colon) {
        char *end;
        port = (uint32_t)strtoul(colon + 1, &end, 10);
        if (*end != '\0' || port > 65535u) return false;
    }
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    struct addrinfo *found = NULL;
    if (getaddrinfo(name, NULL, &hints, &found) != 0 || !found) return false;
    const struct sockaddr_in *a = (const struct sockaddr_in *)found->ai_addr;
    *out = (purr_address){PURR_ADDRESS_IPV4, ntohl(a->sin_addr.s_addr), (uint16_t)port};
    freeaddrinfo(found);
    return true;
}

#endif
