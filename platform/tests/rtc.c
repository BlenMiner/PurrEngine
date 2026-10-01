// The WebRTC stack (platform/src/rtc), piece by piece: crypto against values
// from another implementation (Node's crypto, and RFC 6979's P-256 vectors).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tide_test.h"
#include "rtc/rtc.h"

static size_t unhex(const char *text, uint8_t *out)
{
    size_t n = 0;
    for (; text[0] && text[1]; text += 2) {
        unsigned v = 0;
        sscanf(text, "%2x", &v);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static bool same(const uint8_t *got, const char *want)
{
    uint8_t expected[512];
    const size_t n = unhex(want, expected);
    return memcmp(got, expected, n) == 0;
}

// The same bytes Node hashed: i * 7 + 3.
static void pattern(uint8_t *out, const size_t n)
{
    for (size_t i = 0; i < n; i++) out[i] = (uint8_t)(i * 7 + 3);
}

TIDE_TEST(rtc_hashes)
{
    static const struct {
        size_t size;
        const char *sha256, *sha1, *md5;
    } cases[] = {
        {0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "da39a3ee5e6b4b0d3255bfef95601890afd80709",
         "d41d8cd98f00b204e9800998ecf8427e"},
        {3, "6ab0dba1f4f1dfbb37b4f9eeb092c09fca4900ad32bdcd147d8dde35d6c87c35", "4201de9f98cb0a9b8cf52398be7802b55d45266a",
         "c9aee4810523ef8658121b8d492c6b41"},
        {55, "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b", "ddf57317ef34bfee3b6df83d359098930eb278bc",
         "52c0e574e1198de5fe3f8f11440dcb1b"},
        {56, "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27", "a0d492bb0fc889d0eca3bc137066ab6f4f74f369",
         "46c9907fc908ee68b1e7b8e71286a518"},
        {63, "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055", "c55856749bef509bdfe6bfebfc7bf4e793e82132",
         "a62f6d59e837867693f042f5b8f5a236"},
        {64, "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241", "bede92be29c3874e1b54ddc77988d606fc857a8e",
         "7160b8fb5e9e4023d549c3971fbaeead"},
        {65, "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e", "b05a80522b053d6dc7e0a517d0e70212c7dad11f",
         "70bd662e7aefbda85a0f7244167b7897"},
        {1000, "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371", "4231a8a50a10fa9758db8ec71fdef855b751048a",
         "10046f077f2082ac19676b8079f1cb1a"},
    };
    uint8_t data[1000];
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        pattern(data, cases[c].size);
        uint8_t out[32];
        rtc_sha256_of(data, cases[c].size, out);
        TIDE_CHECK(same(out, cases[c].sha256));
        // In pieces too, split where blocks don't
        rtc_sha256 s;
        rtc_sha256_init(&s);
        const size_t half = cases[c].size / 3;
        rtc_sha256_add(&s, data, half);
        rtc_sha256_add(&s, data + half, cases[c].size - half);
        rtc_sha256_end(&s, out);
        TIDE_CHECK(same(out, cases[c].sha256));
        rtc_sha1 s1;
        rtc_sha1_init(&s1);
        rtc_sha1_add(&s1, data, cases[c].size);
        rtc_sha1_end(&s1, out);
        TIDE_CHECK(same(out, cases[c].sha1));
        rtc_md5(data, cases[c].size, out);
        TIDE_CHECK(same(out, cases[c].md5));
    }
}

TIDE_TEST(rtc_hmac_and_prf)
{
    static const struct {
        size_t key;
        const char *sha256, *sha1;
    } cases[] = {
        {20, "ea77c59f75158580c85fbacb06e2880aa53ad73138a0cd11cd4e511608d40366", "50e85c539cb4852acf27a6f399c27f39e40cecbe"},
        {64, "40c50c23a74b01adbf98e9896c9ee0217c4655facc12a6d7ae681a94e7cad088", "d1d6155fd978f9742514461dfe4e29d1e36f63dc"},
        {100, "7c1710ebc8e44e697c29fdc4cc6de3ddb66df5aa94fe2fe0ae330f751bcf2b01", "532fa0b9f45668b4672067384412204fc0c05dfc"},
    };
    uint8_t data[150], key[100];
    pattern(data, sizeof data);
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        pattern(key, cases[c].key);
        for (size_t i = 0; i < cases[c].key; i++) key[i] ^= 0x55;
        uint8_t out[32];
        rtc_hmac256 h;
        rtc_hmac256_init(&h, key, cases[c].key);
        rtc_hmac256_add(&h, data, sizeof data);
        rtc_hmac256_end(&h, out);
        TIDE_CHECK(same(out, cases[c].sha256));
        rtc_hmac1 h1;
        rtc_hmac1_init(&h1, key, cases[c].key);
        rtc_hmac1_add(&h1, data, sizeof data);
        rtc_hmac1_end(&h1, out);
        TIDE_CHECK(same(out, cases[c].sha1));
    }

    uint8_t secret[48], seed[64], out[100];
    pattern(secret, sizeof secret);
    pattern(seed, sizeof seed);
    rtc_prf(secret, sizeof secret, "master secret", seed, 20, seed + 20, 44, out, sizeof out);
    TIDE_CHECK(same(out, "e310350d417bd405e4e88b7cde891fbb55fdcaa5c1f5884b33f7eaae1970ffa77bd40f2e444bf30e48b120752bf58e04"
                         "1b4e4352c0b9c8dfed9672d9c4d05059963a1a1410cca680c34b6d2769fd8451962116f605f504b69009b3a08f3f2525"
                         "de3aec99"));
}

TIDE_TEST(rtc_chacha20_poly1305)
{
    uint8_t key[32], nonce[12], ad[13], text[300], sealed[316], opened[300];
    pattern(key, sizeof key);
    pattern(nonce, sizeof nonce);
    for (size_t i = 0; i < sizeof nonce; i++) nonce[i] ^= 0x33;
    pattern(ad, sizeof ad);
    pattern(text, sizeof text);
    rtc_seal(key, nonce, ad, sizeof ad, text, sizeof text, sealed);
    TIDE_CHECK(same(sealed,
                    "f8006bd555535db2a21cad8f2bd4cdbb79710694884f16b1abf21b3b225bfa320605e8fe76457bc31a500c53711d0c85"
                    "8577338c0678f81ca18ab62ca8b2c277c0e0f87a29d6a3f8cae951dae26021be7afeb29cf7f657be4f8feb0b1faa934c"
                    "5048af030bc121c17f1dfc47aa7ed6bc94ed8951740dc8de560ec568ba4dd6f537085eb2e1de4f631318de89d98f0752"
                    "98ed4366f5df60caa5e69c9c12d84daecebd306d75fb52c4a5769525ed5b37e1ac5cab754ff3ae472a353aa8c693be8a"
                    "d6144efa148a57d4ca7a10df4eb83792c72990ca45cf0528f2786b1458f2571d9e7a899209c9aa57d234ae3ae0f97673"
                    "c69c2e4b427f5ef57b9c446884eba1ab6fd603b8fd83040326121b566702cccbf08bd6863f0eccada2d48fca00672baa"
                    "5c9c9e96bb6d8a0a8bc726a3f342e56d3d9a427de48d873cbfc10bd2"));
    TIDE_CHECK(rtc_open(key, nonce, ad, sizeof ad, sealed, sizeof sealed, opened));
    TIDE_CHECK(memcmp(opened, text, sizeof text) == 0);
    sealed[100] ^= 1; // Tampered: refused
    TIDE_CHECK(!rtc_open(key, nonce, ad, sizeof ad, sealed, sizeof sealed, opened));
    sealed[100] ^= 1;
    ad[0] ^= 1;
    TIDE_CHECK(!rtc_open(key, nonce, ad, sizeof ad, sealed, sizeof sealed, opened));
}

TIDE_TEST(rtc_p256)
{
    uint8_t d[32], e[32], public_key[65], secret[32];
    unhex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", d);
    unhex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721", e); // RFC 6979's key

    uint8_t d_public[65], e_public[65];
    unhex("04515c3d6eb9e396b904d3feca7f54fdcd0cc1e997bf375dca515ad0a6c3b4035f4536be3a50f318fbf9a5475902a221502bef0d57e0"
          "8c53b2cc0a56f17d9f9354",
          d_public);
    unhex("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb67903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e"
          "9f5177a3c294d4462299",
          e_public);
    // ECDH both ways, which also proves each public key is right (d times the
    // curve's base point, through e's side)
    TIDE_REQUIRE(rtc_p256_ecdh(d, e_public, secret));
    TIDE_CHECK(same(secret, "cf551a5f5d50b264e06ee9c4f7f541aa0318be11d12577b3857c8b5c625f935a"));
    TIDE_REQUIRE(rtc_p256_ecdh(e, d_public, secret));
    TIDE_CHECK(same(secret, "cf551a5f5d50b264e06ee9c4f7f541aa0318be11d12577b3857c8b5c625f935a"));

    // RFC 6979, A.2.5: SHA-256 of "sample", signed deterministically
    uint8_t hash[32], sig[64];
    rtc_sha256_of("sample", 6, hash);
    TIDE_REQUIRE(rtc_p256_sign(e, hash, sig));
    TIDE_CHECK(same(sig, "efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                         "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8"));
    TIDE_CHECK(rtc_p256_verify(e_public, hash, sig));

    // A signature Node made with d, over SHA-256 of "tide"
    rtc_sha256_of("tide", 4, hash);
    unhex("7943bf15f5013f032890d0c60f9a7115ff16e2ee3b91a88ae5ddc807c6d2bb7370903874feeebce47bdba4523c2fc6b36819d6721143cd"
          "50d209645acdb33caa",
          sig);
    TIDE_CHECK(rtc_p256_verify(d_public, hash, sig));
    TIDE_CHECK(!rtc_p256_verify(e_public, hash, sig)); // Someone else's key
    sig[5] ^= 1;
    TIDE_CHECK(!rtc_p256_verify(d_public, hash, sig));

    // Fresh keys: their own signatures verify, and ECDH agrees
    uint8_t a[32], a_public[65], b[32], b_public[65], s1[32], s2[32];
    TIDE_REQUIRE(rtc_p256_keys(a, a_public) && rtc_p256_keys(b, b_public));
    TIDE_CHECK(rtc_p256_ecdh(a, b_public, s1) && rtc_p256_ecdh(b, a_public, s2) && memcmp(s1, s2, 32) == 0);
    TIDE_CHECK(rtc_p256_sign(a, hash, sig) && rtc_p256_verify(a_public, hash, sig));

    // A point that isn't on the curve is refused
    public_key[0] = 4;
    memcpy(public_key + 1, d_public + 1, 64);
    public_key[64] ^= 1;
    TIDE_CHECK(!rtc_p256_ecdh(a, public_key, secret));
}

TIDE_TEST(rtc_crcs)
{
    TIDE_CHECK(rtc_crc32("123456789", 9) == 0xcbf43926u);
    TIDE_CHECK(rtc_crc32c("123456789", 9) == 0xe3069283u);
    uint8_t r[64] = {0};
    TIDE_CHECK(rtc_random(r, sizeof r));
    uint8_t any = 0;
    for (size_t i = 0; i < sizeof r; i++) any |= r[i];
    TIDE_CHECK(any != 0);
}

TIDE_TEST(rtc_certificates)
{
    rtc_identity id;
    TIDE_REQUIRE(rtc_identity_new(&id));
    // For a look with other tools: TIDE_RTC_CERT=<file> writes it out
    const char *path = getenv("TIDE_RTC_CERT");
    FILE *f = path ? fopen(path, "wb") : NULL;
    if (f) {
        fwrite(id.cert, 1, id.cert_size, f);
        fclose(f);
    }
    uint8_t key[65];
    TIDE_REQUIRE(rtc_cert_key(id.cert, id.cert_size, key));
    TIDE_CHECK(memcmp(key, id.public_key, 65) == 0);
    uint8_t fingerprint[32];
    rtc_sha256_of(id.cert, id.cert_size, fingerprint);
    TIDE_CHECK(memcmp(fingerprint, id.fingerprint, 32) == 0);

    // Signatures through DER and back, including ones whose numbers have
    // their top bit set or start with zeros
    uint8_t sig[64], back[64], der[72];
    for (int i = 0; i < 64; i++) sig[i] = (uint8_t)(i * 37);
    sig[0] = 0x80;
    sig[32] = 0;
    sig[33] = 0;
    const size_t n = rtc_sig_to_der(sig, der);
    TIDE_REQUIRE(n > 0);
    TIDE_CHECK(rtc_sig_from_der(der, n, back) && memcmp(sig, back, 64) == 0);
    TIDE_CHECK(!rtc_sig_from_der(der, n - 1, back));

}

// RFC 5769, 2.1: a request with MESSAGE-INTEGRITY and FINGERPRINT
TIDE_TEST(rtc_stun_sample_request)
{
    uint8_t message[108];
    const size_t n = unhex("000100582112a442b7e7a701bc34d686fa87dfae802200105354554e207465737420636c69656e74"
                           "002400046e0001ff80290008932ff9b151263b36000600096576746a3a68367659202020"
                           "000800149aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a280280004e57a3bcf",
                           message);
    TIDE_REQUIRE(n == sizeof message);
    rtc_stun m;
    TIDE_REQUIRE(rtc_stun_parse(message, n, &m));
    TIDE_CHECK(m.type == RTC_STUN_BINDING_REQUEST && m.priority == 0x6e0001ffu && m.controlled);
    TIDE_CHECK(m.username.size == 9 && memcmp(m.username.data, "evtj:h6vY", 9) == 0);
    TIDE_CHECK(rtc_stun_check(&m, "VOkJxbRl1RmTxUk/WvJxBt", 22));
    TIDE_CHECK(!rtc_stun_check(&m, "VOkJxbRl1RmTxUk/WvJxBu", 22));

    // Written by us, read back
    uint8_t out[200];
    rtc_stun_writer w;
    rtc_stun_begin(&w, out, sizeof out, RTC_STUN_BINDING_SUCCESS, m.id);
    rtc_stun_attr_addr(&w, RTC_STUN_XOR_MAPPED_ADDRESS, (rtc_addr){0xc0a80105u, 32853});
    rtc_stun_integrity(&w, "secret", 6);
    rtc_stun_fingerprint(&w);
    TIDE_REQUIRE(!w.overflow);
    rtc_stun back;
    TIDE_REQUIRE(rtc_stun_parse(out, w.size, &back));
    TIDE_CHECK(back.has_mapped && back.mapped.ip == 0xc0a80105u && back.mapped.port == 32853);
    TIDE_CHECK(rtc_stun_check(&back, "secret", 6));
    out[w.size - 1] ^= 1; // A broken FINGERPRINT fails the check too
    TIDE_REQUIRE(rtc_stun_parse(out, w.size, &back));
    TIDE_CHECK(!rtc_stun_check(&back, "secret", 6));
}

TIDE_TEST(rtc_json)
{
    static const char text[] = " {\"relay\": 1, \"ice\": [{\"urls\": [\"stun:a:3478\", \"turn:b:3478?transport=udp\"]},"
                               " {\"urls\": \"stun:c\", \"username\": \"u\\\"s\\n\\u00e9\"}], \"nested\": {\"x\": [1, {}]},"
                               " \"from\": 3} ";
    const rtc_json m = rtc_json_of(text, sizeof text - 1);
    TIDE_REQUIRE(m.text != NULL);
    double n = 0;
    TIDE_CHECK(rtc_json_number(rtc_json_get(m, "relay"), &n) && n == 1);
    TIDE_CHECK(rtc_json_number(rtc_json_get(m, "from"), &n) && n == 3);
    TIDE_CHECK(rtc_json_get(m, "missing").text == NULL);
    const rtc_json ice = rtc_json_get(m, "ice");
    const char *at = NULL;
    rtc_json server;
    int servers = 0;
    char s[64];
    while (rtc_json_next(ice, &at, &server)) {
        servers++;
        const rtc_json urls = rtc_json_get(server, "urls");
        if (servers == 1) {
            const char *u = NULL;
            rtc_json url;
            TIDE_CHECK(rtc_json_next(urls, &u, &url) && rtc_json_string(url, s, sizeof s) && strcmp(s, "stun:a:3478") == 0);
            TIDE_CHECK(rtc_json_next(urls, &u, &url) && rtc_json_string(url, s, sizeof s)
                       && strcmp(s, "turn:b:3478?transport=udp") == 0);
            TIDE_CHECK(!rtc_json_next(urls, &u, &url));
        } else {
            TIDE_CHECK(rtc_json_string(urls, s, sizeof s) && strcmp(s, "stun:c") == 0);
            TIDE_CHECK(rtc_json_string(rtc_json_get(server, "username"), s, sizeof s)
                       && strcmp(s, "u\"s\n\xc3\xa9") == 0);
        }
    }
    TIDE_CHECK(servers == 2);
    TIDE_CHECK(rtc_json_of("{\"a\": ", 6).text == NULL); // Cut short

    char buffer[64];
    rtc_text t = {buffer, 0, sizeof buffer, false};
    rtc_text_put(&t, "{\"sdp\":");
    rtc_text_json_string(&t, "v=0\r\n\"quoted\"");
    rtc_text_put(&t, "}");
    TIDE_CHECK(!t.overflow && strcmp(buffer, "{\"sdp\":\"v=0\\r\\n\\\"quoted\\\"\"}") == 0);
    const rtc_json back = rtc_json_of(buffer, t.size);
    TIDE_CHECK(rtc_json_string(rtc_json_get(back, "sdp"), s, sizeof s) && strcmp(s, "v=0\r\n\"quoted\"") == 0);
}

TIDE_TEST(rtc_sdp)
{
    // What Chrome writes for a data channel (its candidate lines shortened)
    static const char chrome[] = "v=0\r\no=- 4611731400430051336 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
                                 "a=group:BUNDLE 0\r\na=extmap-allow-mixed\r\na=msid-semantic: WMS\r\n"
                                 "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\nc=IN IP4 0.0.0.0\r\n"
                                 "a=candidate:1467250027 1 udp 2122260223 192.168.1.5 46243 typ host generation 0\r\n"
                                 "a=candidate:23456 1 udp 2122194687 7ab3c5e1-2d44.local 51234 typ host generation 0\r\n"
                                 "a=candidate:842163049 1 udp 1686052607 203.0.113.4 46243 typ srflx raddr 192.168.1.5 "
                                 "rport 46243 generation 0\r\n"
                                 "a=ice-ufrag:SZzk\r\na=ice-pwd:CZjoUwvw/U4XnFw95ItXGrfQ\r\na=ice-options:trickle\r\n"
                                 "a=fingerprint:sha-256 7B:8B:F0:65:5F:78:E2:51:3B:AC:6F:F3:3F:46:1B:35:DC:B8:5F:64:1A:"
                                 "24:C2:43:F0:A1:58:D0:A1:2C:19:08\r\n"
                                 "a=setup:actpass\r\na=mid:0\r\na=sctp-port:5000\r\na=max-message-size:262144\r\n";
    rtc_description d;
    TIDE_REQUIRE(rtc_sdp_parse(chrome, &d));
    TIDE_CHECK(strcmp(d.ufrag, "SZzk") == 0 && strcmp(d.pwd, "CZjoUwvw/U4XnFw95ItXGrfQ") == 0);
    TIDE_CHECK(d.fingerprint[0] == 0x7b && d.fingerprint[31] == 0x08);
    TIDE_CHECK(d.setup == RTC_SETUP_ACTPASS && strcmp(d.mid, "0") == 0 && d.sctp_port == 5000);
    TIDE_REQUIRE(d.candidate_count == 2); // Not the .local one: its checks will show where it is
    TIDE_CHECK(d.candidates[0].addr.ip == 0xc0a80105u && d.candidates[0].addr.port == 46243);
    TIDE_CHECK(d.candidates[1].type == RTC_SRFLX && d.candidates[1].priority == 1686052607u);

    // Ours, read back
    char buffer[2048];
    rtc_text t = {buffer, 0, sizeof buffer, false};
    d.setup = RTC_SETUP_ACTIVE;
    rtc_sdp_write(&t, &d, 42);
    TIDE_REQUIRE(!t.overflow);
    rtc_description back;
    TIDE_REQUIRE(rtc_sdp_parse(buffer, &back));
    TIDE_CHECK(strcmp(back.ufrag, d.ufrag) == 0 && strcmp(back.pwd, d.pwd) == 0);
    TIDE_CHECK(memcmp(back.fingerprint, d.fingerprint, 32) == 0 && back.setup == RTC_SETUP_ACTIVE);
    TIDE_CHECK(back.candidate_count == 2 && rtc_addr_equal(back.candidates[1].addr, d.candidates[1].addr));
}

// Two agents on this machine find each other and carry datagrams both ways.
TIDE_TEST(rtc_ice_on_this_machine)
{
    const rtc_ice_config none = {0};
    rtc_ice a, b;
    TIDE_REQUIRE(rtc_ice_start(&a, true, &none) && rtc_ice_start(&b, false, &none));
    rtc_ice_set_remote(&a, b.ufrag, b.pwd);
    rtc_ice_set_remote(&b, a.ufrag, a.pwd);
    rtc_candidate c;
    while (rtc_ice_next_local(&a, &c)) rtc_ice_add_remote(&b, &c);
    while (rtc_ice_next_local(&b, &c)) rtc_ice_add_remote(&a, &c);
    // And each on loopback, for machines without a network
    rtc_candidate loop = {{0x7f000001u, rtc_socket_port(a.socket)}, 2130706431u, RTC_HOST, "9"};
    rtc_ice_add_remote(&b, &loop);
    loop.addr.port = rtc_socket_port(b.socket);
    rtc_ice_add_remote(&a, &loop);

    uint8_t got[64];
    bool a_got = false, b_got = false;
    const double start = rtc_now();
    while (rtc_now() - start < 5.0 && !(a_got && b_got)) {
        const double now = rtc_now();
        rtc_ice_update(&a, now);
        rtc_ice_update(&b, now);
        size_t n;
        while ((n = rtc_ice_receive(&a, now, got, sizeof got)) != 0) a_got |= n == 4 && memcmp(got, "pong", 4) == 0;
        while ((n = rtc_ice_receive(&b, now, got, sizeof got)) != 0) b_got |= n == 4 && memcmp(got, "ping", 4) == 0;
        rtc_ice_send(&a, "ping", 4);
        rtc_ice_send(&b, "pong", 4);
    }
    TIDE_CHECK(a.state == RTC_ICE_CONNECTED && b.state == RTC_ICE_CONNECTED);
    TIDE_CHECK(a_got && b_got);
    rtc_ice_close(&a);
    rtc_ice_close(&b);
}

// DTLS between our client and our server, over a wire that loses datagrams.
typedef struct wire {
    uint8_t datagrams[64][2048];
    size_t sizes[64];
    int count;
    int sent;
    int lose_every; // Every nth datagram is lost; 0 for none
    char got[64];   // Application data received
} wire;

static void wire_send(void *user, const void *data, const size_t size)
{
    wire *w = user;
    w->sent++;
    if (w->lose_every && w->sent % w->lose_every == 0) return;
    if (w->count < 64 && size <= sizeof w->datagrams[0]) {
        memcpy(w->datagrams[w->count], data, size);
        w->sizes[w->count++] = size;
    }
}

static void wire_data(void *user, const void *data, const size_t size)
{
    wire *w = user;
    snprintf(w->got, sizeof w->got, "%.*s", (int)size, (const char *)data);
}

// What `from` sent, into `to`.
static void deliver(wire *from, rtc_dtls *to, const double now)
{
    for (int i = 0; i < from->count; i++) rtc_dtls_receive(to, from->datagrams[i], from->sizes[i], now);
    from->count = 0;
}

static void dtls_pair(const int lose_every)
{
    rtc_identity client_id, server_id;
    TIDE_REQUIRE(rtc_identity_new(&client_id) && rtc_identity_new(&server_id));
    wire to_server = {.lose_every = lose_every}, to_client = {.lose_every = lose_every};
    rtc_dtls client, server;
    double now = 0.0;
    // Each sends to the other's wire; each wire records what arrives for its end
    TIDE_REQUIRE(rtc_dtls_start(&server, false, &server_id, client_id.fingerprint, wire_send, wire_data, &to_client, now));
    TIDE_REQUIRE(rtc_dtls_start(&client, true, &client_id, server_id.fingerprint, wire_send, wire_data, &to_server, now));
    for (int step = 0; step < 400 && !(client.state == RTC_DTLS_OPEN && server.state == RTC_DTLS_OPEN); step++) {
        now += 0.05;
        deliver(&to_server, &server, now);
        deliver(&to_client, &client, now);
        rtc_dtls_update(&client, now);
        rtc_dtls_update(&server, now);
    }
    TIDE_REQUIRE(client.state == RTC_DTLS_OPEN && server.state == RTC_DTLS_OPEN);
    TIDE_CHECK(memcmp(client.master, server.master, 48) == 0);
    to_server.lose_every = to_client.lose_every = 0;
    rtc_dtls_send(&client, "hello", 5);
    rtc_dtls_send(&server, "world", 5);
    // The wires hold what each end sent; each end's `got` is filled by the other's
    deliver(&to_server, &server, now);
    deliver(&to_client, &client, now);
    TIDE_CHECK(strcmp(to_client.got, "hello") == 0); // The server's data callback writes into its user: to_client
    TIDE_CHECK(strcmp(to_server.got, "world") == 0);
}

TIDE_TEST(rtc_dtls_handshake)
{
    dtls_pair(0);
}

TIDE_TEST(rtc_dtls_handshake_losing_datagrams)
{
    dtls_pair(3);
}

// SCTP between two of ours, over a wire that loses packets: the association,
// the channel's open and ack, then messages one way and the other, big ones
// in pieces too.
typedef struct sctp_end {
    rtc_sctp sctp;
    struct sctp_end *other;
    uint8_t packets[256][1500];
    size_t sizes[256];
    int count;
    int sent;
    int lose_every;
    int messages;
    size_t last_size;
} sctp_end;

static void sctp_wire(void *user, const void *data, const size_t size)
{
    sctp_end *e = user;
    e->sent++;
    if (e->lose_every && e->sent % e->lose_every == 0) return;
    if (e->other->count < 256) {
        memcpy(e->other->packets[e->other->count], data, size);
        e->other->sizes[e->other->count++] = size;
    }
}

static void sctp_message(void *user, const void *data, const size_t size)
{
    sctp_end *e = user;
    (void)data;
    e->messages++;
    e->last_size = size;
}

static void sctp_run(sctp_end *a, sctp_end *b, double *now, const int steps)
{
    for (int i = 0; i < steps; i++) {
        *now += 0.01;
        for (int k = 0; k < a->count; k++) rtc_sctp_receive(&a->sctp, a->packets[k], a->sizes[k], *now);
        a->count = 0;
        for (int k = 0; k < b->count; k++) rtc_sctp_receive(&b->sctp, b->packets[k], b->sizes[k], *now);
        b->count = 0;
        rtc_sctp_update(&a->sctp, *now);
        rtc_sctp_update(&b->sctp, *now);
    }
}

TIDE_TEST(rtc_sctp_channel)
{
    static sctp_end a, b;
    memset(&a, 0, sizeof a);
    memset(&b, 0, sizeof b);
    a.other = &b;
    b.other = &a;
    a.lose_every = 5;
    double now = 0.0;
    rtc_sctp_start(&a.sctp, true, 5000, sctp_wire, sctp_message, &a, now);
    rtc_sctp_start(&b.sctp, false, 5000, sctp_wire, sctp_message, &b, now);
    rtc_sctp_open_channel(&a.sctp, 0, now); // The one who joins opens it
    sctp_run(&a, &b, &now, 300);
    TIDE_REQUIRE(a.sctp.state == RTC_SCTP_OPEN && b.sctp.state == RTC_SCTP_OPEN);
    TIDE_REQUIRE(a.sctp.channel && b.sctp.channel);

    uint8_t message[1200] = {1, 2, 3};
    for (int i = 0; i < 100; i++) {
        rtc_sctp_send(&a.sctp, message, 1200, now);
        rtc_sctp_send(&b.sctp, message, 20, now);
        sctp_run(&a, &b, &now, 1);
    }
    sctp_run(&a, &b, &now, 100);
    // One in five of a's packets is lost, and never sent again
    TIDE_CHECK(b.messages >= 70 && b.messages <= 90);
    TIDE_CHECK(a.messages == 100);
    TIDE_CHECK(b.last_size == 1200 && a.last_size == 20);
    // Nothing stays waiting: what a gave up on, b stopped waiting for
    TIDE_CHECK(b.sctp.cumulative == a.sctp.next_tsn - 1);
    TIDE_CHECK(a.sctp.sent_count == 0);
}

// Signals from one peer to the other, as the relay would carry them.
static void pass_signals(rtc_peer *from, rtc_peer *to)
{
    static char json[8192], type[16], sdp[4096], candidate[256];
    while (rtc_peer_next_signal(from, json, sizeof json)) {
        const rtc_json m = rtc_json_of(json, strlen(json));
        const rtc_json d = rtc_json_get(m, "description");
        const rtc_json c = rtc_json_get(m, "candidate");
        if (d.text && rtc_json_string(rtc_json_get(d, "type"), type, sizeof type)
            && rtc_json_string(rtc_json_get(d, "sdp"), sdp, sizeof sdp)) {
            rtc_peer_description(to, type, sdp);
        }
        if (c.text && rtc_json_string(rtc_json_get(c, "candidate"), candidate, sizeof candidate)) {
            rtc_peer_candidate(to, candidate);
        }
    }
}

// Two peers on this machine: the whole of it, ICE, DTLS, SCTP and the
// channel, over real UDP.
TIDE_TEST(rtc_peers_on_this_machine)
{
    const rtc_ice_config none = {0};
    rtc_peer *joiner = rtc_peer_new(true, &none);
    rtc_peer *host = rtc_peer_new(false, &none);
    TIDE_REQUIRE(joiner && host);
    int to_host = 0, to_joiner = 0;
    const double start = rtc_now();
    while (rtc_now() - start < 10.0 && (to_host < 10 || to_joiner < 10)) {
        const double now = rtc_now();
        pass_signals(joiner, host);
        pass_signals(host, joiner);
        rtc_peer_update(joiner, now);
        rtc_peer_update(host, now);
        uint8_t got[1200];
        size_t n;
        while ((n = rtc_peer_receive(host, got, sizeof got)) != 0) to_host += n == 1000;
        while ((n = rtc_peer_receive(joiner, got, sizeof got)) != 0) to_joiner += n == 3;
        static const uint8_t big[1000];
        rtc_peer_send(joiner, big, sizeof big);
        rtc_peer_send(host, "hey", 3);
    }
    TIDE_CHECK(joiner->state == RTC_PEER_OPEN && host->state == RTC_PEER_OPEN);
    TIDE_CHECK(to_host >= 10 && to_joiner >= 10);
    TIDE_CHECK(joiner->dtls_client != host->dtls_client);
    rtc_peer_free(joiner);
    rtc_peer_free(host);
}

TIDE_TEST(rtc_dtls_refuses_the_wrong_certificate)
{
    rtc_identity client_id, server_id, someone;
    TIDE_REQUIRE(rtc_identity_new(&client_id) && rtc_identity_new(&server_id) && rtc_identity_new(&someone));
    wire to_server = {0}, to_client = {0};
    rtc_dtls client, server;
    double now = 0.0;
    TIDE_REQUIRE(rtc_dtls_start(&server, false, &server_id, client_id.fingerprint, wire_send, wire_data, &to_client, now));
    // The client expects someone else's certificate
    TIDE_REQUIRE(rtc_dtls_start(&client, true, &client_id, someone.fingerprint, wire_send, wire_data, &to_server, now));
    for (int step = 0; step < 100; step++) {
        now += 0.05;
        deliver(&to_server, &server, now);
        deliver(&to_client, &client, now);
        rtc_dtls_update(&client, now);
        rtc_dtls_update(&server, now);
    }
    TIDE_CHECK(client.state == RTC_DTLS_FAILED && server.state != RTC_DTLS_OPEN);
}
