// STUN messages (RFC 8489), as ICE (RFC 8445) and TURN (RFC 8656) use them:
// writing them, attribute by attribute, and reading them (see rtc.h).

#include "rtc.h"

#include <stdio.h>
#include <string.h>

#define MAGIC 0x2112a442u

void rtc_stun_begin(rtc_stun_writer *w, uint8_t *buffer, const size_t capacity, const uint16_t type,
                    const uint8_t id[12])
{
    *w = (rtc_stun_writer){buffer, 20, capacity, capacity < 20};
    if (w->overflow) return;
    rtc_put16(buffer, type);
    rtc_put16(buffer + 2, 0);
    rtc_put32(buffer + 4, MAGIC);
    memcpy(buffer + 8, id, 12);
}

static void set_length(rtc_stun_writer *w)
{
    if (!w->overflow) rtc_put16(w->data + 2, (uint32_t)(w->size - 20));
}

void rtc_stun_attr(rtc_stun_writer *w, const uint16_t type, const void *value, const size_t size)
{
    const size_t padded = (size + 3) & ~(size_t)3;
    if (w->overflow || size > 0xffff || w->size + 4 + padded > w->capacity) {
        w->overflow = true;
        return;
    }
    uint8_t *p = w->data + w->size;
    rtc_put16(p, type);
    rtc_put16(p + 2, (uint32_t)size);
    if (size) memcpy(p + 4, value, size);
    memset(p + 4 + size, 0, padded - size);
    w->size += 4 + padded;
    set_length(w);
}

void rtc_stun_attr32(rtc_stun_writer *w, const uint16_t type, const uint32_t value)
{
    uint8_t v[4];
    rtc_put32(v, value);
    rtc_stun_attr(w, type, v, 4);
}

void rtc_stun_attr64(rtc_stun_writer *w, const uint16_t type, const uint64_t value)
{
    uint8_t v[8];
    rtc_put32(v, (uint32_t)(value >> 32));
    rtc_put32(v + 4, (uint32_t)value);
    rtc_stun_attr(w, type, v, 8);
}

void rtc_stun_attr_addr(rtc_stun_writer *w, const uint16_t type, const rtc_addr a)
{
    uint8_t v[8] = {0, 1}; // IPv4
    rtc_put16(v + 2, (uint32_t)(a.port ^ (MAGIC >> 16)));
    rtc_put32(v + 4, a.ip ^ MAGIC);
    rtc_stun_attr(w, type, v, 8);
}

void rtc_stun_integrity(rtc_stun_writer *w, const void *key, const size_t key_size)
{
    if (w->overflow || w->size + 24 > w->capacity) {
        w->overflow = true;
        return;
    }
    // The length counts the attribute it's about to be in
    rtc_put16(w->data + 2, (uint32_t)(w->size - 20 + 24));
    uint8_t mac[20];
    rtc_hmac1 h;
    rtc_hmac1_init(&h, key, key_size);
    rtc_hmac1_add(&h, w->data, w->size);
    rtc_hmac1_end(&h, mac);
    rtc_stun_attr(w, RTC_STUN_MESSAGE_INTEGRITY, mac, 20);
}

void rtc_stun_fingerprint(rtc_stun_writer *w)
{
    if (w->overflow || w->size + 8 > w->capacity) {
        w->overflow = true;
        return;
    }
    rtc_put16(w->data + 2, (uint32_t)(w->size - 20 + 8));
    rtc_stun_attr32(w, RTC_STUN_FINGERPRINT, rtc_crc32(w->data, w->size) ^ 0x5354554eu);
}

// ---------------------------------------------------------------------------

bool rtc_stun_is(const uint8_t *p, const size_t size)
{
    return size >= 20 && (p[0] & 0xc0) == 0 && rtc_get32(p + 4) == MAGIC;
}

bool rtc_stun_parse(const uint8_t *p, const size_t size, rtc_stun *m)
{
    memset(m, 0, sizeof *m);
    if (!rtc_stun_is(p, size) || rtc_get16(p + 2) != size - 20 || (size & 3)) return false;
    m->data = p;
    m->size = size;
    m->type = rtc_get16(p);
    memcpy(m->id, p + 8, 12);
    size_t at = 20;
    while (at + 4 <= size) {
        const uint16_t type = rtc_get16(p + at);
        const size_t length = rtc_get16(p + at + 2);
        const size_t padded = (length + 3) & ~(size_t)3;
        if (at + 4 + padded > size) return false;
        const rtc_stun_value v = {p + at + 4, length};
        // After MESSAGE-INTEGRITY, only FINGERPRINT counts
        if (m->integrity_at && type != RTC_STUN_FINGERPRINT) {
            at += 4 + padded;
            continue;
        }
        switch (type) {
        case RTC_STUN_USERNAME: m->username = v; break;
        case RTC_STUN_MESSAGE_INTEGRITY: m->integrity_at = at; break;
        case RTC_STUN_ERROR_CODE: m->error = v.size >= 4 ? (v.data[2] & 7u) * 100u + v.data[3] : 0; break;
        case RTC_STUN_LIFETIME: m->lifetime = v.size == 4 ? rtc_get32(v.data) : 0; break;
        case RTC_STUN_XOR_PEER_ADDRESS: m->has_peer = rtc_stun_read_addr(v, &m->peer); break;
        case RTC_STUN_DATA: m->payload = v; break;
        case RTC_STUN_REALM: m->realm = v; break;
        case RTC_STUN_NONCE: m->nonce = v; break;
        case RTC_STUN_XOR_RELAYED_ADDRESS: m->has_relayed = rtc_stun_read_addr(v, &m->relayed); break;
        case RTC_STUN_XOR_MAPPED_ADDRESS: m->has_mapped = rtc_stun_read_addr(v, &m->mapped); break;
        case RTC_STUN_PRIORITY: m->priority = v.size == 4 ? rtc_get32(v.data) : 0; break;
        case RTC_STUN_USE_CANDIDATE: m->use_candidate = true; break;
        case RTC_STUN_FINGERPRINT: m->fingerprint_at = at; break;
        case RTC_STUN_ICE_CONTROLLED: m->controlled = true; break;
        case RTC_STUN_ICE_CONTROLLING: m->controlling = true; break;
        default: break;
        }
        at += 4 + padded;
    }
    return at == size;
}

bool rtc_stun_read_addr(const rtc_stun_value v, rtc_addr *out)
{
    if (v.size != 8 || v.data[1] != 1) return false; // IPv4 only
    *out = (rtc_addr){rtc_get32(v.data + 4) ^ MAGIC, (uint16_t)(rtc_get16(v.data + 2) ^ (MAGIC >> 16))};
    return true;
}

bool rtc_stun_check(const rtc_stun *m, const void *key, const size_t key_size)
{
    if (!m->integrity_at) return false;
    if (m->fingerprint_at) {
        const uint32_t want = rtc_crc32(m->data, m->fingerprint_at) ^ 0x5354554eu;
        if (rtc_get32(m->data + m->fingerprint_at + 4) != want) return false;
    }
    // The HMAC covers everything before MESSAGE-INTEGRITY, with a length that
    // ends at it.
    uint8_t header[4];
    rtc_put16(header, m->type);
    rtc_put16(header + 2, (uint32_t)(m->integrity_at - 20 + 24));
    uint8_t mac[20];
    rtc_hmac1 h;
    rtc_hmac1_init(&h, key, key_size);
    rtc_hmac1_add(&h, header, 4);
    rtc_hmac1_add(&h, m->data + 4, m->integrity_at - 4);
    rtc_hmac1_end(&h, mac);
    uint8_t diff = 0;
    for (int i = 0; i < 20; i++) diff |= (uint8_t)(mac[i] ^ m->data[m->integrity_at + 4 + (size_t)i]);
    return diff == 0;
}

void rtc_turn_key(const char *username, const char *realm, const char *password, uint8_t key[16])
{
    char text[768];
    const int n = snprintf(text, sizeof text, "%s:%s:%s", username, realm, password);
    rtc_md5(text, n > 0 && (size_t)n < sizeof text ? (size_t)n : 0, key);
}
