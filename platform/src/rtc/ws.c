// A WebSocket client (RFC 6455) for the relay (see rtc.h): wss:// over the
// system's TLS (tls.c), or ws:// over plain TCP, text messages, pings
// answered. Nothing blocks but looking up the name.

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER (256 * 1024)
#define CONNECT_TIMEOUT 10.0

static void base64(const uint8_t *in, const size_t n, char *out)
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = letters[v >> 18 & 63];
        out[o++] = letters[v >> 12 & 63];
        out[o++] = i + 1 < n ? letters[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? letters[v & 63] : '=';
    }
    out[o] = '\0';
}

static void closed(rtc_ws *w)
{
    rtc_socket_close(w->socket);
    w->socket = RTC_NO_SOCKET;
    w->state = RTC_WS_CLOSED;
}

// Bytes to and from the relay, through TLS for wss://
static int raw_send(rtc_ws *w, const void *data, const size_t size)
{
    return w->tls ? rtc_tls_send(w->tls, w->socket, data, size) : rtc_tcp_send(w->socket, data, size);
}

static int raw_receive(rtc_ws *w, void *out, const size_t capacity)
{
    return w->tls ? rtc_tls_receive(w->tls, w->socket, out, capacity) : rtc_tcp_receive(w->socket, out, capacity);
}

static void queue(rtc_ws *w, const void *data, const size_t size)
{
    if (w->state == RTC_WS_CLOSED || w->out_size + size > BUFFER) return;
    memcpy(w->out + w->out_size, data, size);
    w->out_size += size;
}

bool rtc_ws_open(rtc_ws *w, const char *url, const double now)
{
    memset(w, 0, sizeof *w);
    w->socket = RTC_NO_SOCKET;
    w->state = RTC_WS_CLOSED;
    const bool secure = strncmp(url, "wss://", 6) == 0;
    if (!secure && strncmp(url, "ws://", 5) != 0) return false;
    const char *host = url + (secure ? 6 : 5);
    const char *path = strchr(host, '/');
    const size_t host_size = path ? (size_t)(path - host) : strlen(host);
    char name[256];
    if (host_size == 0 || host_size >= sizeof name) return false;
    memcpy(name, host, host_size);
    name[host_size] = '\0';
    uint16_t port = secure ? 443 : 80;
    char *colon = strchr(name, ':');
    if (colon) {
        *colon = '\0';
        port = (uint16_t)strtoul(colon + 1, NULL, 10);
    }
    rtc_addr to;
    uint8_t nonce[16];
    if (!rtc_resolve(name, port, &to) || !rtc_random(nonce, sizeof nonce)) return false;
    w->in = malloc(BUFFER);
    w->out = malloc(BUFFER);
    w->tls = secure ? rtc_tls_new(name) : NULL;
    w->socket = rtc_tcp_connect(to);
    if (!w->in || !w->out || w->socket == RTC_NO_SOCKET || (secure && !w->tls)) {
        rtc_ws_close(w);
        return false;
    }
    base64(nonce, sizeof nonce, w->key);
    snprintf(w->request, sizeof w->request,
             "GET %s HTTP/1.1\r\nHost: %.*s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
             path ? path : "/", (int)host_size, host, w->key);
    w->state = RTC_WS_CONNECTING;
    w->started = now;
    return true;
}

// A frame of ours: masked, as a client's must be.
static void send_frame(rtc_ws *w, const uint8_t opcode, const void *data, const size_t size)
{
    uint8_t header[14];
    size_t n = 0;
    header[n++] = (uint8_t)(0x80 | opcode);
    if (size < 126) {
        header[n++] = (uint8_t)(0x80 | size);
    } else if (size < 65536) {
        header[n++] = 0x80 | 126;
        header[n++] = (uint8_t)(size >> 8);
        header[n++] = (uint8_t)size;
    } else {
        header[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) header[n++] = (uint8_t)((uint64_t)size >> (8 * i));
    }
    uint8_t mask[4];
    rtc_random(mask, sizeof mask);
    memcpy(header + n, mask, 4);
    n += 4;
    if (w->out_size + n + size > BUFFER) return;
    queue(w, header, n);
    const uint8_t *p = data;
    for (size_t i = 0; i < size; i++) w->out[w->out_size++] = p[i] ^ mask[i & 3];
}

void rtc_ws_send(rtc_ws *w, const char *text, const size_t size)
{
    if (w->state != RTC_WS_CLOSED) send_frame(w, 1, text, size);
}

void rtc_ws_update(rtc_ws *w, const double now)
{
    if (w->state == RTC_WS_CLOSED) return;
    if (w->state == RTC_WS_CONNECTING && !w->upgraded) {
        if (now - w->started > CONNECT_TIMEOUT) {
            closed(w);
            return;
        }
        if (!w->connected) {
            const int connected = rtc_tcp_connected(w->socket);
            if (connected < 0) {
                closed(w);
                return;
            }
            if (connected == 0) return;
            w->connected = true;
        }
        if (w->tls) {
            const int done = rtc_tls_handshake(w->tls, w->socket);
            if (done < 0) {
                fprintf(stderr, "purr: couldn't connect securely to the relay\n");
                closed(w);
                return;
            }
            if (done == 0) return;
        }
        if (w->request[0]) {
            // The request goes before anything else
            const size_t n = strlen(w->request);
            memmove(w->out + n, w->out, w->out_size);
            memcpy(w->out, w->request, n);
            w->out_size += n;
            w->request[0] = '\0';
        }
    }
    while (w->out_size) {
        const int sent = raw_send(w, w->out, w->out_size);
        if (sent < 0) {
            closed(w);
            return;
        }
        if (sent == 0) break;
        memmove(w->out, w->out + sent, w->out_size - (size_t)sent);
        w->out_size -= (size_t)sent;
    }
    for (;;) {
        if (w->in_size == BUFFER) {
            closed(w); // A message too big for us
            return;
        }
        const int got = raw_receive(w, w->in + w->in_size, BUFFER - w->in_size);
        if (got < 0) {
            closed(w);
            return;
        }
        if (got == 0) break;
        w->in_size += (size_t)got;
    }
    if (!w->upgraded) {
        // The server's answer: 101, with the key we sent, hashed
        const uint8_t *end = NULL;
        for (size_t i = 3; i < w->in_size && !end; i++) {
            if (memcmp(w->in + i - 3, "\r\n\r\n", 4) == 0) end = w->in + i + 1;
        }
        if (!end) return;
        const size_t head = (size_t)(end - w->in);
        char text[1024];
        const size_t n = head < sizeof text - 1 ? head : sizeof text - 1;
        memcpy(text, w->in, n);
        text[n] = '\0';
        char both[64], accept[40];
        snprintf(both, sizeof both, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", w->key);
        uint8_t hash[20];
        rtc_sha1 s;
        rtc_sha1_init(&s);
        rtc_sha1_add(&s, both, strlen(both));
        rtc_sha1_end(&s, hash);
        base64(hash, sizeof hash, accept);
        if (strncmp(text, "HTTP/1.1 101", 12) != 0 || !strstr(text, accept)) {
            closed(w);
            return;
        }
        memmove(w->in, w->in + head, w->in_size - head);
        w->in_size -= head;
        w->upgraded = true;
        w->state = RTC_WS_OPEN;
    }
}

bool rtc_ws_next(rtc_ws *w, char *message, const size_t capacity)
{
    while (w->upgraded && w->in_size >= 2) {
        const uint8_t *p = w->in;
        const bool fin = p[0] & 0x80;
        const uint8_t opcode = p[0] & 0x0f;
        uint64_t size = p[1] & 0x7f;
        size_t at = 2;
        if (size == 126) {
            if (w->in_size < 4) return false;
            size = rtc_get16(p + 2);
            at = 4;
        } else if (size == 127) {
            if (w->in_size < 10) return false;
            size = (uint64_t)rtc_get32(p + 2) << 32 | rtc_get32(p + 6);
            at = 10;
        }
        if (p[1] & 0x80) at += 4; // Servers don't mask, but skip it if one does
        if (size > BUFFER || w->in_size < at + size) {
            if (size > BUFFER) closed(w);
            return false;
        }
        const uint8_t *payload = p + at;
        bool got = false;
        if (opcode == 9) {
            send_frame(w, 10, payload, (size_t)size); // Ping: pong
        } else if (opcode == 8) {
            closed(w);
            return false;
        } else if ((opcode == 1 || opcode == 2) && fin && size < capacity) {
            memcpy(message, payload, (size_t)size);
            message[size] = '\0';
            got = true;
        }
        // Fragmented messages aren't something the relay sends
        memmove(w->in, w->in + at + size, w->in_size - at - (size_t)size);
        w->in_size -= at + (size_t)size;
        if (got) return true;
    }
    return false;
}

void rtc_ws_close(rtc_ws *w)
{
    if (w->state == RTC_WS_OPEN) {
        static const uint8_t normal[] = {0x03, 0xe8}; // 1000
        send_frame(w, 8, normal, sizeof normal);
        raw_send(w, w->out, w->out_size);
    }
    rtc_socket_close(w->socket);
    w->socket = RTC_NO_SOCKET;
    w->state = RTC_WS_CLOSED;
    rtc_tls_free(w->tls);
    w->tls = NULL;
    free(w->in);
    free(w->out);
    w->in = NULL;
    w->out = NULL;
}
