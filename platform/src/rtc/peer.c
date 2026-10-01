// A peer connection (see rtc.h): the offer and answer, ICE, then DTLS, then
// SCTP and the channel, as a browser's RTCPeerConnection does them.
//
// The side that joins offers (actpass) and opens the channel; the host
// answers as the DTLS client (active), which also starts SCTP. Candidates
// trickle: each goes as a signal of its own once the description has.

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void signal_out(rtc_peer *p, const char *json)
{
    if (p->signal_count == RTC_PEER_SIGNALS) return;
    const size_t n = strlen(json);
    char *copy = malloc(n + 1);
    if (!copy) return;
    memcpy(copy, json, n + 1);
    p->signals[p->signal_count++] = copy;
}

static const char *our_mid(const rtc_peer *p)
{
    return p->offerer ? "0" : p->remote.mid;
}

static void tell_description(rtc_peer *p)
{
    rtc_description d;
    memset(&d, 0, sizeof d);
    snprintf(d.ufrag, sizeof d.ufrag, "%s", p->ice.ufrag);
    snprintf(d.pwd, sizeof d.pwd, "%s", p->ice.pwd);
    memcpy(d.fingerprint, p->identity.fingerprint, 32);
    d.has_fingerprint = true;
    d.setup = p->offerer ? RTC_SETUP_ACTPASS : RTC_SETUP_ACTIVE;
    snprintf(d.mid, sizeof d.mid, "%s", our_mid(p));
    d.sctp_port = 5000;
    char sdp[2048];
    rtc_text t = {sdp, 0, sizeof sdp, false};
    rtc_sdp_write(&t, &d, p->session);
    char json[4096];
    rtc_text j = {json, 0, sizeof json, false};
    rtc_text_put(&j, p->offerer ? "{\"description\":{\"type\":\"offer\",\"sdp\":" : "{\"description\":{\"type\":\"answer\",\"sdp\":");
    rtc_text_json_string(&j, sdp);
    rtc_text_put(&j, "}}");
    if (!t.overflow && !j.overflow) signal_out(p, json);
    p->description_told = true;
}

static void tell_candidates(rtc_peer *p)
{
    rtc_candidate c;
    while (p->description_told && rtc_ice_next_local(&p->ice, &c)) {
        char line[256];
        rtc_text t = {line, 0, sizeof line, false};
        rtc_candidate_write(&t, &c);
        char json[512];
        rtc_text j = {json, 0, sizeof json, false};
        rtc_text_put(&j, "{\"candidate\":{\"candidate\":");
        rtc_text_json_string(&j, line);
        rtc_text_put(&j, ",\"sdpMid\":");
        rtc_text_json_string(&j, our_mid(p));
        rtc_text_put(&j, ",\"sdpMLineIndex\":0}}");
        if (!t.overflow && !j.overflow) signal_out(p, json);
    }
}

// Between the layers: DTLS over ICE, SCTP over DTLS, messages into the inbox.
static void dtls_out(void *user, const void *data, const size_t size)
{
    rtc_peer *p = user;
    rtc_ice_send(&p->ice, data, size);
}

static void dtls_in(void *user, const void *data, const size_t size)
{
    rtc_peer *p = user;
    if (p->sctp_started) rtc_sctp_receive(&p->sctp, data, size, p->now);
}

static void sctp_out(void *user, const void *data, const size_t size)
{
    rtc_peer *p = user;
    rtc_dtls_send(&p->dtls, data, size);
}

static void sctp_in(void *user, const void *data, const size_t size)
{
    rtc_peer *p = user;
    if (p->inbox_count == RTC_PEER_INBOX || size > sizeof p->inbox[0].data) return; // Like UDP: lost
    const int at = (p->inbox_head + p->inbox_count++) % RTC_PEER_INBOX;
    p->inbox[at].size = (uint16_t)size;
    memcpy(p->inbox[at].data, data, size);
}

rtc_peer *rtc_peer_new(const bool offerer, const rtc_ice_config *config)
{
    rtc_peer *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->offerer = offerer;
    p->now = rtc_now();
    if (!rtc_identity_new(&p->identity) || !rtc_ice_start(&p->ice, offerer, config)
        || !rtc_random(&p->session, sizeof p->session)) {
        free(p);
        return NULL;
    }
    if (offerer) tell_description(p);
    return p;
}

void rtc_peer_free(rtc_peer *p)
{
    if (!p) return;
    if (p->dtls_started) rtc_dtls_close(&p->dtls);
    rtc_ice_close(&p->ice);
    for (int i = 0; i < p->signal_count; i++) free(p->signals[i]);
    free(p);
}

void rtc_peer_description(rtc_peer *p, const char *type, const char *sdp)
{
    const bool expected = strcmp(type, p->offerer ? "answer" : "offer") == 0;
    if (p->have_remote || !expected) return;
    if (!rtc_sdp_parse(sdp, &p->remote)) {
        p->state = RTC_PEER_FAILED;
        return;
    }
    rtc_ice_set_remote(&p->ice, p->remote.ufrag, p->remote.pwd);
    for (int i = 0; i < p->remote.candidate_count; i++) rtc_ice_add_remote(&p->ice, &p->remote.candidates[i]);
    // We answer as the DTLS client; an answer to us says which we are
    p->dtls_client = p->offerer ? p->remote.setup == RTC_SETUP_PASSIVE : true;
    rtc_debug("peer: their %s, with %d candidates: we're the DTLS %s", type, p->remote.candidate_count,
              p->dtls_client ? "client" : "server");
    p->have_remote = true;
    if (!p->offerer) tell_description(p);
    if (!p->dtls_client) {
        // A server only waits for ClientHello, which can come as soon as ICE works for them
        p->dtls_started = rtc_dtls_start(&p->dtls, false, &p->identity, p->remote.fingerprint, dtls_out, dtls_in, p,
                                         p->now);
        if (!p->dtls_started) p->state = RTC_PEER_FAILED;
    }
}

void rtc_peer_candidate(rtc_peer *p, const char *candidate)
{
    rtc_candidate c;
    if (candidate[0] && rtc_candidate_parse(candidate, &c)) rtc_ice_add_remote(&p->ice, &c);
}

bool rtc_peer_next_signal(rtc_peer *p, char *json, const size_t capacity)
{
    if (!p->signal_count) return false;
    snprintf(json, capacity, "%s", p->signals[0]);
    free(p->signals[0]);
    memmove(p->signals, p->signals + 1, sizeof p->signals[0] * (size_t)(p->signal_count - 1));
    p->signal_count--;
    return true;
}

void rtc_peer_update(rtc_peer *p, const double now)
{
    p->now = now;
    if (p->state == RTC_PEER_FAILED) return;
    rtc_ice_update(&p->ice, now);
    tell_candidates(p);

    // The client starts DTLS once ICE has a pair to send ClientHello on
    if (p->have_remote && p->dtls_client && !p->dtls_started && p->ice.state == RTC_ICE_CONNECTED) {
        p->dtls_started = rtc_dtls_start(&p->dtls, true, &p->identity, p->remote.fingerprint, dtls_out, dtls_in, p, now);
        if (!p->dtls_started) p->state = RTC_PEER_FAILED;
    }
    uint8_t datagram[2048];
    size_t n;
    while ((n = rtc_ice_receive(&p->ice, now, datagram, sizeof datagram)) != 0) {
        if (p->dtls_started && datagram[0] >= 20 && datagram[0] <= 63) rtc_dtls_receive(&p->dtls, datagram, n, now);
    }
    if (p->dtls_started) rtc_dtls_update(&p->dtls, now);

    // Then SCTP: the DTLS client starts the association, and whoever joined opens the channel
    if (p->dtls_started && p->dtls.state == RTC_DTLS_OPEN && !p->sctp_started) {
        rtc_sctp_start(&p->sctp, p->dtls_client, p->remote.sctp_port, sctp_out, sctp_in, p, now);
        if (p->offerer) rtc_sctp_open_channel(&p->sctp, p->dtls_client ? 0 : 1, now);
        p->sctp_started = true;
    }
    if (p->sctp_started) rtc_sctp_update(&p->sctp, now);

    const bool open = p->sctp_started && p->sctp.state == RTC_SCTP_OPEN && p->sctp.channel;
    if (open && p->state != RTC_PEER_OPEN) rtc_debug("peer: the channel is open");
    if (open) p->state = RTC_PEER_OPEN;
    const bool dtls_down = p->dtls_started && (p->dtls.state == RTC_DTLS_FAILED || p->dtls.state == RTC_DTLS_CLOSED);
    const bool sctp_down = p->sctp_started && (p->sctp.state == RTC_SCTP_FAILED || p->sctp.state == RTC_SCTP_CLOSED);
    const bool closed = p->state == RTC_PEER_OPEN && !open;
    if (p->ice.state == RTC_ICE_FAILED || dtls_down || sctp_down || closed) p->state = RTC_PEER_FAILED;
}

void rtc_peer_send(rtc_peer *p, const void *data, const size_t size)
{
    if (p->state == RTC_PEER_OPEN) rtc_sctp_send(&p->sctp, data, size, p->now);
}

size_t rtc_peer_receive(rtc_peer *p, void *out, const size_t capacity)
{
    while (p->inbox_count) {
        const int at = p->inbox_head;
        p->inbox_head = (p->inbox_head + 1) % RTC_PEER_INBOX;
        p->inbox_count--;
        if (p->inbox[at].size <= capacity && p->inbox[at].size) {
            memcpy(out, p->inbox[at].data, p->inbox[at].size);
            return p->inbox[at].size;
        }
    }
    return 0;
}
