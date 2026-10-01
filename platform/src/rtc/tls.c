// TLS for the relay's wss:// (see rtc.h), with each system's own: SChannel on
// Windows, Secure Transport on macOS, and OpenSSL on Linux, loaded when it's
// first needed rather than linked, so games ship none of it. Each checks the
// relay's certificate against the system's trusted ones, and its name, as a
// browser does. Everything runs on the socket without blocking: a call that
// would have to wait says so, and is made again later.

#if defined(__linux__)
#define _DEFAULT_SOURCE // dlopen under strict C
#endif

#include "rtc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <windows.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#define BUFFER 65536

struct rtc_tls {
    CredHandle credentials;
    CtxtHandle context;
    bool have_credentials;
    bool have_context;
    bool done; // The handshake
    wchar_t host[256];
    SecPkgContext_StreamSizes sizes;
    uint8_t in[BUFFER]; // Sealed bytes come in, not used yet
    size_t in_size;
    uint8_t plain[BUFFER]; // Opened, not read yet
    size_t plain_size;
    uint8_t out[BUFFER]; // Sealed, not sent yet
    size_t out_size;
};

rtc_tls *rtc_tls_new(const char *host)
{
    rtc_tls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, host, -1, t->host, 255);
    SCHANNEL_CRED cred;
    memset(&cred, 0, sizeof cred);
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT; // 1.3 adds messages after the handshake that SSPI makes awkward
    cred.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    TimeStamp expiry;
    if (AcquireCredentialsHandleW(NULL, (SEC_WCHAR *)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, NULL, &cred, NULL, NULL,
                                  &t->credentials, &expiry) != SEC_E_OK) {
        free(t);
        return NULL;
    }
    t->have_credentials = true;
    return t;
}

void rtc_tls_free(rtc_tls *t)
{
    if (!t) return;
    if (t->have_context) DeleteSecurityContext(&t->context);
    if (t->have_credentials) FreeCredentialsHandle(&t->credentials);
    free(t);
}

// Sealed bytes out, as far as the socket takes them: false if it's gone.
static bool flush(rtc_tls *t, const rtc_socket s)
{
    while (t->out_size) {
        const int sent = rtc_tcp_send(s, t->out, t->out_size);
        if (sent < 0) return false;
        if (sent == 0) return true;
        memmove(t->out, t->out + sent, t->out_size - (size_t)sent);
        t->out_size -= (size_t)sent;
    }
    return true;
}

static bool queue(rtc_tls *t, const void *data, const size_t size)
{
    if (t->out_size + size > BUFFER) return false;
    memcpy(t->out + t->out_size, data, size);
    t->out_size += size;
    return true;
}

// Sealed bytes in, as far as there are: false if the socket's gone.
static bool fill(rtc_tls *t, const rtc_socket s)
{
    while (t->in_size < BUFFER) {
        const int got = rtc_tcp_receive(s, t->in + t->in_size, BUFFER - t->in_size);
        if (got < 0) return false;
        if (got == 0) break;
        t->in_size += (size_t)got;
    }
    return true;
}

int rtc_tls_handshake(rtc_tls *t, const rtc_socket s)
{
    if (t->done) return 1;
    if (!flush(t, s) || !fill(t, s)) return -1;
    const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY
                      | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR;
    for (;;) {
        if (t->have_context && t->in_size == 0) return 0; // Waiting for the server
        SecBuffer in[2] = {{(unsigned long)t->in_size, SECBUFFER_TOKEN, t->in}, {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc in_desc = {SECBUFFER_VERSION, 2, in};
        SecBuffer out[1] = {{0, SECBUFFER_TOKEN, NULL}};
        SecBufferDesc out_desc = {SECBUFFER_VERSION, 1, out};
        DWORD got_flags = 0;
        TimeStamp expiry;
        const SECURITY_STATUS status = InitializeSecurityContextW(
            &t->credentials, t->have_context ? &t->context : NULL, t->host, flags, 0, 0,
            t->have_context ? &in_desc : NULL, 0, t->have_context ? NULL : &t->context, &out_desc, &got_flags, &expiry);
        t->have_context = true;
        if (out[0].cbBuffer && out[0].pvBuffer) {
            const bool queued = queue(t, out[0].pvBuffer, out[0].cbBuffer);
            FreeContextBuffer(out[0].pvBuffer);
            if (!queued || !flush(t, s)) return -1;
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE) return 0; // More from the server first
        // What the call used of the input; anything after it is the next message
        const size_t extra = in[1].BufferType == SECBUFFER_EXTRA ? in[1].cbBuffer : 0;
        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED) {
            memmove(t->in, t->in + t->in_size - extra, extra);
            t->in_size = extra;
        }
        if (status == SEC_E_OK) {
            if (QueryContextAttributesW(&t->context, SECPKG_ATTR_STREAM_SIZES, &t->sizes) != SEC_E_OK) return -1;
            t->done = true;
            return 1;
        }
        if (status != SEC_I_CONTINUE_NEEDED) {
            rtc_debug("tls: the handshake failed (%08lx)", (unsigned long)status);
            return -1;
        }
    }
}

int rtc_tls_send(rtc_tls *t, const rtc_socket s, const void *data, const size_t size)
{
    if (!t->done || !flush(t, s)) return t->done ? -1 : 0;
    if (t->out_size) return 0; // Still sending the last
    const size_t n = size < t->sizes.cbMaximumMessage ? size : t->sizes.cbMaximumMessage;
    const size_t total = t->sizes.cbHeader + n + t->sizes.cbTrailer;
    if (total > BUFFER) return -1;
    memcpy(t->out + t->sizes.cbHeader, data, n);
    SecBuffer buffers[4] = {{t->sizes.cbHeader, SECBUFFER_STREAM_HEADER, t->out},
                            {(unsigned long)n, SECBUFFER_DATA, t->out + t->sizes.cbHeader},
                            {t->sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, t->out + t->sizes.cbHeader + n},
                            {0, SECBUFFER_EMPTY, NULL}};
    SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};
    if (EncryptMessage(&t->context, 0, &desc, 0) != SEC_E_OK) return -1;
    t->out_size = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
    if (!flush(t, s)) return -1;
    return (int)n;
}

int rtc_tls_receive(rtc_tls *t, const rtc_socket s, void *out, const size_t capacity)
{
    if (!t->done) return 0;
    if (!flush(t, s)) return -1;
    bool gone = !fill(t, s);
    while (!t->plain_size && t->in_size) {
        SecBuffer buffers[4] = {{(unsigned long)t->in_size, SECBUFFER_DATA, t->in},
                                {0, SECBUFFER_EMPTY, NULL},
                                {0, SECBUFFER_EMPTY, NULL},
                                {0, SECBUFFER_EMPTY, NULL}};
        SecBufferDesc desc = {SECBUFFER_VERSION, 4, buffers};
        const SECURITY_STATUS status = DecryptMessage(&t->context, &desc, 0, NULL);
        if (status == SEC_E_INCOMPLETE_MESSAGE) break;
        if (status == SEC_I_CONTEXT_EXPIRED) return -1; // The server closed it
        if (status != SEC_E_OK) {
            rtc_debug("tls: a record didn't open (%08lx)", (unsigned long)status);
            return -1;
        }
        size_t extra = 0;
        const uint8_t *extra_at = NULL;
        for (int i = 0; i < 4; i++) {
            if (buffers[i].BufferType == SECBUFFER_DATA && buffers[i].cbBuffer
                && t->plain_size + buffers[i].cbBuffer <= BUFFER) {
                memcpy(t->plain + t->plain_size, buffers[i].pvBuffer, buffers[i].cbBuffer);
                t->plain_size += buffers[i].cbBuffer;
            }
            if (buffers[i].BufferType == SECBUFFER_EXTRA) {
                extra = buffers[i].cbBuffer;
                extra_at = t->in + t->in_size - extra;
            }
        }
        if (extra_at) memmove(t->in, extra_at, extra);
        t->in_size = extra;
    }
    if (!t->plain_size) return gone ? -1 : 0;
    const size_t n = t->plain_size < capacity ? t->plain_size : capacity;
    memcpy(out, t->plain, n);
    memmove(t->plain, t->plain + n, t->plain_size - n);
    t->plain_size -= n;
    return (int)n;
}

#elif defined(__APPLE__)

#include <Security/SecureTransport.h>
#include <errno.h>
#include <sys/socket.h>

// Secure Transport is deprecated in favour of Network.framework, which runs
// its own connections on its own queues. This fits the rest (a socket we
// poll), and does all a relay's connection needs.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

struct rtc_tls {
    SSLContextRef context;
    rtc_socket socket;
    bool done;
};

static OSStatus tls_read(SSLConnectionRef connection, void *data, size_t *size)
{
    const rtc_tls *t = connection;
    const size_t wanted = *size;
    size_t got = 0;
    while (got < wanted) {
        const int n = rtc_tcp_receive(t->socket, (uint8_t *)data + got, wanted - got);
        if (n < 0) {
            *size = got;
            return errSSLClosedAbort;
        }
        if (n == 0) break;
        got += (size_t)n;
    }
    *size = got;
    return got < wanted ? errSSLWouldBlock : noErr;
}

static OSStatus tls_write(SSLConnectionRef connection, const void *data, size_t *size)
{
    const rtc_tls *t = connection;
    const size_t wanted = *size;
    size_t sent = 0;
    while (sent < wanted) {
        const int n = rtc_tcp_send(t->socket, (const uint8_t *)data + sent, wanted - sent);
        if (n < 0) {
            *size = sent;
            return errSSLClosedAbort;
        }
        if (n == 0) break;
        sent += (size_t)n;
    }
    *size = sent;
    return sent < wanted ? errSSLWouldBlock : noErr;
}

rtc_tls *rtc_tls_new(const char *host)
{
    rtc_tls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->context = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
    if (!t->context || SSLSetIOFuncs(t->context, tls_read, tls_write) != noErr
        || SSLSetConnection(t->context, t) != noErr || SSLSetPeerDomainName(t->context, host, strlen(host)) != noErr) {
        rtc_tls_free(t);
        return NULL;
    }
    return t;
}

void rtc_tls_free(rtc_tls *t)
{
    if (!t) return;
    if (t->context) CFRelease(t->context);
    free(t);
}

int rtc_tls_handshake(rtc_tls *t, const rtc_socket s)
{
    if (t->done) return 1;
    t->socket = s;
    const OSStatus status = SSLHandshake(t->context);
    if (status == noErr) {
        t->done = true;
        return 1;
    }
    if (status == errSSLWouldBlock) return 0;
    rtc_debug("tls: the handshake failed (%d)", (int)status);
    return -1;
}

int rtc_tls_send(rtc_tls *t, const rtc_socket s, const void *data, const size_t size)
{
    if (!t->done) return 0;
    t->socket = s;
    size_t done = 0;
    const OSStatus status = SSLWrite(t->context, data, size, &done);
    if (status != noErr && status != errSSLWouldBlock) return -1;
    return (int)done;
}

int rtc_tls_receive(rtc_tls *t, const rtc_socket s, void *out, const size_t capacity)
{
    if (!t->done) return 0;
    t->socket = s;
    size_t done = 0;
    const OSStatus status = SSLRead(t->context, out, capacity, &done);
    if (done) return (int)done;
    return status == errSSLWouldBlock ? 0 : -1;
}

#else

// OpenSSL, as the system has it: libssl 3, or 1.1 on older systems. Only
// these functions, looked up once.
#include <dlfcn.h>

typedef struct ssl_api {
    bool tried, ok;
    const void *(*TLS_client_method)(void);
    void *(*SSL_CTX_new)(const void *);
    int (*SSL_CTX_set_default_verify_paths)(void *);
    void (*SSL_CTX_free)(void *);
    void *(*SSL_new)(void *);
    void (*SSL_free)(void *);
    int (*SSL_set_fd)(void *, int);
    long (*SSL_ctrl)(void *, int, long, void *);
    int (*SSL_set1_host)(void *, const char *);
    void (*SSL_set_verify)(void *, int, void *);
    int (*SSL_connect)(void *);
    int (*SSL_read)(void *, void *, int);
    int (*SSL_write)(void *, const void *, int);
    int (*SSL_get_error)(const void *, int);
} ssl_api;

static ssl_api api;

#define SSL_ERROR_WANT_READ 2
#define SSL_ERROR_WANT_WRITE 3
#define SSL_VERIFY_PEER 1
#define SSL_CTRL_MODE 33
#define SSL_CTRL_SET_TLSEXT_HOSTNAME 55
#define SSL_MODE_PARTIAL_WRITE_AND_MOVING_BUFFER 3

static bool load(void)
{
    if (api.tried) return api.ok;
    api.tried = true;
    void *lib = dlopen("libssl.so.3", RTLD_NOW);
    if (!lib) lib = dlopen("libssl.so.1.1", RTLD_NOW);
    if (!lib) {
        fprintf(stderr, "purr: rooms need OpenSSL (libssl) to reach the relay securely, and it isn't here\n");
        return false;
    }
#define FIND(name) *(void **)&api.name = dlsym(lib, #name); if (!api.name) return false
    FIND(TLS_client_method);
    FIND(SSL_CTX_new);
    FIND(SSL_CTX_set_default_verify_paths);
    FIND(SSL_CTX_free);
    FIND(SSL_new);
    FIND(SSL_free);
    FIND(SSL_set_fd);
    FIND(SSL_ctrl);
    FIND(SSL_set1_host);
    FIND(SSL_set_verify);
    FIND(SSL_connect);
    FIND(SSL_read);
    FIND(SSL_write);
    FIND(SSL_get_error);
#undef FIND
    api.ok = true;
    return true;
}

struct rtc_tls {
    void *context;
    void *ssl;
    bool attached;
    bool done;
};

rtc_tls *rtc_tls_new(const char *host)
{
    if (!load()) return NULL;
    rtc_tls *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->context = api.SSL_CTX_new(api.TLS_client_method());
    t->ssl = t->context ? api.SSL_new(t->context) : NULL;
    if (!t->ssl || api.SSL_CTX_set_default_verify_paths(t->context) != 1) {
        rtc_tls_free(t);
        return NULL;
    }
    api.SSL_ctrl(t->ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, 0, (void *)host); // SNI
    api.SSL_ctrl(t->ssl, SSL_CTRL_MODE, SSL_MODE_PARTIAL_WRITE_AND_MOVING_BUFFER, NULL);
    api.SSL_set1_host(t->ssl, host); // The certificate must be for it
    api.SSL_set_verify(t->ssl, SSL_VERIFY_PEER, NULL);
    return t;
}

void rtc_tls_free(rtc_tls *t)
{
    if (!t) return;
    if (t->ssl) api.SSL_free(t->ssl);
    if (t->context) api.SSL_CTX_free(t->context);
    free(t);
}

int rtc_tls_handshake(rtc_tls *t, const rtc_socket s)
{
    if (t->done) return 1;
    if (!t->attached) {
        api.SSL_set_fd(t->ssl, (int)s);
        t->attached = true;
    }
    const int r = api.SSL_connect(t->ssl);
    if (r == 1) {
        t->done = true;
        return 1;
    }
    const int error = api.SSL_get_error(t->ssl, r);
    if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) return 0;
    rtc_debug("tls: the handshake failed (%d)", error);
    return -1;
}

int rtc_tls_send(rtc_tls *t, const rtc_socket s, const void *data, const size_t size)
{
    (void)s;
    if (!t->done) return 0;
    const int r = api.SSL_write(t->ssl, data, (int)size);
    if (r > 0) return r;
    const int error = api.SSL_get_error(t->ssl, r);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
}

int rtc_tls_receive(rtc_tls *t, const rtc_socket s, void *out, const size_t capacity)
{
    (void)s;
    if (!t->done) return 0;
    const int r = api.SSL_read(t->ssl, out, (int)capacity);
    if (r > 0) return r;
    const int error = api.SSL_get_error(t->ssl, r);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
}

#endif
