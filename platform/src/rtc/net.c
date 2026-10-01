// Sockets and the clock for WebRTC (see rtc.h): UDP for ICE and what goes
// over it, TCP for the relay, all without blocking, except looking up names.

#if !defined(_WIN32) && !defined(__APPLE__)
#define _DEFAULT_SOURCE // getaddrinfo and clock_gettime under strict C
#endif

#include "rtc.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define close_socket closesocket
#define FD(s) ((SOCKET)(s))
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#define close_socket close
#define FD(s) ((int)(s))
#endif

double rtc_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
#endif
}

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

static bool nonblocking(const rtc_socket s)
{
#ifdef _WIN32
    u_long on = 1;
    return ioctlsocket((SOCKET)s, FIONBIO, &on) == 0;
#else
    const int flags = fcntl(FD(s), F_GETFL, 0);
    return flags >= 0 && fcntl(FD(s), F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

static struct sockaddr_in to_sockaddr(const rtc_addr a)
{
    struct sockaddr_in s;
    memset(&s, 0, sizeof s);
    s.sin_family = AF_INET;
    s.sin_port = htons(a.port);
    s.sin_addr.s_addr = htonl(a.ip);
    return s;
}

static bool would_block(void)
{
#ifdef _WIN32
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
#endif
}

void rtc_socket_close(const rtc_socket s)
{
    if (s != RTC_NO_SOCKET) close_socket(FD(s));
}

// PURR_RTC_LOCAL: sockets on loopback only, for tests on one machine. They
// then open no port to the network, and on Windows ask nothing of the firewall.
bool rtc_local_only(void)
{
    static int on = -1;
    if (on < 0) on = getenv("PURR_RTC_LOCAL") != NULL;
    return on;
}

rtc_socket rtc_udp_open(void)
{
    if (!started()) return RTC_NO_SOCKET;
    const rtc_socket s = (rtc_socket)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == RTC_NO_SOCKET) return RTC_NO_SOCKET;
    const struct sockaddr_in any = to_sockaddr((rtc_addr){rtc_local_only() ? 0x7f000001u : 0u, 0});
    if (!nonblocking(s) || bind(FD(s), (const struct sockaddr *)&any, sizeof any) != 0) {
        close_socket(FD(s));
        return RTC_NO_SOCKET;
    }
#ifdef _WIN32
    // Don't report datagrams that didn't arrive as errors on the next read
    BOOL report = FALSE;
    DWORD returned = 0;
    WSAIoctl((SOCKET)s, _WSAIOW(IOC_VENDOR, 12), &report, sizeof report, NULL, 0, &returned, NULL, NULL);
#endif
    return s;
}

uint16_t rtc_socket_port(const rtc_socket s)
{
    struct sockaddr_in a;
    socklen_t length = sizeof a;
    if (getsockname(FD(s), (struct sockaddr *)&a, &length) != 0) return 0;
    return ntohs(a.sin_port);
}

void rtc_udp_send(const rtc_socket s, const rtc_addr to, const void *data, const size_t size)
{
    const struct sockaddr_in a = to_sockaddr(to);
    sendto(FD(s), (const char *)data, (int)size, 0, (const struct sockaddr *)&a, sizeof a);
}

size_t rtc_udp_receive(const rtc_socket s, rtc_addr *from, void *out, const size_t capacity)
{
    for (;;) {
        struct sockaddr_in a;
        socklen_t length = sizeof a;
        const int n = (int)recvfrom(FD(s), (char *)out, (int)capacity, 0, (struct sockaddr *)&a, &length);
        if (n > 0 && a.sin_family == AF_INET) {
            *from = (rtc_addr){ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
            return (size_t)n;
        }
        if (n > 0) continue;
#ifdef _WIN32
        const int error = WSAGetLastError();
        if (n < 0 && (error == WSAECONNRESET || error == WSAEMSGSIZE)) continue;
#endif
        return 0;
    }
}

// The address this machine reaches others from: "connecting" a UDP socket
// sends nothing, but picks the route, and with it the local address.
bool rtc_local_ip(uint32_t *ip)
{
    if (rtc_local_only()) {
        *ip = 0x7f000001u;
        return true;
    }
    if (!started()) return false;
    const rtc_socket s = (rtc_socket)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == RTC_NO_SOCKET) return false;
    const struct sockaddr_in outside = to_sockaddr((rtc_addr){0x08080808u, 53}); // Any public address will do
    struct sockaddr_in a;
    socklen_t length = sizeof a;
    const bool ok = connect(FD(s), (const struct sockaddr *)&outside, sizeof outside) == 0
                 && getsockname(FD(s), (struct sockaddr *)&a, &length) == 0 && a.sin_addr.s_addr != 0;
    if (ok) *ip = ntohl(a.sin_addr.s_addr);
    close_socket(FD(s));
    return ok;
}

bool rtc_resolve(const char *host, const uint16_t port, rtc_addr *out)
{
    if (!started()) return false;
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    struct addrinfo *found = NULL;
    if (getaddrinfo(host, NULL, &hints, &found) != 0 || !found) return false;
    const struct sockaddr_in *a = (const struct sockaddr_in *)found->ai_addr;
    *out = (rtc_addr){ntohl(a->sin_addr.s_addr), port};
    freeaddrinfo(found);
    return true;
}

rtc_socket rtc_tcp_connect(const rtc_addr to)
{
    if (!started()) return RTC_NO_SOCKET;
    const rtc_socket s = (rtc_socket)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == RTC_NO_SOCKET) return RTC_NO_SOCKET;
    const int on = 1;
    setsockopt(FD(s), IPPROTO_TCP, TCP_NODELAY, (const char *)&on, sizeof on);
#ifdef SO_NOSIGPIPE
    setsockopt(FD(s), SOL_SOCKET, SO_NOSIGPIPE, (const char *)&on, sizeof on); // macOS: a closed socket isn't a signal
#endif
    const struct sockaddr_in a = to_sockaddr(to);
    if (!nonblocking(s) || (connect(FD(s), (const struct sockaddr *)&a, sizeof a) != 0 && !would_block())) {
        close_socket(FD(s));
        return RTC_NO_SOCKET;
    }
    return s;
}

int rtc_tcp_connected(const rtc_socket s)
{
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(FD(s), &writable);
    FD_SET(FD(s), &failed);
    struct timeval now = {0, 0};
    if (select((int)s + 1, NULL, &writable, &failed, &now) < 0) return -1;
    if (FD_ISSET(FD(s), &failed)) return -1;
    if (!FD_ISSET(FD(s), &writable)) return 0;
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(FD(s), SOL_SOCKET, SO_ERROR, (char *)&error, &length) != 0 || error != 0) return -1;
    return 1;
}

int rtc_tcp_send(const rtc_socket s, const void *data, const size_t size)
{
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    const int n = (int)send(FD(s), (const char *)data, (int)size, flags);
    if (n >= 0) return n;
    return would_block() ? 0 : -1;
}

int rtc_tcp_receive(const rtc_socket s, void *out, const size_t capacity)
{
    const int n = (int)recv(FD(s), (char *)out, (int)capacity, 0);
    if (n > 0) return n;
    if (n == 0) return -1; // Closed
    return would_block() ? 0 : -1;
}

void rtc_debug(const char *format, ...)
{
    static int on = -1;
    if (on < 0) on = getenv("PURR_RTC_DEBUG") != NULL;
    if (!on) return;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "rtc %.3f: ", rtc_now());
    vfprintf(stderr, format, args);
    fputc(10, stderr);
    va_end(args);
}
