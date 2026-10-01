// Session descriptions (SDP, RFC 8866) for a data channel alone, as browsers
// write them (RFC 8841): what ICE needs to connect (its username fragment
// and password, and candidates), the DTLS certificate's fingerprint and role,
// and the SCTP port. See rtc.h.

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CANDIDATE_TYPES[] = {"host", "srflx", "prflx", "relay"};

bool rtc_candidate_parse(const char *text, rtc_candidate *c)
{
    // [a=]candidate:<foundation> <component> <transport> <priority> <address> <port> typ <type> ...
    if (strncmp(text, "a=", 2) == 0) text += 2;
    if (strncmp(text, "candidate:", 10) != 0) return false;
    char foundation[33], transport[8], address[64], type[8];
    unsigned component = 0, port = 0;
    unsigned long priority = 0;
    if (sscanf(text + 10, "%32s %u %7s %lu %63s %u typ %7s", foundation, &component, transport, &priority, address,
               &port, type) != 7) {
        return false;
    }
    // One component, UDP and IPv4: what we reach. Names (like the .local ones
    // browsers hide their addresses behind) and IPv6 aren't, for now.
    unsigned a, b, cc, d;
    char rest;
    if (component != 1 || (strcmp(transport, "udp") != 0 && strcmp(transport, "UDP") != 0) || port == 0 || port > 65535
        || sscanf(address, "%u.%u.%u.%u%c", &a, &b, &cc, &d, &rest) != 4 || a > 255 || b > 255 || cc > 255 || d > 255) {
        return false;
    }
    memset(c, 0, sizeof *c);
    c->addr = (rtc_addr){a << 24 | b << 16 | cc << 8 | d, (uint16_t)port};
    c->priority = (uint32_t)priority;
    c->type = RTC_HOST;
    for (int i = 0; i < 4; i++) {
        if (strcmp(type, CANDIDATE_TYPES[i]) == 0) c->type = (uint8_t)i;
    }
    snprintf(c->foundation, sizeof c->foundation, "%s", foundation);
    return true;
}

void rtc_candidate_write(rtc_text *t, const rtc_candidate *c)
{
    char line[160];
    snprintf(line, sizeof line, "candidate:%s 1 udp %lu %u.%u.%u.%u %u typ %s", c->foundation,
             (unsigned long)c->priority, (unsigned)(c->addr.ip >> 24), (unsigned)(c->addr.ip >> 16 & 255),
             (unsigned)(c->addr.ip >> 8 & 255), (unsigned)(c->addr.ip & 255), (unsigned)c->addr.port,
             CANDIDATE_TYPES[c->type < 4 ? c->type : 0]);
    rtc_text_put(t, line);
    if (c->type != RTC_HOST) rtc_text_put(t, " raddr 0.0.0.0 rport 0"); // Not telling: the base is only ours
    rtc_text_put(t, " generation 0");
}

// ---------------------------------------------------------------------------

static bool parse_fingerprint(const char *text, uint8_t out[32])
{
    // sha-256 AB:CD:...
    if (strncmp(text, "sha-256 ", 8) != 0 && strncmp(text, "SHA-256 ", 8) != 0) return false;
    const char *p = text + 8;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(p, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
        p += 2;
        if (i < 31 && *p++ != ':') return false;
    }
    return true;
}

bool rtc_sdp_parse(const char *sdp, rtc_description *d)
{
    memset(d, 0, sizeof *d);
    d->sctp_port = 5000;
    bool application = false;
    const char *line = sdp;
    while (*line) {
        const char *end = line;
        while (*end && *end != '\r' && *end != '\n') end++;
        char text[512];
        const size_t n = (size_t)(end - line) < sizeof text - 1 ? (size_t)(end - line) : sizeof text - 1;
        memcpy(text, line, n);
        text[n] = '\0';
        if (strncmp(text, "m=", 2) == 0) application = strstr(text, "webrtc-datachannel") != NULL;
        else if (strncmp(text, "a=ice-ufrag:", 12) == 0) snprintf(d->ufrag, sizeof d->ufrag, "%s", text + 12);
        else if (strncmp(text, "a=ice-pwd:", 10) == 0) snprintf(d->pwd, sizeof d->pwd, "%s", text + 10);
        else if (strncmp(text, "a=fingerprint:", 14) == 0) d->has_fingerprint = parse_fingerprint(text + 14, d->fingerprint);
        else if (strncmp(text, "a=setup:", 8) == 0) {
            d->setup = strcmp(text + 8, "active") == 0    ? RTC_SETUP_ACTIVE
                     : strcmp(text + 8, "passive") == 0 ? RTC_SETUP_PASSIVE
                                                        : RTC_SETUP_ACTPASS;
        } else if (strncmp(text, "a=mid:", 6) == 0 && application) {
            snprintf(d->mid, sizeof d->mid, "%s", text + 6);
        } else if (strncmp(text, "a=sctp-port:", 12) == 0) {
            d->sctp_port = (uint16_t)strtoul(text + 12, NULL, 10);
        } else if (strncmp(text, "a=candidate:", 12) == 0 && d->candidate_count < RTC_MAX_CANDIDATES) {
            if (rtc_candidate_parse(text, &d->candidates[d->candidate_count])) d->candidate_count++;
        }
        line = end;
        while (*line == '\r' || *line == '\n') line++;
    }
    return d->ufrag[0] && d->pwd[0] && d->has_fingerprint;
}

void rtc_sdp_write(rtc_text *t, const rtc_description *d, const uint64_t session)
{
    char line[256];
    snprintf(line, sizeof line, "v=0\r\no=- %llu 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE %s\r\n",
             (unsigned long long)(session & 0x7fffffffffffffffull), d->mid);
    rtc_text_put(t, line);
    rtc_text_put(t, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n");
    snprintf(line, sizeof line, "a=ice-ufrag:%s\r\na=ice-pwd:%s\r\na=ice-options:trickle\r\n", d->ufrag, d->pwd);
    rtc_text_put(t, line);
    rtc_text_put(t, "a=fingerprint:sha-256 ");
    for (int i = 0; i < 32; i++) {
        snprintf(line, sizeof line, "%02X%s", d->fingerprint[i], i < 31 ? ":" : "\r\n");
        rtc_text_put(t, line);
    }
    snprintf(line, sizeof line, "a=setup:%s\r\na=mid:%s\r\na=sctp-port:%u\r\na=max-message-size:262144\r\n",
             d->setup == RTC_SETUP_ACTIVE ? "active" : d->setup == RTC_SETUP_PASSIVE ? "passive" : "actpass", d->mid,
             (unsigned)d->sctp_port);
    rtc_text_put(t, line);
    for (int i = 0; i < d->candidate_count; i++) {
        rtc_text_put(t, "a=");
        rtc_candidate_write(t, &d->candidates[i]);
        rtc_text_put(t, "\r\n");
    }
}
