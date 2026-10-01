#pragma once

// WebRTC data channels for desktop games, written for Tide with no code
// from elsewhere: what rooms need to reach browsers, and each other, directly
// (see platform/src/rooms.c). Inside the platform layer only.
//
// - Crypto: hashes, HMAC, TLS 1.2's PRF, ChaCha20-Poly1305, P-256 (ECDH and
//   ECDSA), CRCs and the system's random bytes.
// - STUN and ICE: finding a way between two machines, through their routers,
//   or through a TURN server.
// - DTLS 1.2: the encryption WebRTC requires, over ICE.
// - SCTP: the data channels, over DTLS.
//
// Temporary implementation written by Claude; the project owner takes it over
// later.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Hashes

typedef struct rtc_sha256 {
    uint32_t h[8];
    uint64_t bytes;
    uint8_t block[64];
} rtc_sha256;

void rtc_sha256_init(rtc_sha256 *s);
void rtc_sha256_add(rtc_sha256 *s, const void *data, size_t size);
void rtc_sha256_end(rtc_sha256 *s, uint8_t out[32]);
void rtc_sha256_of(const void *data, size_t size, uint8_t out[32]);

typedef struct rtc_sha1 {
    uint32_t h[5];
    uint64_t bytes;
    uint8_t block[64];
} rtc_sha1;

void rtc_sha1_init(rtc_sha1 *s);
void rtc_sha1_add(rtc_sha1 *s, const void *data, size_t size);
void rtc_sha1_end(rtc_sha1 *s, uint8_t out[20]);

// TURN's long-term credentials still key their HMACs with MD5.
void rtc_md5(const void *data, size_t size, uint8_t out[16]);

// HMAC, in pieces: start with the key, add the message, end.
typedef struct rtc_hmac256 {
    rtc_sha256 inner;
    rtc_sha256 outer;
} rtc_hmac256;

void rtc_hmac256_init(rtc_hmac256 *h, const void *key, size_t key_size);
void rtc_hmac256_add(rtc_hmac256 *h, const void *data, size_t size);
void rtc_hmac256_end(rtc_hmac256 *h, uint8_t out[32]);

typedef struct rtc_hmac1 {
    rtc_sha1 inner;
    rtc_sha1 outer;
} rtc_hmac1;

void rtc_hmac1_init(rtc_hmac1 *h, const void *key, size_t key_size);
void rtc_hmac1_add(rtc_hmac1 *h, const void *data, size_t size);
void rtc_hmac1_end(rtc_hmac1 *h, uint8_t out[20]);

// TLS 1.2's PRF with SHA-256 (RFC 5246, 5): `size` bytes from the secret, a
// label and a seed, which comes in two pieces (either may be empty).
void rtc_prf(const uint8_t *secret, size_t secret_size, const char *label, const uint8_t *seed1, size_t seed1_size,
             const uint8_t *seed2, size_t seed2_size, uint8_t *out, size_t size);

// ---------------------------------------------------------------------------
// ChaCha20-Poly1305 (RFC 8439): `out` gets the text and a 16-byte tag after
// it. Opening checks the tag, in constant time, and writes nothing if it's
// wrong. `out` may be `text`.

void rtc_seal(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_size, const uint8_t *text,
              size_t size, uint8_t *out);
bool rtc_open(const uint8_t key[32], const uint8_t nonce[12], const uint8_t *ad, size_t ad_size, const uint8_t *sealed,
              size_t sealed_size, uint8_t *out);

// ---------------------------------------------------------------------------
// P-256 (secp256r1). Private keys are 32 bytes, big-endian, below the curve's
// order. Public keys are 65: 4, then x and y, big-endian.

bool rtc_p256_keys(uint8_t private_key[32], uint8_t public_key[65]);
// A private key's public key: false if it isn't one.
bool rtc_p256_public(const uint8_t private_key[32], uint8_t public_key[65]);
// The shared secret: x of private_key times their key, which must be a point
// on the curve. False if it isn't.
bool rtc_p256_ecdh(const uint8_t private_key[32], const uint8_t their_key[65], uint8_t secret[32]);
// ECDSA over a SHA-256 hash, with RFC 6979's deterministic nonces: `sig` is
// r then s, 32 bytes each.
bool rtc_p256_sign(const uint8_t private_key[32], const uint8_t hash[32], uint8_t sig[64]);
bool rtc_p256_verify(const uint8_t public_key[65], const uint8_t hash[32], const uint8_t sig[64]);

// ---------------------------------------------------------------------------
// Certificates (cert.c): each side's own, self-signed; the other checks its
// SHA-256 against the fingerprint in the description.

typedef struct rtc_identity {
    uint8_t private_key[32];
    uint8_t public_key[65];
    uint8_t cert[512]; // DER
    size_t cert_size;
    uint8_t fingerprint[32]; // SHA-256 of the certificate
} rtc_identity;

// A fresh key pair and a certificate for it.
bool rtc_identity_new(rtc_identity *id);
// A certificate's public key: false unless it's P-256.
bool rtc_cert_key(const uint8_t *cert, size_t size, uint8_t key[65]);
// ECDSA signatures (r and s) as TLS sends them, in DER: its size, at most 72.
size_t rtc_sig_to_der(const uint8_t sig[64], uint8_t out[72]);
bool rtc_sig_from_der(const uint8_t *der, size_t size, uint8_t sig[64]);

// ---------------------------------------------------------------------------
// Sockets and time (net.c). Nothing blocks, but looking up a name.

typedef intptr_t rtc_socket;
#define RTC_NO_SOCKET ((rtc_socket)-1)

typedef struct rtc_addr {
    uint32_t ip; // IPv4, in host order
    uint16_t port;
} rtc_addr;

static inline bool rtc_addr_equal(const rtc_addr a, const rtc_addr b)
{
    return a.ip == b.ip && a.port == b.port;
}

double rtc_now(void); // Seconds, from some point
// A UDP socket on a port of its own, or RTC_NO_SOCKET: on every address, or
// on loopback only when TIDE_RTC_LOCAL is set (for tests).
rtc_socket rtc_udp_open(void);
bool rtc_local_only(void);
uint16_t rtc_socket_port(rtc_socket s);
void rtc_udp_send(rtc_socket s, rtc_addr to, const void *data, size_t size);
// The next datagram: its size, or 0 when there's none.
size_t rtc_udp_receive(rtc_socket s, rtc_addr *from, void *out, size_t capacity);
void rtc_socket_close(rtc_socket s);
// The address this machine reaches the internet from.
bool rtc_local_ip(uint32_t *ip);
// A name's IPv4 address. It waits for the answer.
bool rtc_resolve(const char *host, uint16_t port, rtc_addr *out);
// TCP: connecting starts at once; rtc_tcp_connected says 1 once it has, 0
// while it's trying and -1 if it can't. Sending and receiving give the bytes
// they moved, 0 if they'd have to wait, and -1 once the connection is gone.
rtc_socket rtc_tcp_connect(rtc_addr to);
int rtc_tcp_connected(rtc_socket s);
int rtc_tcp_send(rtc_socket s, const void *data, size_t size);
int rtc_tcp_receive(rtc_socket s, void *out, size_t capacity);

// ---------------------------------------------------------------------------
// STUN (stun.c), for ICE and TURN

enum {
    // Message types: a method and a class
    RTC_STUN_BINDING_REQUEST = 0x0001,
    RTC_STUN_BINDING_SUCCESS = 0x0101,
    RTC_STUN_BINDING_ERROR = 0x0111,
    RTC_STUN_ALLOCATE_REQUEST = 0x0003,
    RTC_STUN_ALLOCATE_SUCCESS = 0x0103,
    RTC_STUN_ALLOCATE_ERROR = 0x0113,
    RTC_STUN_REFRESH_REQUEST = 0x0004,
    RTC_STUN_REFRESH_SUCCESS = 0x0104,
    RTC_STUN_REFRESH_ERROR = 0x0114,
    RTC_STUN_SEND_INDICATION = 0x0016,
    RTC_STUN_DATA_INDICATION = 0x0017,
    RTC_STUN_PERMISSION_REQUEST = 0x0008,
    RTC_STUN_PERMISSION_SUCCESS = 0x0108,
    RTC_STUN_PERMISSION_ERROR = 0x0118,

    // Attributes
    RTC_STUN_USERNAME = 0x0006,
    RTC_STUN_MESSAGE_INTEGRITY = 0x0008,
    RTC_STUN_ERROR_CODE = 0x0009,
    RTC_STUN_LIFETIME = 0x000d,
    RTC_STUN_XOR_PEER_ADDRESS = 0x0012,
    RTC_STUN_DATA = 0x0013,
    RTC_STUN_REALM = 0x0014,
    RTC_STUN_NONCE = 0x0015,
    RTC_STUN_XOR_RELAYED_ADDRESS = 0x0016,
    RTC_STUN_REQUESTED_TRANSPORT = 0x0019,
    RTC_STUN_XOR_MAPPED_ADDRESS = 0x0020,
    RTC_STUN_PRIORITY = 0x0024,
    RTC_STUN_USE_CANDIDATE = 0x0025,
    RTC_STUN_FINGERPRINT = 0x8028,
    RTC_STUN_ICE_CONTROLLED = 0x8029,
    RTC_STUN_ICE_CONTROLLING = 0x802a,
};

typedef struct rtc_stun_writer {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool overflow;
} rtc_stun_writer;

void rtc_stun_begin(rtc_stun_writer *w, uint8_t *buffer, size_t capacity, uint16_t type, const uint8_t id[12]);
void rtc_stun_attr(rtc_stun_writer *w, uint16_t type, const void *value, size_t size);
void rtc_stun_attr32(rtc_stun_writer *w, uint16_t type, uint32_t value);
void rtc_stun_attr64(rtc_stun_writer *w, uint16_t type, uint64_t value);
void rtc_stun_attr_addr(rtc_stun_writer *w, uint16_t type, rtc_addr a); // XOR-ed, as every *-ADDRESS we write is
// MESSAGE-INTEGRITY, then FINGERPRINT: the last two attributes, in that order.
void rtc_stun_integrity(rtc_stun_writer *w, const void *key, size_t key_size);
void rtc_stun_fingerprint(rtc_stun_writer *w);

typedef struct rtc_stun_value {
    const uint8_t *data;
    size_t size;
} rtc_stun_value;

// A message read: what ICE and TURN look at of it.
typedef struct rtc_stun {
    const uint8_t *data;
    size_t size;
    uint16_t type;
    uint8_t id[12];
    rtc_stun_value username, realm, nonce, payload;
    uint32_t error; // ERROR-CODE, like 401
    uint32_t lifetime;
    uint32_t priority;
    rtc_addr mapped, relayed, peer;
    bool has_mapped, has_relayed, has_peer;
    bool use_candidate, controlling, controlled;
    size_t integrity_at, fingerprint_at; // Where those attributes are, 0 if nowhere
} rtc_stun;

// Whether a datagram is STUN, from its first bytes.
bool rtc_stun_is(const uint8_t *p, size_t size);
bool rtc_stun_parse(const uint8_t *p, size_t size, rtc_stun *m);
bool rtc_stun_read_addr(rtc_stun_value v, rtc_addr *out);
// Its MESSAGE-INTEGRITY with `key` (and FINGERPRINT, if it has one) is right.
bool rtc_stun_check(const rtc_stun *m, const void *key, size_t key_size);
// A TURN server's long-term key: MD5 of "username:realm:password".
void rtc_turn_key(const char *username, const char *realm, const char *password, uint8_t key[16]);

// ---------------------------------------------------------------------------
// JSON (json.c), for the relay: a value is where it is in the text, which
// has to outlast it. A missing one has no text.

typedef struct rtc_json {
    const char *text;
    size_t size;
} rtc_json;

rtc_json rtc_json_of(const char *text, size_t size);
rtc_json rtc_json_get(rtc_json object, const char *key);
// An array's items, one at a time: start with *at NULL.
bool rtc_json_next(rtc_json array, const char **at, rtc_json *item);
bool rtc_json_is_string(rtc_json v);
// A string's text, unescaped, into `out`: false if it isn't one, or it's too long.
bool rtc_json_string(rtc_json v, char *out, size_t capacity);
bool rtc_json_number(rtc_json v, double *out);

// Text being written into a fixed buffer, always ending in a NUL.
typedef struct rtc_text {
    char *data;
    size_t size;
    size_t capacity;
    bool overflow;
} rtc_text;

void rtc_text_add(rtc_text *t, const char *s, size_t n);
void rtc_text_put(rtc_text *t, const char *s);
void rtc_text_json_string(rtc_text *t, const char *s); // Quoted and escaped

// ---------------------------------------------------------------------------
// Candidates and descriptions (sdp.c)

#define RTC_MAX_CANDIDATES 16

enum { RTC_HOST, RTC_SRFLX, RTC_PRFLX, RTC_RELAY }; // Candidate types, as SDP names them in order

// A way to reach an agent: IPv4 and UDP only, for now.
typedef struct rtc_candidate {
    rtc_addr addr;
    uint32_t priority;
    uint8_t type;
    char foundation[33];
} rtc_candidate;

// "candidate:..." (with "a=" or not): false for one we can't use, like the
// .local names browsers hide their addresses behind. They're found anyway:
// their checks come from those addresses (peer-reflexive candidates).
bool rtc_candidate_parse(const char *text, rtc_candidate *c);
void rtc_candidate_write(rtc_text *t, const rtc_candidate *c); // "candidate:..."

enum { RTC_SETUP_ACTPASS, RTC_SETUP_ACTIVE, RTC_SETUP_PASSIVE }; // Who's the DTLS client: active is

typedef struct rtc_description {
    char ufrag[64];
    char pwd[128];
    uint8_t fingerprint[32];
    bool has_fingerprint;
    int setup;
    char mid[32];
    uint16_t sctp_port;
    rtc_candidate candidates[RTC_MAX_CANDIDATES];
    int candidate_count;
} rtc_description;

// False unless it has what ICE and DTLS need.
bool rtc_sdp_parse(const char *sdp, rtc_description *d);
void rtc_sdp_write(rtc_text *t, const rtc_description *d, uint64_t session);

// ---------------------------------------------------------------------------
// ICE (ice.c): gathering this machine's candidates (its own address, the one
// its router shows the internet, through a STUN server, and one on a TURN
// server), checking pairs of theirs and ours until one works, and carrying
// datagrams on it. One UDP socket carries it all.

typedef struct rtc_ice_config {
    bool has_stun;
    rtc_addr stun;
    bool has_turn;
    rtc_addr turn;
    char username[256];
    char credential[256];
} rtc_ice_config;

enum { RTC_ICE_CHECKING, RTC_ICE_CONNECTED, RTC_ICE_FAILED };

#define RTC_MAX_PAIRS 32
#define RTC_MAX_PERMISSIONS 16

typedef struct rtc_pair {
    bool relayed; // From our TURN allocation, rather than our socket
    rtc_addr remote;
    uint32_t remote_priority;
    uint64_t priority;
    uint8_t state; // Waiting, in progress, succeeded or failed
    uint8_t id[12];
    double sent;
    int tries;
    bool nominated; // They picked it (USE-CANDIDATE)
} rtc_pair;

typedef struct rtc_ice {
    rtc_socket socket;
    int state;
    bool controlling;
    bool relay_only; // TIDE_RTC_RELAY_ONLY: through TURN or not at all, to test it
    uint64_t tiebreaker;
    char ufrag[9];
    char pwd[25];
    char remote_ufrag[64];
    char remote_pwd[128];
    bool has_remote;
    rtc_candidate locals[4];
    int local_count;
    int locals_told;
    rtc_candidate remotes[RTC_MAX_CANDIDATES];
    int remote_count;
    rtc_pair pairs[RTC_MAX_PAIRS];
    int pair_count;
    int selected; // The pair in use, or -1
    double started;
    double next_check;
    double last_heard;   // From the other side, on the selected pair
    double next_consent; // Checking they still want our packets
    rtc_ice_config config;
    // The STUN server, for our server-reflexive address
    uint8_t stun_id[12];
    double stun_sent;
    int stun_tries;
    // The TURN allocation
    int turn_state;
    uint8_t turn_id[12];
    double turn_sent;
    int turn_tries;
    char realm[128];
    char nonce[256];
    uint8_t turn_key[16];
    rtc_addr relayed;
    uint32_t lifetime;
    double refresh_at;
    struct {
        uint32_t ip;
        bool granted;
        double sent;
        double granted_at;
        uint8_t id[12];
    } permissions[RTC_MAX_PERMISSIONS];
    int permission_count;
} rtc_ice;

bool rtc_ice_start(rtc_ice *ice, bool controlling, const rtc_ice_config *config);
void rtc_ice_close(rtc_ice *ice);
void rtc_ice_set_remote(rtc_ice *ice, const char *ufrag, const char *pwd);
void rtc_ice_add_remote(rtc_ice *ice, const rtc_candidate *c);
// Our next candidate to tell the other side about, if there's a new one.
bool rtc_ice_next_local(rtc_ice *ice, rtc_candidate *c);
// Checks, retries and refreshes that are due.
void rtc_ice_update(rtc_ice *ice, double now);
// The next datagram for the layer above (not STUN, which this handles): its
// size, or 0 when there's none.
size_t rtc_ice_receive(rtc_ice *ice, double now, uint8_t *out, size_t capacity);
// On the selected pair; dropped until there is one.
void rtc_ice_send(rtc_ice *ice, const void *data, size_t size);

// ---------------------------------------------------------------------------
// DTLS 1.2 (dtls.c, RFC 6347), as WebRTC uses it (RFC 8842): both sides show
// their certificate, which must have the fingerprint the other's description
// said; keys come from ECDHE on P-256; records are sealed with
// ChaCha20-Poly1305 (TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256).

enum { RTC_DTLS_HANDSHAKE, RTC_DTLS_OPEN, RTC_DTLS_FAILED, RTC_DTLS_CLOSED };

typedef void (*rtc_send_fn)(void *user, const void *data, size_t size);

#define RTC_DTLS_FLIGHT 6 // Handshake messages in a flight, at most

typedef struct rtc_dtls {
    int state;
    bool client;
    const rtc_identity *identity;
    uint8_t their_fingerprint[32];
    rtc_send_fn send; // Datagrams for the other side
    rtc_send_fn data; // What they sent: application data
    void *user;
    double started;

    // The handshake
    uint8_t client_random[32];
    uint8_t server_random[32];
    uint8_t ecdh_private[32];
    uint8_t ecdh_public[65];
    uint8_t their_key[65]; // From their certificate
    bool have_their_key;
    uint8_t premaster[32];
    bool extended_master; // RFC 7627
    bool renegotiation_info;
    rtc_sha256 transcript;
    uint8_t hello[512];   // Our ClientHello, until it gets a ServerHello
    size_t hello_size;
    uint8_t cookie[255];
    size_t cookie_size;
    uint16_t seq_out; // The next handshake message's message_seq
    uint16_t seq_in;  // The one expected
    uint8_t message[2048]; // One coming in, in fragments
    size_t message_have;
    bool changed_cipher; // Their ChangeCipherSpec came

    // The last flight we sent, to send again until it's answered
    struct {
        uint8_t type; // Record content type
        uint16_t epoch;
        uint8_t body[800];
        size_t size;
    } flight[RTC_DTLS_FLIGHT];
    int flight_count;
    double flight_sent;
    double flight_timeout;
    bool flight_done; // Our last flight: resent only when theirs comes again

    // Keys, once agreed
    bool keys;
    uint8_t master[48];
    uint8_t write_key[32];
    uint8_t write_iv[12];
    uint8_t read_key[32];
    uint8_t read_iv[12];
    uint16_t epoch_out;
    uint64_t record_seq[2]; // The next record's, in each epoch
    uint64_t replay_top;    // Epoch 1's highest record, and the 64 below it
    uint64_t replay_seen;
} rtc_dtls;

bool rtc_dtls_start(rtc_dtls *d, bool client, const rtc_identity *id, const uint8_t their_fingerprint[32],
                    rtc_send_fn send, rtc_send_fn data, void *user, double now);
void rtc_dtls_receive(rtc_dtls *d, const uint8_t *datagram, size_t size, double now);
void rtc_dtls_update(rtc_dtls *d, double now); // Sending flights again
void rtc_dtls_send(rtc_dtls *d, const void *data, size_t size); // Once it's open
void rtc_dtls_close(rtc_dtls *d);

// ---------------------------------------------------------------------------
// SCTP (sctp.c, RFC 9260) over DTLS (RFC 8261), as data channels use it (RFC
// 8831 and 8832): one channel, whose messages are neither ordered nor sent
// again, like UDP. Only what that needs: the association's setup, DATA with
// its acknowledgements, FORWARD-TSN for what won't be sent again, heartbeats,
// stream resets, and the channel's own open and ack.

enum { RTC_SCTP_CLOSED, RTC_SCTP_CONNECTING, RTC_SCTP_OPEN, RTC_SCTP_FAILED };

typedef void (*rtc_message_fn)(void *user, const void *data, size_t size);

#define RTC_SCTP_WINDOW 1024 // TSNs tracked past the cumulative one
#define RTC_SCTP_FRAGMENTS 16

typedef struct rtc_sctp {
    int state;
    bool initiator; // Sends INIT, rather than waiting for theirs
    rtc_send_fn send;         // SCTP packets, for DTLS
    rtc_message_fn message;   // The channel's messages
    void *user;
    uint16_t port;            // Ours and theirs: both 5000, as WebRTC has it
    uint16_t their_port;
    uint32_t my_tag;
    uint32_t their_tag;
    uint32_t my_initial_tsn;
    double started;
    double init_sent;
    int init_tries;
    uint8_t cookie[64];       // Theirs, to echo
    size_t cookie_size;

    // Receiving: TSNs up to `cumulative` all came (or were given up on);
    // `received` has a bit for each of the WINDOW after it
    uint32_t cumulative;
    uint8_t received[RTC_SCTP_WINDOW / 8];
    int unacknowledged; // DATA not in a SACK yet
    double first_unacknowledged;
    struct {
        bool used;
        uint32_t tsn;
        uint8_t flags;
        size_t size;
        uint8_t data[1200];
    } fragments[RTC_SCTP_FRAGMENTS];

    // Sending
    uint32_t next_tsn;
    uint32_t their_cumulative; // What their SACKs say they have
    uint32_t forward;          // Up to where we've given up sending: FORWARD-TSN
    double forward_sent;
    struct {
        uint32_t tsn;
        double sent;
        bool acked;
        bool reliable; // The channel's open and ack: sent until acknowledged
        size_t size;
        uint8_t chunk[128];
    } sent[64];
    int sent_count;

    // The channel
    bool channel;      // Open
    bool opening;      // We sent DATA_CHANNEL_OPEN
    uint16_t stream;
    uint16_t ssn;      // Our next stream sequence number, for the ordered open/ack
} rtc_sctp;

void rtc_sctp_start(rtc_sctp *s, bool initiator, uint16_t their_port, rtc_send_fn send, rtc_message_fn message,
                    void *user, double now);
// A packet from DTLS
void rtc_sctp_receive(rtc_sctp *s, const uint8_t *packet, size_t size, double now);
void rtc_sctp_update(rtc_sctp *s, double now);
// Opens the channel: the side that creates it (the one who joins), with a
// stream number of the parity its DTLS role gives it.
void rtc_sctp_open_channel(rtc_sctp *s, uint16_t stream, double now);
// A message on the channel, once it's open. Up to 1200 bytes.
void rtc_sctp_send(rtc_sctp *s, const void *data, size_t size, double now);

// ---------------------------------------------------------------------------
// A peer connection (peer.c): ICE, DTLS and SCTP together, for one data
// channel to one other machine, like a browser's RTCPeerConnection. The side
// that joins makes the offer and the channel; signals (descriptions and
// candidates) go through the relay, as JSON the browser's side reads.

enum { RTC_PEER_CONNECTING, RTC_PEER_OPEN, RTC_PEER_FAILED };

#define RTC_PEER_INBOX 64
#define RTC_PEER_SIGNALS 16

typedef struct rtc_peer {
    int state;
    bool offerer;
    bool dtls_client;
    bool have_remote;
    bool dtls_started;
    bool sctp_started;
    rtc_identity identity;
    rtc_ice ice;
    rtc_dtls dtls;
    rtc_sctp sctp;
    rtc_description remote;
    uint64_t session;
    double now;
    // Signals for the other side, as JSON
    char *signals[RTC_PEER_SIGNALS];
    int signal_count;
    bool description_told;
    // Messages from the other side
    struct {
        uint16_t size;
        uint8_t data[1200];
    } inbox[RTC_PEER_INBOX];
    int inbox_head;
    int inbox_count;
} rtc_peer;

// Heap-allocated: it's big.
rtc_peer *rtc_peer_new(bool offerer, const rtc_ice_config *config);
void rtc_peer_free(rtc_peer *p);
// Signals from the other side: a description (its type and SDP) or a candidate.
void rtc_peer_description(rtc_peer *p, const char *type, const char *sdp);
void rtc_peer_candidate(rtc_peer *p, const char *candidate);
// Our next signal, as JSON ({description: ...} or {candidate: ...}): false if none.
bool rtc_peer_next_signal(rtc_peer *p, char *json, size_t capacity);
void rtc_peer_update(rtc_peer *p, double now);
void rtc_peer_send(rtc_peer *p, const void *data, size_t size);
size_t rtc_peer_receive(rtc_peer *p, void *out, size_t capacity);

// ---------------------------------------------------------------------------
// TLS (tls.c), with the system's own, for wss://. Nothing blocks: what would
// have to wait says 0, and is asked again later.

typedef struct rtc_tls rtc_tls;

// NULL if the system has no TLS for us.
rtc_tls *rtc_tls_new(const char *host);
// Over a connected TCP socket: 1 once it's done, 0 while it's going, -1 if
// it failed (a certificate that isn't the host's, or isn't trusted, too).
int rtc_tls_handshake(rtc_tls *t, rtc_socket s);
// As rtc_tcp_send and rtc_tcp_receive, sealed and opened.
int rtc_tls_send(rtc_tls *t, rtc_socket s, const void *data, size_t size);
int rtc_tls_receive(rtc_tls *t, rtc_socket s, void *out, size_t capacity);
void rtc_tls_free(rtc_tls *t);

// ---------------------------------------------------------------------------
// WebSocket (ws.c), for the relay: wss://, or ws:// (on this machine, for
// tests), text messages.

enum { RTC_WS_CONNECTING, RTC_WS_OPEN, RTC_WS_CLOSED };

typedef struct rtc_ws {
    int state;
    rtc_socket socket;
    rtc_tls *tls; // wss://
    bool connected;
    bool upgraded;
    char request[512];
    char key[25];
    double started;
    uint8_t *in;  // What came, not read yet
    size_t in_size;
    uint8_t *out; // What's to go, not sent yet
    size_t out_size;
} rtc_ws;

// wss:// or ws://host[:port][/path]. False if the address is wrong or can't be found.
bool rtc_ws_open(rtc_ws *w, const char *url, double now);
void rtc_ws_update(rtc_ws *w, double now);
// The next text message, NUL-terminated: false if there's none yet.
bool rtc_ws_next(rtc_ws *w, char *message, size_t capacity);
void rtc_ws_send(rtc_ws *w, const char *text, size_t size);
void rtc_ws_close(rtc_ws *w);

// ---------------------------------------------------------------------------
// Everything else

// What the stack does, on stderr, when TIDE_RTC_DEBUG is set.
void rtc_debug(const char *format, ...) __attribute__((format(printf, 1, 2)));

uint32_t rtc_crc32(const void *data, size_t size);  // STUN's FINGERPRINT
uint32_t rtc_crc32c(const void *data, size_t size); // SCTP's checksum
// Random bytes from the operating system, for keys and nonces. False if it
// has none to give, which nothing can go on without.
bool rtc_random(void *out, size_t size);

// Reading and writing big-endian numbers, as networks write them.
static inline uint16_t rtc_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static inline uint32_t rtc_get24(const uint8_t *p)
{
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

static inline uint32_t rtc_get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static inline void rtc_put16(uint8_t *p, const uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void rtc_put24(uint8_t *p, const uint32_t v)
{
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)v;
}

static inline void rtc_put32(uint8_t *p, const uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
