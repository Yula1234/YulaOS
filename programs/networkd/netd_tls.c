// SPDX-License-Identifier: GPL-2.0

#include "netd_tls.h"

#include <string.h>

#include <yula.h>

#include "netd_aead_aes128gcm.h"
#include "netd_aead_chacha20poly1305.h"
#include "netd_hkdf_sha256.h"
#include "netd_hmac_sha256.h"
#include "netd_rand.h"
#include "netd_sha256.h"
#include "netd_tcp.h"
#include "netd_x25519.h"

#define NETD_TLS_CT_CHANGE_CIPHER_SPEC 20u
#define NETD_TLS_CT_ALERT 21u
#define NETD_TLS_CT_HANDSHAKE 22u
#define NETD_TLS_CT_APPLICATION_DATA 23u

#define NETD_TLS_HS_CLIENT_HELLO 1u
#define NETD_TLS_HS_SERVER_HELLO 2u
#define NETD_TLS_HS_ENCRYPTED_EXTENSIONS 8u
#define NETD_TLS_HS_CERTIFICATE 11u
#define NETD_TLS_HS_CERTIFICATE_VERIFY 15u
#define NETD_TLS_HS_FINISHED 20u
#define NETD_TLS_HS_MESSAGE_HASH 254u

#define NETD_TLS_SUITE_AES128GCM_SHA256 0x1301u
#define NETD_TLS_SUITE_CHACHA20POLY1305_SHA256 0x1303u

#define NETD_TLS_EXT_SERVER_NAME 0u
#define NETD_TLS_EXT_SUPPORTED_GROUPS 10u
#define NETD_TLS_EXT_SIGNATURE_ALGORITHMS 13u
#define NETD_TLS_EXT_SUPPORTED_VERSIONS 43u
#define NETD_TLS_EXT_COOKIE 44u
#define NETD_TLS_EXT_KEY_SHARE 51u

#define NETD_TLS_GROUP_X25519 29u

#define NETD_TLS_PROT_NONE 0
#define NETD_TLS_PROT_HANDSHAKE 1
#define NETD_TLS_PROT_APPLICATION 2

typedef struct {
    uint32_t start_ms;
    uint32_t timeout_ms;
} netd_tls_deadline_t;


static void netd_tls_deadline_init(netd_tls_deadline_t* d, uint32_t timeout_ms) {
    if (!d) {
        return;
    }

    if (timeout_ms == 0) {
        timeout_ms = 5000u;
    }

    d->start_ms = uptime_ms();
    d->timeout_ms = timeout_ms;
}

static uint32_t netd_tls_deadline_remaining(const netd_tls_deadline_t* d) {
    if (!d) {
        return 0;
    }

    uint32_t now_ms = uptime_ms();
    uint32_t elapsed = now_ms - d->start_ms;
    if (elapsed >= d->timeout_ms) {
        return 0;
    }

    return d->timeout_ms - elapsed;
}

static uint16_t netd_tls_load_be16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

static uint32_t netd_tls_load_be24(const uint8_t* p) {
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static void netd_tls_store_be16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v);
}

static void netd_tls_store_be24(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 16);
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v);
}

static void netd_tls_store_be64(uint8_t out[8], uint64_t v) {
    out[0] = (uint8_t)(v >> 56);
    out[1] = (uint8_t)(v >> 48);
    out[2] = (uint8_t)(v >> 40);
    out[3] = (uint8_t)(v >> 32);
    out[4] = (uint8_t)(v >> 24);
    out[5] = (uint8_t)(v >> 16);
    out[6] = (uint8_t)(v >> 8);
    out[7] = (uint8_t)(v);
}

static void netd_tls_nonce_xor(uint8_t out[12], const uint8_t iv[12], uint64_t seq) {
    memcpy(out, iv, 12u);

    uint8_t s[8];
    netd_tls_store_be64(s, seq);

    out[4] ^= s[0];
    out[5] ^= s[1];
    out[6] ^= s[2];
    out[7] ^= s[3];
    out[8] ^= s[4];
    out[9] ^= s[5];
    out[10] ^= s[6];
    out[11] ^= s[7];
}

static void netd_tls_wipe(void* p, uint32_t n) {
    if (!p || n == 0) {
        return;
    }
    volatile uint8_t* v = (volatile uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) {
        v[i] = 0;
    }
}

static int netd_tls_tcp_read_exact_deadline(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t* out, uint32_t n, const netd_tls_deadline_t* deadline) {
    if (!ctx || !t || !t->tcp || (!out && n != 0)) {
        return 0;
    }

    uint32_t off = 0;
    while (off < n) {
        uint32_t remaining_ms = netd_tls_deadline_remaining(deadline);
        if (remaining_ms == 0) {
            t->tcp->last_err = NET_STATUS_TIMEOUT;
            return 0;
        }

        uint32_t got = 0;
        uint32_t cap = n - off;
        if (cap > 512u) {
            cap = 512u;
        }

        if (!netd_tcp_recv(ctx, t->tcp, out + off, cap, remaining_ms, &got)) {
            return 0;
        }
        if (got == 0) {
            return 0;
        }
        off += got;
    }

    return 1;
}

static int netd_tls_tcp_read_exact(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t* out, uint32_t n, uint32_t timeout_ms) {
    if (!ctx || !t || !t->tcp || (!out && n != 0)) {
        return 0;
    }

    netd_tls_deadline_t deadline;
    netd_tls_deadline_init(&deadline, timeout_ms);
    return netd_tls_tcp_read_exact_deadline(ctx, t, out, n, &deadline);
}

static int netd_tls_tcp_write_all_deadline(netd_ctx_t* ctx, netd_tls_client_t* t, const uint8_t* data, uint32_t len, const netd_tls_deadline_t* deadline) {
    if (!ctx || !t || !t->tcp || (!data && len != 0)) {
        return 0;
    }

    uint32_t remaining_ms = netd_tls_deadline_remaining(deadline);
    if (remaining_ms == 0) {
        t->tcp->last_err = NET_STATUS_TIMEOUT;
        return 0;
    }

    return netd_tcp_send(ctx, t->tcp, data, len, remaining_ms);
}

static int netd_tls_tcp_write_all(netd_ctx_t* ctx, netd_tls_client_t* t, const uint8_t* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !t || !t->tcp || (!data && len != 0)) {
        return 0;
    }
    return netd_tcp_send(ctx, t->tcp, data, len, timeout_ms);
}

typedef struct {
    uint8_t buf[8192];
    uint32_t r;
    uint32_t w;
} netd_tls_hs_rx_t;

static void netd_tls_hs_rx_reset(netd_tls_hs_rx_t* h) {
    if (!h) {
        return;
    }
    h->r = 0;
    h->w = 0;
    memset(h->buf, 0, sizeof(h->buf));
}

static uint32_t netd_tls_hs_rx_avail(const netd_tls_hs_rx_t* h) {
    if (!h) {
        return 0;
    }
    return h->w - h->r;
}

static int netd_tls_hs_rx_push(netd_tls_hs_rx_t* h, const uint8_t* data, uint32_t len) {
    if (!h || (!data && len != 0)) {
        return 0;
    }

    if (len > (uint32_t)sizeof(h->buf)) {
        data += (len - (uint32_t)sizeof(h->buf));
        len = (uint32_t)sizeof(h->buf);
        h->r = 0;
        h->w = 0;
    }

    uint32_t avail = netd_tls_hs_rx_avail(h);
    uint32_t cap = (uint32_t)sizeof(h->buf);
    if (avail + len > cap) {
        uint32_t drop = (avail + len) - cap;
        h->r += drop;
    }

    uint32_t wi = h->w % cap;
    uint32_t first = cap - wi;
    if (first > len) {
        first = len;
    }
    memcpy(h->buf + wi, data, first);
    if (len > first) {
        memcpy(h->buf, data + first, len - first);
    }
    h->w += len;
    return 1;
}

static int netd_tls_hs_rx_peek(const netd_tls_hs_rx_t* h, uint32_t off, void* out, uint32_t n) {
    if (!h || !out || n == 0) {
        return 0;
    }

    uint32_t avail = netd_tls_hs_rx_avail(h);
    if (off + n > avail) {
        return 0;
    }

    uint32_t cap = (uint32_t)sizeof(h->buf);
    uint32_t ri = (h->r + off) % cap;
    uint32_t first = cap - ri;
    if (first > n) {
        first = n;
    }
    memcpy(out, h->buf + ri, first);
    if (n > first) {
        memcpy((uint8_t*)out + first, h->buf, n - first);
    }
    return 1;
}

static int netd_tls_hs_rx_drop(netd_tls_hs_rx_t* h, uint32_t n) {
    if (!h) {
        return 0;
    }
    uint32_t avail = netd_tls_hs_rx_avail(h);
    if (n > avail) {
        return 0;
    }
    h->r += n;
    return 1;
}

static void netd_tls_transcript_init(netd_sha256_t* tr) {
    netd_sha256_init(tr);
}

static void netd_tls_transcript_update(netd_sha256_t* tr, const void* data, uint32_t len) {
    netd_sha256_update(tr, data, len);
}

static void netd_tls_transcript_hash(netd_sha256_t* tr, uint8_t out[32]) {
    netd_sha256_t tmp;
    memcpy(&tmp, tr, sizeof(tmp));
    netd_sha256_final(&tmp, out);
    memset(&tmp, 0, sizeof(tmp));
}

static void netd_tls_sha256_empty(uint8_t out[32]) {
    netd_sha256_hash(0, 0, out);
}

static int netd_tls_write_record_plain(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t ct, const uint8_t* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !t || !t->tcp || (!data && len != 0)) {
        return 0;
    }

    uint8_t hdr[5];
    hdr[0] = ct;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    netd_tls_store_be16(hdr + 3, (uint16_t)len);

    if (!netd_tls_tcp_write_all(ctx, t, hdr, (uint32_t)sizeof(hdr), timeout_ms)) {
        return 0;
    }
    if (len > 0) {
        if (!netd_tls_tcp_write_all(ctx, t, data, len, timeout_ms)) {
            return 0;
        }
    }
    return 1;
}

static int netd_tls_read_record_header(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t hdr[5], uint32_t timeout_ms) {
    return netd_tls_tcp_read_exact(ctx, t, hdr, 5u, timeout_ms);
}
static int netd_tls_read_record_body(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t* out, uint32_t len, uint32_t timeout_ms) {
    return netd_tls_tcp_read_exact(ctx, t, out, len, timeout_ms);
}

static int netd_tls_write_record_plain_deadline(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t ct, const uint8_t* data, uint32_t len, const netd_tls_deadline_t* deadline) {
    if (!ctx || !t || !t->tcp || (!data && len != 0)) {
        return 0;
    }

    uint8_t hdr[5];
    hdr[0] = ct;
    hdr[1] = 0x03;
    hdr[2] = 0x03;
    netd_tls_store_be16(hdr + 3, (uint16_t)len);

    if (!netd_tls_tcp_write_all_deadline(ctx, t, hdr, (uint32_t)sizeof(hdr), deadline)) {
        return 0;
    }
    if (len > 0) {
        if (!netd_tls_tcp_write_all_deadline(ctx, t, data, len, deadline)) {
            return 0;
        }
    }
    return 1;
}

static int netd_tls_read_record_header_deadline(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t hdr[5], const netd_tls_deadline_t* deadline) {
    return netd_tls_tcp_read_exact_deadline(ctx, t, hdr, 5u, deadline);
}

static int netd_tls_read_record_body_deadline(netd_ctx_t* ctx, netd_tls_client_t* t, uint8_t* out, uint32_t len, const netd_tls_deadline_t* deadline) {
    return netd_tls_tcp_read_exact_deadline(ctx, t, out, len, deadline);
}

static void netd_tls_set_internal_alert(netd_tls_client_t* t, uint16_t code) {
    if (!t) {
        return;
    }

    t->hs_alert = (uint16_t)(NET_HTTP_TLS_ALERT_INTERNAL_FLAG | code);
}

static void netd_tls_mark_io_failure(netd_ctx_t* ctx, netd_tls_client_t* t) {
    if (!ctx || !t) {
        return;
    }

    if (t->tcp && t->tcp->remote_closed) {
        netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_EOF);
        return;
    }

    netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_IO);
}

static int netd_tls_is_close_notify(const uint8_t* payload, uint32_t len) {
    if (!payload || len < 2u) {
        return 0;
    }
    return payload[1] == 0u;
}

static void netd_tls_capture_alert(netd_tls_client_t* t, const uint8_t* alert, uint32_t len) {
    if (!t || !alert || len < 2u) {
        return;
    }

    t->hs_alert = (uint16_t)((uint16_t)alert[0] << 8) | (uint16_t)alert[1];
}

static int netd_tls_seal_record(
    uint8_t out_hdr[5],
    uint8_t* out_body,
    uint32_t* out_body_len,
    uint16_t suite,
    const uint8_t key[32],
    const uint8_t iv[12],
    uint64_t* io_seq,
    const uint8_t* plaintext,
    uint32_t plaintext_len,
    uint8_t inner_type
) {
    if (!out_hdr || !out_body || !out_body_len || !key || !iv || !io_seq || (!plaintext && plaintext_len != 0)) {
        return 0;
    }

    uint32_t inner_len = plaintext_len + 1u;
    memcpy(out_body, plaintext, plaintext_len);
    out_body[plaintext_len] = inner_type;

    uint32_t key_len = 0;
    if (suite == NETD_TLS_SUITE_AES128GCM_SHA256) {
        key_len = 16u;
    } else if (suite == NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        key_len = 32u;
    }

    uint8_t nonce[12];
    netd_tls_nonce_xor(nonce, iv, *io_seq);

    out_hdr[0] = NETD_TLS_CT_APPLICATION_DATA;
    out_hdr[1] = 0x03;
    out_hdr[2] = 0x03;
    netd_tls_store_be16(out_hdr + 3, (uint16_t)(inner_len + 16u));

    uint8_t tag[16];
    int ok = 0;
    if (suite == NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        ok = netd_aead_chacha20poly1305_seal(
            key,
            nonce,
            out_hdr,
            5u,
            out_body,
            inner_len,
            out_body,
            tag
        );
    } else if (suite == NETD_TLS_SUITE_AES128GCM_SHA256) {
        ok = netd_aead_aes128gcm_seal(
            key,
            nonce,
            out_hdr,
            5u,
            out_body,
            inner_len,
            out_body,
            tag
        );
    }

    if (!ok) {
        memset(nonce, 0, sizeof(nonce));
        return 0;
    }

    memcpy(out_body + inner_len, tag, (uint32_t)sizeof(tag));
    *out_body_len = inner_len + (uint32_t)sizeof(tag);
    *io_seq += 1u;

    memset(tag, 0, sizeof(tag));
    memset(nonce, 0, sizeof(nonce));
    return 1;
}

static int netd_tls_open_record(
    uint8_t* buf,
    uint32_t len,
    uint16_t suite,
    const uint8_t key[32],
    const uint8_t iv[12],
    uint64_t* io_seq,
    uint8_t hdr[5],
    uint8_t* out_type,
    uint8_t** out_payload,
    uint32_t* out_payload_len
) {
    if (!buf || !key || !iv || !io_seq || !hdr || !out_type || !out_payload || !out_payload_len) {
        return 0;
    }

    if (len < 16u + 1u) {
        return 0;
    }

    uint32_t cipher_len = len - 16u;
    uint8_t* ciphertext = buf;
    uint8_t* tag = buf + cipher_len;

    uint32_t key_len = 0;
    if (suite == NETD_TLS_SUITE_AES128GCM_SHA256) {
        key_len = 16u;
    } else if (suite == NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        key_len = 32u;
    }

    uint8_t nonce[12];
    netd_tls_nonce_xor(nonce, iv, *io_seq);

    uint8_t aad[5];
    aad[0] = hdr[0];
    aad[1] = hdr[1];
    aad[2] = hdr[2];
    netd_tls_store_be16(aad + 3u, (uint16_t)len);

    int ok = 0;
    if (suite == NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        ok = netd_aead_chacha20poly1305_open(
            key,
            nonce,
            aad,
            (uint32_t)sizeof(aad),
            ciphertext,
            cipher_len,
            tag,
            ciphertext
        );
    } else if (suite == NETD_TLS_SUITE_AES128GCM_SHA256) {
        ok = netd_aead_aes128gcm_open(
            key,
            nonce,
            aad,
            (uint32_t)sizeof(aad),
            ciphertext,
            cipher_len,
            tag,
            ciphertext
        );
    }

    if (!ok) {
        memset(nonce, 0, sizeof(nonce));
        return 0;
    }

    *io_seq += 1u;

    uint32_t i = cipher_len;
    while (i > 0 && ciphertext[i - 1u] == 0) {
        i--;
    }
    if (i == 0) {
        memset(nonce, 0, sizeof(nonce));
        return 0;
    }

    uint8_t inner_type = ciphertext[i - 1u];
    *out_type = inner_type;
    *out_payload = ciphertext;
    *out_payload_len = i - 1u;

    memset(nonce, 0, sizeof(nonce));
    return 1;
}

typedef struct {
    uint8_t buf[2048];
    uint32_t w;
} netd_tls_wbuf_t;

static void netd_tls_wbuf_reset(netd_tls_wbuf_t* w) {
    if (!w) {
        return;
    }
    w->w = 0;
    memset(w->buf, 0, sizeof(w->buf));
}

static int netd_tls_wbuf_put(netd_tls_wbuf_t* w, const void* data, uint32_t len) {
    if (!w || (!data && len != 0)) {
        return 0;
    }
    if (w->w + len > (uint32_t)sizeof(w->buf)) {
        return 0;
    }
    memcpy(w->buf + w->w, data, len);
    w->w += len;
    return 1;
}

static int netd_tls_wbuf_put_u8(netd_tls_wbuf_t* w, uint8_t v) {
    return netd_tls_wbuf_put(w, &v, 1u);
}

static int netd_tls_wbuf_put_u16(netd_tls_wbuf_t* w, uint16_t v) {
    uint8_t b[2];
    netd_tls_store_be16(b, v);
    return netd_tls_wbuf_put(w, b, (uint32_t)sizeof(b));
}

static int netd_tls_wbuf_put_u24(netd_tls_wbuf_t* w, uint32_t v) {
    uint8_t b[3];
    netd_tls_store_be24(b, v);
    return netd_tls_wbuf_put(w, b, (uint32_t)sizeof(b));
}

static int netd_tls_build_client_hello_ex(
    netd_ctx_t* ctx,
    const char* host,
    const uint8_t* cookie,
    uint32_t cookie_len,
    uint8_t out_hs[2048],
    uint32_t* out_hs_len,
    uint8_t out_x25519_priv[32]
);

static int netd_tls_crypto_selftest_basics(netd_tls_client_t* t);
static int netd_tls_crypto_selftest_aead_for_suite(netd_tls_client_t* t, uint16_t suite);

static int netd_tls_build_client_hello(
    netd_ctx_t* ctx,
    const char* host,
    uint8_t out_hs[2048],
    uint32_t* out_hs_len,
    uint8_t out_x25519_priv[32]
) {
    return netd_tls_build_client_hello_ex(ctx, host, 0, 0, out_hs, out_hs_len, out_x25519_priv);
}

static int netd_tls_build_client_hello_ex(
    netd_ctx_t* ctx,
    const char* host,
    const uint8_t* cookie,
    uint32_t cookie_len,
    uint8_t out_hs[2048],
    uint32_t* out_hs_len,
    uint8_t out_x25519_priv[32]
) {
    if (!ctx || !host || !out_hs || !out_hs_len || !out_x25519_priv) {
        return 0;
    }

    uint32_t host_len = (uint32_t)strlen(host);
    if (host_len == 0 || host_len > 253u) {
        return 0;
    }

    if (!cookie && cookie_len != 0) {
        return 0;
    }

    if (cookie_len > 256u) {
        return 0;
    }

    netd_rand_bytes(&ctx->rand, out_x25519_priv, 32u);

    uint8_t keyshare_pub[32];
    netd_x25519_public_key(keyshare_pub, out_x25519_priv);

    uint8_t random_bytes[32];
    netd_rand_bytes(&ctx->rand, random_bytes, 32u);

    netd_tls_wbuf_t w;
    netd_tls_wbuf_reset(&w);

    if (!netd_tls_wbuf_put_u8(&w, NETD_TLS_HS_CLIENT_HELLO)) {
        return 0;
    }
    if (!netd_tls_wbuf_put_u24(&w, 0)) {
        return 0;
    }

    if (!netd_tls_wbuf_put_u16(&w, 0x0303u)) {
        return 0;
    }
    if (!netd_tls_wbuf_put(&w, random_bytes, (uint32_t)sizeof(random_bytes))) {
        return 0;
    }

    uint8_t session_id[32];
    netd_rand_bytes(&ctx->rand, session_id, (uint32_t)sizeof(session_id));

    if (!netd_tls_wbuf_put_u8(&w, (uint8_t)sizeof(session_id))) {
        return 0;
    }
    if (!netd_tls_wbuf_put(&w, session_id, (uint32_t)sizeof(session_id))) {
        return 0;
    }

    if (!netd_tls_wbuf_put_u16(&w, 4u)) {
        return 0;
    }
    if (!netd_tls_wbuf_put_u16(&w, NETD_TLS_SUITE_AES128GCM_SHA256)) {
        return 0;
    }
    if (!netd_tls_wbuf_put_u16(&w, NETD_TLS_SUITE_CHACHA20POLY1305_SHA256)) {
        return 0;
    }

    if (!netd_tls_wbuf_put_u8(&w, 1u)) {
        return 0;
    }
    if (!netd_tls_wbuf_put_u8(&w, 0u)) {
        return 0;
    }

    uint32_t ext_len_off = w.w;
    if (!netd_tls_wbuf_put_u16(&w, 0)) {
        return 0;
    }

    {
        uint8_t ext_buf[512];
        uint32_t ext_w = 0;

        {
            uint32_t list_len = 1u + 2u + host_len;
            uint32_t sni_len = 2u + list_len;

            uint32_t need = 4u + sni_len;
            if (ext_w + need > (uint32_t)sizeof(ext_buf)) {
                return 0;
            }

            netd_tls_store_be16(ext_buf + ext_w + 0, NETD_TLS_EXT_SERVER_NAME);
            netd_tls_store_be16(ext_buf + ext_w + 2, (uint16_t)sni_len);
            netd_tls_store_be16(ext_buf + ext_w + 4, (uint16_t)list_len);
            ext_buf[ext_w + 6] = 0u;
            netd_tls_store_be16(ext_buf + ext_w + 7, (uint16_t)host_len);
            memcpy(ext_buf + ext_w + 9, host, host_len);
            ext_w += need;
        }

        {
            uint16_t groups[] = { NETD_TLS_GROUP_X25519, 23u };
            uint32_t list_len = (uint32_t)(sizeof(groups) / sizeof(groups[0])) * 2u;
            uint32_t body_len = 2u + list_len;
            uint32_t need = 4u + body_len;
            if (ext_w + need > (uint32_t)sizeof(ext_buf)) {
                return 0;
            }

            netd_tls_store_be16(ext_buf + ext_w + 0, NETD_TLS_EXT_SUPPORTED_GROUPS);
            netd_tls_store_be16(ext_buf + ext_w + 2, (uint16_t)body_len);
            netd_tls_store_be16(ext_buf + ext_w + 4, (uint16_t)list_len);

            uint32_t gw = ext_w + 6u;
            for (uint32_t i = 0; i < (uint32_t)(sizeof(groups) / sizeof(groups[0])); i++) {
                netd_tls_store_be16(ext_buf + gw, groups[i]);
                gw += 2u;
            }
            ext_w += need;
        }

        {
            uint16_t algs[] = { 0x0804u, 0x0403u, 0x0401u };
            uint32_t list_len = (uint32_t)(sizeof(algs) / sizeof(algs[0])) * 2u;
            uint32_t body_len = 2u + list_len;
            uint32_t need = 4u + body_len;
            if (ext_w + need > (uint32_t)sizeof(ext_buf)) {
                return 0;
            }

            netd_tls_store_be16(ext_buf + ext_w + 0, NETD_TLS_EXT_SIGNATURE_ALGORITHMS);
            netd_tls_store_be16(ext_buf + ext_w + 2, (uint16_t)body_len);
            netd_tls_store_be16(ext_buf + ext_w + 4, (uint16_t)list_len);

            uint32_t aw = ext_w + 6u;
            for (uint32_t i = 0; i < (uint32_t)(sizeof(algs) / sizeof(algs[0])); i++) {
                netd_tls_store_be16(ext_buf + aw, algs[i]);
                aw += 2u;
            }
            ext_w += need;
        }

        {
            uint8_t vers[] = { 0x03u, 0x04u };
            uint32_t list_len = (uint32_t)sizeof(vers);
            uint32_t body_len = 1u + list_len;
            uint32_t need = 4u + body_len;
            if (ext_w + need > (uint32_t)sizeof(ext_buf)) {
                return 0;
            }

            netd_tls_store_be16(ext_buf + ext_w + 0, NETD_TLS_EXT_SUPPORTED_VERSIONS);
            netd_tls_store_be16(ext_buf + ext_w + 2, (uint16_t)body_len);
            ext_buf[ext_w + 4] = (uint8_t)list_len;
            memcpy(ext_buf + ext_w + 5, vers, list_len);
            ext_w += need;
        }

        {
            uint32_t key_ex_len = 32u;
            uint32_t share_len = 2u + 2u + key_ex_len;
            uint32_t body_len = 2u + share_len;
            uint32_t need = 4u + body_len;
            if (ext_w + need > (uint32_t)sizeof(ext_buf)) {
                return 0;
            }

            netd_tls_store_be16(ext_buf + ext_w + 0, NETD_TLS_EXT_KEY_SHARE);
            netd_tls_store_be16(ext_buf + ext_w + 2, (uint16_t)body_len);
            netd_tls_store_be16(ext_buf + ext_w + 4, (uint16_t)share_len);
            netd_tls_store_be16(ext_buf + ext_w + 6, NETD_TLS_GROUP_X25519);
            netd_tls_store_be16(ext_buf + ext_w + 8, (uint16_t)key_ex_len);
            memcpy(ext_buf + ext_w + 10, keyshare_pub, key_ex_len);
            ext_w += need;
        }

        if (cookie_len > 0) {
            uint32_t body_len = 2u + cookie_len;
            uint32_t need = 4u + body_len;
            if (ext_w + need > (uint32_t)sizeof(ext_buf)) {
                return 0;
            }

            netd_tls_store_be16(ext_buf + ext_w + 0, NETD_TLS_EXT_COOKIE);
            netd_tls_store_be16(ext_buf + ext_w + 2, (uint16_t)body_len);
            netd_tls_store_be16(ext_buf + ext_w + 4, (uint16_t)cookie_len);
            memcpy(ext_buf + ext_w + 6, cookie, cookie_len);
            ext_w += need;
        }

        if (!netd_tls_wbuf_put(&w, ext_buf, ext_w)) {
            return 0;
        }

        netd_tls_store_be16(w.buf + ext_len_off, (uint16_t)ext_w);
    }

    uint32_t body_len = w.w - 4u;
    w.buf[1] = (uint8_t)(body_len >> 16);
    w.buf[2] = (uint8_t)(body_len >> 8);
    w.buf[3] = (uint8_t)(body_len);

    if (w.w > 2048u) {
        return 0;
    }

    memcpy(out_hs, w.buf, w.w);
    *out_hs_len = w.w;

    netd_tls_wipe(&w, (uint32_t)sizeof(w));
    netd_tls_wipe(keyshare_pub, (uint32_t)sizeof(keyshare_pub));
    netd_tls_wipe(random_bytes, (uint32_t)sizeof(random_bytes));
    netd_tls_wipe(session_id, (uint32_t)sizeof(session_id));
    return 1;
}

static int netd_tls_crypto_selftest_sha256(void) {
    static const uint8_t expected[32] = {
        0xE3u, 0xB0u, 0xC4u, 0x42u, 0x98u, 0xFCu, 0x1Cu, 0x14u,
        0x9Au, 0xFBu, 0xF4u, 0xC8u, 0x99u, 0x6Fu, 0xB9u, 0x24u,
        0x27u, 0xAEu, 0x41u, 0xE4u, 0x64u, 0x9Bu, 0x93u, 0x4Cu,
        0xA4u, 0x95u, 0x99u, 0x1Bu, 0x78u, 0x52u, 0xB8u, 0x55u
    };

    uint8_t got[32];
    memset(got, 0, sizeof(got));
    netd_sha256_hash(0, 0, got);
    return memcmp(got, expected, 32u) == 0;
}

static int netd_tls_crypto_selftest_hkdf(void) {
    static const uint8_t ikm[22] = {
        0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu,
        0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu, 0x0Bu
    };

    static const uint8_t salt[13] = {
        0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu
    };

    static const uint8_t info[10] = {
        0xF0u, 0xF1u, 0xF2u, 0xF3u, 0xF4u, 0xF5u, 0xF6u, 0xF7u, 0xF8u, 0xF9u
    };

    static const uint8_t expected_prk[32] = {
        0x07u, 0x77u, 0x09u, 0x36u, 0x2Cu, 0x2Eu, 0x32u, 0xDFu,
        0x0Du, 0xDCu, 0x3Fu, 0x0Du, 0xC4u, 0x7Bu, 0xBAu, 0x63u,
        0x90u, 0xB6u, 0xC7u, 0x3Bu, 0xB5u, 0x0Fu, 0x9Cu, 0x31u,
        0x22u, 0xECu, 0x84u, 0x4Au, 0xD7u, 0xC2u, 0xB3u, 0xE5u
    };

    static const uint8_t expected_okm[42] = {
        0x3Cu, 0xB2u, 0x5Fu, 0x25u, 0xFAu, 0xACu, 0xD5u, 0x7Au,
        0x90u, 0x43u, 0x4Fu, 0x64u, 0xD0u, 0x36u, 0x2Fu, 0x2Au,
        0x2Du, 0x2Du, 0x0Au, 0x90u, 0xCFu, 0x1Au, 0x5Au, 0x4Cu,
        0x5Du, 0xB0u, 0x2Du, 0x56u, 0xECu, 0xC4u, 0xC5u, 0xBFu,
        0x34u, 0x00u, 0x72u, 0x08u, 0xD5u, 0xB8u, 0x87u, 0x18u,
        0x58u, 0x65u
    };

    uint8_t prk[32];
    memset(prk, 0, sizeof(prk));
    netd_hkdf_sha256_extract(salt, (uint32_t)sizeof(salt), ikm, (uint32_t)sizeof(ikm), prk);
    if (memcmp(prk, expected_prk, 32u) != 0) {
        netd_tls_wipe(prk, (uint32_t)sizeof(prk));
        return 0;
    }

    uint8_t okm[42];
    memset(okm, 0, sizeof(okm));
    if (!netd_hkdf_sha256_expand(prk, info, (uint32_t)sizeof(info), okm, (uint32_t)sizeof(okm))) {
        netd_tls_wipe(prk, (uint32_t)sizeof(prk));
        netd_tls_wipe(okm, (uint32_t)sizeof(okm));
        return 0;
    }

    int ok = memcmp(okm, expected_okm, (uint32_t)sizeof(expected_okm)) == 0;
    netd_tls_wipe(prk, (uint32_t)sizeof(prk));
    netd_tls_wipe(okm, (uint32_t)sizeof(okm));
    return ok;
}

static int netd_tls_crypto_selftest_x25519(uint16_t* out_fail_code) {
    if (out_fail_code) {
        *out_fail_code = 0;
    }

    static const uint8_t alice_priv[32] = {
        0x77u, 0x07u, 0x6Du, 0x0Au, 0x73u, 0x18u, 0xA5u, 0x7Du,
        0x3Cu, 0x16u, 0xC1u, 0x72u, 0x51u, 0xB2u, 0x66u, 0x45u,
        0xDFu, 0x4Cu, 0x2Fu, 0x87u, 0xEBu, 0xC0u, 0x99u, 0x2Au,
        0xB1u, 0x77u, 0xFBu, 0xA5u, 0x1Du, 0xB9u, 0x2Cu, 0x2Au
    };

    static const uint8_t bob_pub[32] = {
        0xDEu, 0x9Eu, 0xDBu, 0x7Du, 0x7Bu, 0x7Du, 0xC1u, 0xB4u,
        0xD3u, 0x5Bu, 0x61u, 0xC2u, 0xECu, 0xE4u, 0x35u, 0x37u,
        0x3Fu, 0x83u, 0x43u, 0xC8u, 0x5Bu, 0x78u, 0x67u, 0x4Du,
        0xADu, 0xFCu, 0x7Eu, 0x14u, 0x6Fu, 0x88u, 0x2Bu, 0x4Fu
    };

    static const uint8_t expected_alice_pub[32] = {
        0x85u, 0x20u, 0xF0u, 0x09u, 0x89u, 0x30u, 0xA7u, 0x54u,
        0x74u, 0x8Bu, 0x7Du, 0xDCu, 0xB4u, 0x3Eu, 0xF7u, 0x5Au,
        0x0Du, 0xBFu, 0x3Au, 0x0Du, 0x26u, 0x38u, 0x1Au, 0xF4u,
        0xEBu, 0xA4u, 0xA9u, 0x8Eu, 0xAAu, 0x9Bu, 0x4Eu, 0x6Au
    };

    static const uint8_t expected_shared[32] = {
        0x4Au, 0x5Du, 0x9Du, 0x5Bu, 0xA4u, 0xCEu, 0x2Du, 0xE1u,
        0x72u, 0x8Eu, 0x3Bu, 0xF4u, 0x80u, 0x35u, 0x0Fu, 0x25u,
        0xE0u, 0x7Eu, 0x21u, 0xC9u, 0x47u, 0xD1u, 0x9Eu, 0x33u,
        0x76u, 0xF0u, 0x9Bu, 0x3Cu, 0x1Eu, 0x16u, 0x17u, 0x42u
    };

    uint8_t alice_pub[32];
    memset(alice_pub, 0, sizeof(alice_pub));
    netd_x25519_public_key(alice_pub, alice_priv);
    if (memcmp(alice_pub, expected_alice_pub, 32u) != 0) {
        if (out_fail_code) {
            *out_fail_code = NET_HTTP_TLS_INTERNAL_SELFTEST_X25519_PUB;
        }
        netd_tls_wipe(alice_pub, (uint32_t)sizeof(alice_pub));
        return 0;
    }

    uint8_t shared[32];
    memset(shared, 0, sizeof(shared));
    netd_x25519(shared, alice_priv, bob_pub);
    if (memcmp(shared, expected_shared, 32u) != 0) {
        if (out_fail_code) {
            *out_fail_code = NET_HTTP_TLS_INTERNAL_SELFTEST_X25519_SHARED;
        }
        netd_tls_wipe(alice_pub, (uint32_t)sizeof(alice_pub));
        netd_tls_wipe(shared, (uint32_t)sizeof(shared));
        return 0;
    }
    netd_tls_wipe(alice_pub, (uint32_t)sizeof(alice_pub));
    netd_tls_wipe(shared, (uint32_t)sizeof(shared));
    return 1;
}

static int netd_tls_crypto_selftest_aesgcm(void) {
    static const uint8_t key[16] = {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };

    static const uint8_t nonce[12] = {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };

    static const uint8_t plaintext[16] = {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };

    static const uint8_t expected_ciphertext[16] = {
        0x03u, 0x88u, 0xDAu, 0xCEu, 0x60u, 0xB6u, 0xA3u, 0x92u,
        0xF3u, 0x28u, 0xC2u, 0xB9u, 0x71u, 0xB2u, 0xFEu, 0x78u
    };

    static const uint8_t expected_tag[16] = {
        0xABu, 0x6Eu, 0x47u, 0xD4u, 0x2Cu, 0xECu, 0x13u, 0xBDu,
        0xF5u, 0x3Au, 0x67u, 0xB2u, 0x12u, 0x57u, 0xBDu, 0xDFu
    };

    uint8_t ciphertext[16];
    uint8_t tag[16];
    memset(ciphertext, 0, sizeof(ciphertext));
    memset(tag, 0, sizeof(tag));

    if (!netd_aead_aes128gcm_seal(key, nonce, 0, 0, plaintext, (uint32_t)sizeof(plaintext), ciphertext, tag)) {
        return 0;
    }

    if (memcmp(ciphertext, expected_ciphertext, 16u) != 0) {
        return 0;
    }

    if (memcmp(tag, expected_tag, 16u) != 0) {
        return 0;
    }

    uint8_t opened[16];
    memset(opened, 0, sizeof(opened));
    if (!netd_aead_aes128gcm_open(key, nonce, 0, 0, ciphertext, (uint32_t)sizeof(ciphertext), tag, opened)) {
        return 0;
    }

    return memcmp(opened, plaintext, 16u) == 0;
}

static int netd_tls_crypto_selftest_chacha20poly1305(void) {
    static const uint8_t key[32] = {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };

    static const uint8_t nonce[12] = {
        0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u
    };

    static const uint8_t plaintext[32] = {
        0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
        0x08u, 0x09u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
        0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u,
        0x18u, 0x19u, 0x1Au, 0x1Bu, 0x1Cu, 0x1Du, 0x1Eu, 0x1Fu
    };

    uint8_t ciphertext[32];
    uint8_t tag[16];
    memset(ciphertext, 0, sizeof(ciphertext));
    memset(tag, 0, sizeof(tag));

    if (!netd_aead_chacha20poly1305_seal(key, nonce, 0, 0, plaintext, (uint32_t)sizeof(plaintext), ciphertext, tag)) {
        return 0;
    }

    uint8_t opened[32];
    memset(opened, 0, sizeof(opened));
    if (!netd_aead_chacha20poly1305_open(key, nonce, 0, 0, ciphertext, (uint32_t)sizeof(ciphertext), tag, opened)) {
        return 0;
    }

    return memcmp(opened, plaintext, (uint32_t)sizeof(plaintext)) == 0;
}

static int netd_tls_crypto_selftest_basics(netd_tls_client_t* t) {
    if (!t) {
        return 0;
    }

    static int passed = 0;
    if (passed) {
        return 1;
    }

    if (!netd_tls_crypto_selftest_sha256()) {
        netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_SELFTEST_SHA256);
        return 0;
    }

    if (!netd_tls_crypto_selftest_hkdf()) {
        netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_SELFTEST_HKDF);
        return 0;
    }

    uint16_t x25519_fail_code = 0;
    if (!netd_tls_crypto_selftest_x25519(&x25519_fail_code)) {
        if (x25519_fail_code != 0) {
            netd_tls_set_internal_alert(t, x25519_fail_code);
        } else {
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_SELFTEST_X25519);
        }
        return 0;
    }

    passed = 1;
    return 1;
}

static int netd_tls_crypto_selftest_aead_for_suite(netd_tls_client_t* t, uint16_t suite) {
    if (!t) {
        return 0;
    }

    static int passed_aesgcm = 0;
    static int passed_chacha = 0;

    if (suite == NETD_TLS_SUITE_AES128GCM_SHA256) {
        if (passed_aesgcm) {
            return 1;
        }

        if (!netd_tls_crypto_selftest_aesgcm()) {
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_SELFTEST_AESGCM);
            return 0;
        }

        passed_aesgcm = 1;
        return 1;
    }

    if (suite == NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        if (passed_chacha) {
            return 1;
        }

        if (!netd_tls_crypto_selftest_chacha20poly1305()) {
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_SELFTEST_CHACHA20POLY1305);
            return 0;
        }

        passed_chacha = 1;
        return 1;
    }

    return 1;
}

static int netd_tls_is_hello_retry_request(const uint8_t* hs, uint32_t hs_len) {
    static const uint8_t hrr_random[32] = {
        0xCFu, 0x21u, 0xADu, 0x74u, 0xE5u, 0x9Au, 0x61u, 0x11u,
        0xBEu, 0x1Du, 0x8Cu, 0x02u, 0x1Eu, 0x65u, 0xB8u, 0x91u,
        0xC2u, 0xA2u, 0x11u, 0x16u, 0x7Au, 0xBBu, 0x8Cu, 0x5Eu,
        0x07u, 0x9Eu, 0x09u, 0xE2u, 0xC8u, 0xA8u, 0x33u, 0x9Cu
    };

    if (!hs || hs_len < 4u) {
        return 0;
    }

    if (hs[0] != NETD_TLS_HS_SERVER_HELLO) {
        return 0;
    }

    uint32_t body_len = netd_tls_load_be24(hs + 1);
    if (4u + body_len != hs_len) {
        return 0;
    }

    if (body_len < 2u + 32u) {
        return 0;
    }

    const uint8_t* p = hs + 4u;
    p += 2u;
    return memcmp(p, hrr_random, 32u) == 0;
}

static int netd_tls_parse_hello_retry_request(
    const uint8_t* hs,
    uint32_t hs_len,
    uint16_t* out_suite,
    uint16_t* out_selected_group,
    uint8_t* out_cookie,
    uint32_t out_cookie_cap,
    uint32_t* out_cookie_len
) {
    if (out_cookie_len) {
        *out_cookie_len = 0;
    }

    if (!hs || !out_suite || !out_selected_group || !out_cookie_len) {
        return 0;
    }

    if (!netd_tls_is_hello_retry_request(hs, hs_len)) {
        return 0;
    }

    uint32_t body_len = netd_tls_load_be24(hs + 1);
    const uint8_t* p = hs + 4;
    uint32_t rem = body_len;

    if (rem < 2u + 32u + 1u) {
        return 0;
    }

    uint16_t legacy_version = netd_tls_load_be16(p);
    (void)legacy_version;
    p += 2u;
    rem -= 2u;

    p += 32u;
    rem -= 32u;

    uint8_t sid_len = p[0];
    p += 1u;
    rem -= 1u;
    if (rem < sid_len + 2u + 1u + 2u) {
        return 0;
    }
    p += sid_len;
    rem -= sid_len;

    uint16_t suite = netd_tls_load_be16(p);
    p += 2u;
    rem -= 2u;

    uint8_t comp = p[0];
    (void)comp;
    p += 1u;
    rem -= 1u;

    uint16_t ext_len = netd_tls_load_be16(p);
    p += 2u;
    rem -= 2u;
    if (rem < ext_len) {
        return 0;
    }

    int have_supported_versions = 0;
    int have_key_share = 0;

    uint16_t selected_group = 0;

    const uint8_t* ex = p;
    uint32_t ex_rem = ext_len;
    while (ex_rem >= 4u) {
        uint16_t et = netd_tls_load_be16(ex + 0);
        uint16_t el = netd_tls_load_be16(ex + 2);
        ex += 4u;
        ex_rem -= 4u;
        if (ex_rem < el) {
            return 0;
        }

        const uint8_t* ed = ex;
        if (et == NETD_TLS_EXT_SUPPORTED_VERSIONS) {
            if (el == 2u) {
                uint16_t v = netd_tls_load_be16(ed);
                if (v == 0x0304u) {
                    have_supported_versions = 1;
                }
            }
        } else if (et == NETD_TLS_EXT_KEY_SHARE) {
            if (el == 2u) {
                selected_group = netd_tls_load_be16(ed);
                have_key_share = 1;
            }
        } else if (et == NETD_TLS_EXT_COOKIE) {
            if (el >= 2u) {
                uint16_t cl = netd_tls_load_be16(ed);
                if (el == 2u + cl) {
                    if (cl > out_cookie_cap) {
                        return 0;
                    }
                    if (cl > 0) {
                        if (!out_cookie) {
                            return 0;
                        }
                        memcpy(out_cookie, ed + 2u, cl);
                    }
                    *out_cookie_len = cl;
                }
            }
        }

        ex += el;
        ex_rem -= el;
    }

    if (!have_supported_versions || !have_key_share) {
        return 0;
    }

    if (selected_group == 0) {
        return 0;
    }

    if (suite != NETD_TLS_SUITE_AES128GCM_SHA256 && suite != NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        return 0;
    }

    *out_suite = suite;
    *out_selected_group = selected_group;
    return 1;
}

static void netd_tls_transcript_apply_hello_retry_request(
    netd_sha256_t* tr,
    const uint8_t* client_hello,
    uint32_t client_hello_len,
    const uint8_t* hrr,
    uint32_t hrr_len
) {
    if (!tr || (!client_hello && client_hello_len != 0) || (!hrr && hrr_len != 0)) {
        return;
    }

    uint8_t ch_hash[32];
    netd_sha256_hash(client_hello, client_hello_len, ch_hash);

    uint8_t msg_hash[4u + 32u];
    msg_hash[0] = NETD_TLS_HS_MESSAGE_HASH;
    netd_tls_store_be24(msg_hash + 1u, 32u);
    memcpy(msg_hash + 4u, ch_hash, 32u);

    netd_tls_transcript_init(tr);
    netd_tls_transcript_update(tr, msg_hash, (uint32_t)sizeof(msg_hash));
    netd_tls_transcript_update(tr, hrr, hrr_len);

    netd_tls_wipe(ch_hash, (uint32_t)sizeof(ch_hash));
    netd_tls_wipe(msg_hash, (uint32_t)sizeof(msg_hash));
}

static int netd_tls_parse_server_hello(
    const uint8_t* hs,
    uint32_t hs_len,
    uint16_t* out_suite,
    uint8_t out_server_pub[32]
) {
    if (!hs || hs_len < 4u || !out_suite || !out_server_pub) {
        return 0;
    }

    if (hs[0] != NETD_TLS_HS_SERVER_HELLO) {
        return 0;
    }

    uint32_t body_len = netd_tls_load_be24(hs + 1);
    if (4u + body_len != hs_len) {
        return 0;
    }

    const uint8_t* p = hs + 4;
    uint32_t rem = body_len;

    if (rem < 2u + 32u + 1u) {
        return 0;
    }

    uint16_t legacy_version = netd_tls_load_be16(p);
    (void)legacy_version;
    p += 2u;
    rem -= 2u;

    p += 32u;
    rem -= 32u;

    uint8_t sid_len = p[0];
    p += 1u;
    rem -= 1u;
    if (rem < sid_len + 2u + 1u + 2u) {
        return 0;
    }
    p += sid_len;
    rem -= sid_len;

    uint16_t suite = netd_tls_load_be16(p);
    p += 2u;
    rem -= 2u;

    uint8_t comp = p[0];
    (void)comp;
    p += 1u;
    rem -= 1u;

    uint16_t ext_len = netd_tls_load_be16(p);
    p += 2u;
    rem -= 2u;
    if (rem < ext_len) {
        return 0;
    }

    int have_supported_versions = 0;
    int have_key_share = 0;

    const uint8_t* ex = p;
    uint32_t ex_rem = ext_len;
    while (ex_rem >= 4u) {
        uint16_t et = netd_tls_load_be16(ex + 0);
        uint16_t el = netd_tls_load_be16(ex + 2);
        ex += 4u;
        ex_rem -= 4u;
        if (ex_rem < el) {
            return 0;
        }

        const uint8_t* ed = ex;
        if (et == NETD_TLS_EXT_SUPPORTED_VERSIONS) {
            if (el == 2u) {
                uint16_t v = netd_tls_load_be16(ed);
                if (v == 0x0304u) {
                    have_supported_versions = 1;
                }
            }
        } else if (et == NETD_TLS_EXT_KEY_SHARE) {
            if (el >= 2u + 2u) {
                uint16_t group = netd_tls_load_be16(ed);
                uint16_t klen = netd_tls_load_be16(ed + 2u);
                if (group == NETD_TLS_GROUP_X25519 && klen == 32u && el == 4u + klen) {
                    memcpy(out_server_pub, ed + 4u, 32u);
                    have_key_share = 1;
                }
            }
        }

        ex += el;
        ex_rem -= el;
    }

    if (!have_supported_versions || !have_key_share) {
        return 0;
    }

    if (suite != NETD_TLS_SUITE_AES128GCM_SHA256 && suite != NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        return 0;
    }

    *out_suite = suite;
    return 1;
}

static void netd_tls_derive_traffic_key_iv(
    const uint8_t traffic_secret[32],
    uint32_t key_len,
    uint8_t out_key[32],
    uint8_t out_iv[12]
) {
    if (!traffic_secret || !out_key || !out_iv) {
        return;
    }

    if (key_len > 32u) {
        key_len = 32u;
    }

    memset(out_key, 0, 32u);
    (void)netd_hkdf_sha256_expand_label(traffic_secret, "key", 0, 0, out_key, key_len);
    (void)netd_hkdf_sha256_expand_label(traffic_secret, "iv", 0, 0, out_iv, 12u);
}

static void netd_tls_derive_finished_key(const uint8_t traffic_secret[32], uint8_t out_finished_key[32]) {
    (void)netd_hkdf_sha256_expand_label(traffic_secret, "finished", 0, 0, out_finished_key, 32u);
}

static void netd_tls_derive_secret(const uint8_t secret[32], const char* label, const uint8_t transcript_hash[32], uint8_t out[32]) {
    (void)netd_hkdf_sha256_expand_label(secret, label, transcript_hash, 32u, out, 32u);
}

static int netd_tls_send_finished(
    netd_ctx_t* ctx,
    netd_tls_client_t* t,
    netd_sha256_t* transcript,
    const uint8_t finished_key[32],
    const netd_tls_deadline_t* deadline
) {
    if (!ctx || !t || !transcript || !finished_key) {
        return 0;
    }

    uint8_t th[32];
    netd_tls_transcript_hash(transcript, th);

    uint8_t verify_data[32];
    netd_hmac_sha256(finished_key, 32u, th, (uint32_t)sizeof(th), verify_data);

    uint8_t hs[4 + 32];
    hs[0] = NETD_TLS_HS_FINISHED;
    netd_tls_store_be24(hs + 1, 32u);
    memcpy(hs + 4, verify_data, 32u);

    uint8_t rec_hdr[5];
    uint8_t rec_body[4 + 32 + 1 + 16];
    uint32_t rec_body_len = 0;

    if (!netd_tls_seal_record(
            rec_hdr,
            rec_body,
            &rec_body_len,
            t->suite,
            t->hs_key_w,
            t->hs_iv_w,
            &t->hs_seq_w,
            hs,
            (uint32_t)sizeof(hs),
            NETD_TLS_CT_HANDSHAKE
        )) {
        return 0;
    }

    if (!netd_tls_tcp_write_all_deadline(ctx, t, rec_hdr, 5u, deadline)) {
        return 0;
    }
    if (!netd_tls_tcp_write_all_deadline(ctx, t, rec_body, rec_body_len, deadline)) {
        return 0;
    }

    netd_tls_transcript_update(transcript, hs, (uint32_t)sizeof(hs));

    netd_tls_wipe(th, (uint32_t)sizeof(th));
    netd_tls_wipe(verify_data, (uint32_t)sizeof(verify_data));
    netd_tls_wipe(hs, (uint32_t)sizeof(hs));
    netd_tls_wipe(rec_body, (uint32_t)sizeof(rec_body));
    return 1;
}

static int netd_tls_recv_handshake_message(
    netd_ctx_t* ctx,
    netd_tls_client_t* t,
    netd_tls_hs_rx_t* hs_rx,
    uint8_t* out_hs,
    uint32_t out_cap,
    uint32_t* out_len,
    const netd_tls_deadline_t* deadline
) {
    if (out_len) {
        *out_len = 0;
    }

    if (!ctx || !t || !hs_rx || !out_hs || !out_len) {
        return 0;
    }

    uint32_t avail = netd_tls_hs_rx_avail(hs_rx);
    while (avail < 4u) {
        uint8_t rec_hdr[5];
        if (!netd_tls_read_record_header_deadline(ctx, t, rec_hdr, deadline)) {
            netd_tls_mark_io_failure(ctx, t);
            return 0;
        }

        uint16_t rec_len = netd_tls_load_be16(rec_hdr + 3);
        if (rec_len == 0 || rec_len > 16384u + 256u) {
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_BAD_RECORD);
            return 0;
        }

        uint8_t rec_body[16384u + 256u];
        if (!netd_tls_read_record_body_deadline(ctx, t, rec_body, rec_len, deadline)) {
            netd_tls_mark_io_failure(ctx, t);
            return 0;
        }

        if (t->prot_read == NETD_TLS_PROT_NONE) {
            if (rec_hdr[0] == NETD_TLS_CT_HANDSHAKE) {
                (void)netd_tls_hs_rx_push(hs_rx, rec_body, rec_len);
            } else if (rec_hdr[0] == NETD_TLS_CT_CHANGE_CIPHER_SPEC) {
                continue;
            } else if (rec_hdr[0] == NETD_TLS_CT_ALERT) {
                netd_tls_capture_alert(t, rec_body, rec_len);
                return 0;
            }
        } else {
            if (rec_hdr[0] == NETD_TLS_CT_APPLICATION_DATA) {
                uint8_t* payload = 0;
                uint32_t payload_len = 0;
                uint8_t inner_type = 0;

                const uint8_t* key = 0;
                const uint8_t* iv = 0;
                uint64_t* seq = 0;
                if (t->prot_read == NETD_TLS_PROT_HANDSHAKE) {
                    key = t->hs_key_r;
                    iv = t->hs_iv_r;
                    seq = &t->hs_seq_r;
                } else {
                    key = t->app_key_r;
                    iv = t->app_iv_r;
                    seq = &t->app_seq_r;
                }

                if (!netd_tls_open_record(rec_body, rec_len, t->suite, key, iv, seq, rec_hdr, &inner_type, &payload, &payload_len)) {
                    netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_DECRYPT);
                    return 0;
                }

                if (inner_type == NETD_TLS_CT_HANDSHAKE) {
                    (void)netd_tls_hs_rx_push(hs_rx, payload, payload_len);
                } else if (inner_type == NETD_TLS_CT_ALERT) {
                    netd_tls_capture_alert(t, payload, payload_len);
                    return 0;
                }
            }
        }

        avail = netd_tls_hs_rx_avail(hs_rx);
    }

    uint8_t hdr[4];
    if (!netd_tls_hs_rx_peek(hs_rx, 0, hdr, 4u)) {
        return 0;
    }

    uint32_t body_len = netd_tls_load_be24(hdr + 1);
    uint32_t total_len = 4u + body_len;
    if (total_len > out_cap) {
        return 0;
    }

    while (netd_tls_hs_rx_avail(hs_rx) < total_len) {
        uint8_t rec_hdr[5];
        if (!netd_tls_read_record_header_deadline(ctx, t, rec_hdr, deadline)) {
            netd_tls_mark_io_failure(ctx, t);
            return 0;
        }

        uint16_t rec_len = netd_tls_load_be16(rec_hdr + 3);
        if (rec_len == 0 || rec_len > 16384u + 256u) {
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_BAD_RECORD);
            return 0;
        }

        uint8_t rec_body[16384u + 256u];
        if (!netd_tls_read_record_body_deadline(ctx, t, rec_body, rec_len, deadline)) {
            netd_tls_mark_io_failure(ctx, t);
            return 0;
        }

        if (t->prot_read == NETD_TLS_PROT_NONE) {
            if (rec_hdr[0] == NETD_TLS_CT_HANDSHAKE) {
                (void)netd_tls_hs_rx_push(hs_rx, rec_body, rec_len);
            } else if (rec_hdr[0] == NETD_TLS_CT_CHANGE_CIPHER_SPEC) {
                continue;
            } else if (rec_hdr[0] == NETD_TLS_CT_ALERT) {
                netd_tls_capture_alert(t, rec_body, rec_len);
                return 0;
            }
            continue;
        }

        if (rec_hdr[0] == NETD_TLS_CT_CHANGE_CIPHER_SPEC) {
            continue;
        }

        if (rec_hdr[0] != NETD_TLS_CT_APPLICATION_DATA) {
            continue;
        }

        uint8_t* payload = 0;
        uint32_t payload_len = 0;
        uint8_t inner_type = 0;

        const uint8_t* key = 0;
        const uint8_t* iv = 0;
        uint64_t* seq = 0;
        if (t->prot_read == NETD_TLS_PROT_HANDSHAKE) {
            key = t->hs_key_r;
            iv = t->hs_iv_r;
            seq = &t->hs_seq_r;
        } else {
            key = t->app_key_r;
            iv = t->app_iv_r;
            seq = &t->app_seq_r;
        }

        if (!netd_tls_open_record(rec_body, rec_len, t->suite, key, iv, seq, rec_hdr, &inner_type, &payload, &payload_len)) {
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_DECRYPT);
            return 0;
        }

        if (inner_type == NETD_TLS_CT_HANDSHAKE) {
            (void)netd_tls_hs_rx_push(hs_rx, payload, payload_len);
        } else if (inner_type == NETD_TLS_CT_ALERT) {
            netd_tls_capture_alert(t, payload, payload_len);
            return 0;
        }
    }

    if (!netd_tls_hs_rx_peek(hs_rx, 0, out_hs, total_len)) {
        return 0;
    }
    if (!netd_tls_hs_rx_drop(hs_rx, total_len)) {
        return 0;
    }
    *out_len = total_len;
    return 1;
}

static int netd_tls_ingest_handshake_bytes(
    netd_ctx_t* ctx,
    netd_tls_client_t* t,
    netd_tls_hs_rx_t* hs_rx,
    const netd_tls_deadline_t* deadline
) {
    if (!ctx || !t || !hs_rx) {
        return 0;
    }

    uint8_t rec_hdr[5];
    if (!netd_tls_read_record_header_deadline(ctx, t, rec_hdr, deadline)) {
        netd_tls_mark_io_failure(ctx, t);
        return 0;
    }

    uint16_t rec_len = netd_tls_load_be16(rec_hdr + 3);
    if (rec_len == 0 || rec_len > 16384u + 256u) {
        netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_BAD_RECORD);
        return 0;
    }

    uint8_t rec_body[16384u + 256u];
    if (!netd_tls_read_record_body_deadline(ctx, t, rec_body, rec_len, deadline)) {
        netd_tls_mark_io_failure(ctx, t);
        return 0;
    }

    if (t->prot_read == NETD_TLS_PROT_NONE) {
        if (rec_hdr[0] == NETD_TLS_CT_HANDSHAKE) {
            (void)netd_tls_hs_rx_push(hs_rx, rec_body, rec_len);
        } else if (rec_hdr[0] == NETD_TLS_CT_CHANGE_CIPHER_SPEC) {
            return 1;
        } else if (rec_hdr[0] == NETD_TLS_CT_ALERT) {
            netd_tls_capture_alert(t, rec_body, rec_len);
            return 0;
        }
        return 1;
    }

    if (rec_hdr[0] != NETD_TLS_CT_APPLICATION_DATA) {
        return 1;
    }

    uint8_t* payload = 0;
    uint32_t payload_len = 0;
    uint8_t inner_type = 0;

    const uint8_t* key = 0;
    const uint8_t* iv = 0;
    uint64_t* seq = 0;
    if (t->prot_read == NETD_TLS_PROT_HANDSHAKE) {
        key = t->hs_key_r;
        iv = t->hs_iv_r;
        seq = &t->hs_seq_r;
    } else {
        key = t->app_key_r;
        iv = t->app_iv_r;
        seq = &t->app_seq_r;
    }

    if (!netd_tls_open_record(rec_body, rec_len, t->suite, key, iv, seq, rec_hdr, &inner_type, &payload, &payload_len)) {
        netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_DECRYPT);
        return 0;
    }

    if (inner_type == NETD_TLS_CT_HANDSHAKE) {
        (void)netd_tls_hs_rx_push(hs_rx, payload, payload_len);
        return 1;
    }

    if (inner_type == NETD_TLS_CT_ALERT) {
        netd_tls_capture_alert(t, payload, payload_len);
        return 0;
    }

    return 1;
}

static int netd_tls_peek_handshake_header(
    netd_ctx_t* ctx,
    netd_tls_client_t* t,
    netd_tls_hs_rx_t* hs_rx,
    uint8_t out_hdr[4],
    const netd_tls_deadline_t* deadline
) {
    if (!ctx || !t || !hs_rx || !out_hdr) {
        return 0;
    }

    while (netd_tls_hs_rx_avail(hs_rx) < 4u) {
        if (!netd_tls_ingest_handshake_bytes(ctx, t, hs_rx, deadline)) {
            return 0;
        }
    }

    return netd_tls_hs_rx_peek(hs_rx, 0, out_hdr, 4u);
}

static int netd_tls_discard_handshake_message(
    netd_ctx_t* ctx,
    netd_tls_client_t* t,
    netd_tls_hs_rx_t* hs_rx,
    netd_sha256_t* transcript,
    const netd_tls_deadline_t* deadline
) {
    if (!ctx || !t || !hs_rx || !transcript) {
        return 0;
    }

    uint8_t hdr[4];
    if (!netd_tls_peek_handshake_header(ctx, t, hs_rx, hdr, deadline)) {
        return 0;
    }

    uint32_t body_len = netd_tls_load_be24(hdr + 1);
    uint32_t remaining = 4u + body_len;

    uint8_t tmp[512];
    while (remaining > 0) {
        uint32_t avail = netd_tls_hs_rx_avail(hs_rx);
        if (avail == 0) {
            if (!netd_tls_ingest_handshake_bytes(ctx, t, hs_rx, deadline)) {
                return 0;
            }
            continue;
        }

        uint32_t take = avail;
        if (take > remaining) {
            take = remaining;
        }
        if (take > (uint32_t)sizeof(tmp)) {
            take = (uint32_t)sizeof(tmp);
        }

        if (!netd_tls_hs_rx_peek(hs_rx, 0, tmp, take)) {
            return 0;
        }
        netd_tls_transcript_update(transcript, tmp, take);
        if (!netd_tls_hs_rx_drop(hs_rx, take)) {
            return 0;
        }

        remaining -= take;
    }

    netd_tls_wipe(tmp, (uint32_t)sizeof(tmp));
    return 1;
}

static void netd_tls_client_init(netd_tls_client_t* t) {
    if (!t) {
        return;
    }

    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->prot_read = NETD_TLS_PROT_NONE;
    t->prot_write = NETD_TLS_PROT_NONE;
    t->hs_seq_r = 0;
    t->hs_seq_w = 0;
    t->app_seq_r = 0;
    t->app_seq_w = 0;
    t->rx_r = 0;
    t->rx_w = 0;
    t->hs_step = 0;
    t->hs_status = NET_STATUS_OK;
    t->hs_alert = 0;
}

static void netd_tls_client_reset(netd_tls_client_t* t) {
    if (!t) {
        return;
    }

    uint32_t hs_step = t->hs_step;
    uint32_t hs_status = t->hs_status;
    uint16_t hs_alert = t->hs_alert;

    t->active = 0;
    t->ready = 0;
    t->closed = 0;
    netd_tls_wipe(t, (uint32_t)sizeof(*t));

    t->hs_step = hs_step;
    t->hs_status = hs_status;
    t->hs_alert = hs_alert;
}

int netd_tls_handshake(
    netd_ctx_t* ctx,
    netd_tls_client_t* t,
    netd_tcp_conn_t* tcp,
    const char* host,
    uint32_t timeout_ms
) {
    if (!ctx || !t || !tcp || !host) {
        return 0;
    }

    netd_tls_deadline_t deadline;
    netd_tls_deadline_init(&deadline, timeout_ms);

    netd_tls_client_init(t);
    t->tcp = tcp;
    t->hs_status = NET_STATUS_ERROR;

    t->hs_step = NET_HTTP_TLS_STEP_BUILD_CLIENT_HELLO;
    if (!netd_tls_crypto_selftest_basics(t)) {
        t->hs_status = NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        return 0;
    }

    uint8_t client_priv[32];
    uint8_t client_hello[2048];
    uint32_t client_hello_len = 0;

    memset(client_priv, 0, sizeof(client_priv));
    memset(client_hello, 0, sizeof(client_hello));

    if (!netd_tls_build_client_hello(ctx, host, client_hello, &client_hello_len, client_priv)) {
        t->hs_status = NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        netd_tls_wipe(client_priv, (uint32_t)sizeof(client_priv));
        netd_tls_wipe(client_hello, (uint32_t)sizeof(client_hello));
        return 0;
    }

    netd_sha256_t transcript;
    netd_tls_transcript_init(&transcript);
    netd_tls_transcript_update(&transcript, client_hello, client_hello_len);

    t->hs_step = NET_HTTP_TLS_STEP_SEND_CLIENT_HELLO;
    if (!netd_tls_write_record_plain_deadline(ctx, t, NETD_TLS_CT_HANDSHAKE, client_hello, client_hello_len, &deadline)) {
        t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        netd_tls_wipe(client_priv, (uint32_t)sizeof(client_priv));
        netd_tls_wipe(client_hello, (uint32_t)sizeof(client_hello));
        return 0;
    }

    {
        uint8_t ccs = 1u;
        if (!netd_tls_write_record_plain_deadline(ctx, t, NETD_TLS_CT_CHANGE_CIPHER_SPEC, &ccs, 1u, &deadline)) {
            t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            netd_tls_wipe(client_priv, (uint32_t)sizeof(client_priv));
            netd_tls_wipe(client_hello, (uint32_t)sizeof(client_hello));
            return 0;
        }
    }

    netd_tls_hs_rx_t hs_rx;
    netd_tls_hs_rx_reset(&hs_rx);

    uint8_t server_hello[2048];
    uint32_t server_hello_len = 0;
    memset(server_hello, 0, sizeof(server_hello));

    t->hs_step = NET_HTTP_TLS_STEP_RECV_SERVER_HELLO;
    if (!netd_tls_recv_handshake_message(ctx, t, &hs_rx, server_hello, (uint32_t)sizeof(server_hello), &server_hello_len, &deadline)) {
        t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        goto cleanup;
    }

    uint8_t hrr_cookie[256];
    uint32_t hrr_cookie_len = 0;
    uint16_t hrr_suite = 0;
    uint16_t hrr_group = 0;
    memset(hrr_cookie, 0, sizeof(hrr_cookie));

    if (netd_tls_is_hello_retry_request(server_hello, server_hello_len)) {
        t->hs_step = NET_HTTP_TLS_STEP_PARSE_SERVER_HELLO;
        if (!netd_tls_parse_hello_retry_request(
                server_hello,
                server_hello_len,
                &hrr_suite,
                &hrr_group,
                hrr_cookie,
                (uint32_t)sizeof(hrr_cookie),
                &hrr_cookie_len
            )) {
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        if (hrr_group != NETD_TLS_GROUP_X25519) {
            t->hs_status = NET_STATUS_UNSUPPORTED;
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_HRR);
            netd_tls_client_reset(t);
            goto cleanup;
        }

        netd_tls_transcript_apply_hello_retry_request(&transcript, client_hello, client_hello_len, server_hello, server_hello_len);

        memset(client_priv, 0, sizeof(client_priv));
        memset(client_hello, 0, sizeof(client_hello));
        client_hello_len = 0;

        t->hs_step = NET_HTTP_TLS_STEP_BUILD_CLIENT_HELLO;
        if (!netd_tls_build_client_hello_ex(ctx, host, hrr_cookie, hrr_cookie_len, client_hello, &client_hello_len, client_priv)) {
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        t->hs_step = NET_HTTP_TLS_STEP_SEND_CLIENT_HELLO;
        if (!netd_tls_write_record_plain_deadline(ctx, t, NETD_TLS_CT_HANDSHAKE, client_hello, client_hello_len, &deadline)) {
            t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        {
            uint8_t ccs = 1u;
            if (!netd_tls_write_record_plain_deadline(ctx, t, NETD_TLS_CT_CHANGE_CIPHER_SPEC, &ccs, 1u, &deadline)) {
                t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
                netd_tls_client_reset(t);
                goto cleanup;
            }
        }

        netd_tls_transcript_update(&transcript, client_hello, client_hello_len);

        memset(server_hello, 0, sizeof(server_hello));
        server_hello_len = 0;

        t->hs_step = NET_HTTP_TLS_STEP_RECV_SERVER_HELLO;
        if (!netd_tls_recv_handshake_message(ctx, t, &hs_rx, server_hello, (uint32_t)sizeof(server_hello), &server_hello_len, &deadline)) {
            t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        if (netd_tls_is_hello_retry_request(server_hello, server_hello_len)) {
            t->hs_status = NET_STATUS_UNSUPPORTED;
            netd_tls_set_internal_alert(t, NET_HTTP_TLS_INTERNAL_HRR);
            netd_tls_client_reset(t);
            goto cleanup;
        }
    }

    uint16_t suite = 0;
    uint8_t server_pub[32];
    memset(server_pub, 0, sizeof(server_pub));

    t->hs_step = NET_HTTP_TLS_STEP_PARSE_SERVER_HELLO;
    if (!netd_tls_parse_server_hello(server_hello, server_hello_len, &suite, server_pub)) {
        t->hs_status = NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        goto cleanup;
    }

    if (hrr_suite != 0 && suite != hrr_suite) {
        t->hs_status = NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        goto cleanup;
    }

    t->suite = suite;
    if (suite == NETD_TLS_SUITE_AES128GCM_SHA256) {
        t->key_len = 16u;
    } else if (suite == NETD_TLS_SUITE_CHACHA20POLY1305_SHA256) {
        t->key_len = 32u;
    } else {
        netd_tls_client_reset(t);
        goto cleanup;
    }

    if (!netd_tls_crypto_selftest_aead_for_suite(t, suite)) {
        t->hs_status = NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        goto cleanup;
    }

    netd_tls_transcript_update(&transcript, server_hello, server_hello_len);

    uint8_t shared[32];
    memset(shared, 0, sizeof(shared));
    netd_x25519(shared, client_priv, server_pub);

    uint8_t zero_salt[32];
    memset(zero_salt, 0, sizeof(zero_salt));

    uint8_t zero_ikm[32];
    memset(zero_ikm, 0, sizeof(zero_ikm));

    uint8_t early_secret[32];
    memset(early_secret, 0, sizeof(early_secret));
    netd_hkdf_sha256_extract(
        zero_salt,
        (uint32_t)sizeof(zero_salt),
        zero_ikm,
        (uint32_t)sizeof(zero_ikm),
        early_secret
    );

    uint8_t empty_hash[32];
    memset(empty_hash, 0, sizeof(empty_hash));
    netd_tls_sha256_empty(empty_hash);

    uint8_t derived_early[32];
    memset(derived_early, 0, sizeof(derived_early));
    netd_tls_derive_secret(early_secret, "derived", empty_hash, derived_early);

    uint8_t handshake_secret[32];
    memset(handshake_secret, 0, sizeof(handshake_secret));
    netd_hkdf_sha256_extract(derived_early, 32u, shared, 32u, handshake_secret);

    uint8_t th1[32];
    memset(th1, 0, sizeof(th1));
    netd_tls_transcript_hash(&transcript, th1);

    uint8_t c_hs_ts[32];
    uint8_t s_hs_ts[32];
    memset(c_hs_ts, 0, sizeof(c_hs_ts));
    memset(s_hs_ts, 0, sizeof(s_hs_ts));
    netd_tls_derive_secret(handshake_secret, "c hs traffic", th1, c_hs_ts);
    netd_tls_derive_secret(handshake_secret, "s hs traffic", th1, s_hs_ts);

    netd_tls_derive_traffic_key_iv(c_hs_ts, t->key_len, t->hs_key_w, t->hs_iv_w);
    netd_tls_derive_traffic_key_iv(s_hs_ts, t->key_len, t->hs_key_r, t->hs_iv_r);
    t->prot_read = NETD_TLS_PROT_HANDSHAKE;
    t->prot_write = NETD_TLS_PROT_HANDSHAKE;

    uint8_t s_finished_key[32];
    memset(s_finished_key, 0, sizeof(s_finished_key));
    netd_tls_derive_finished_key(s_hs_ts, s_finished_key);

    uint8_t c_finished_key[32];
    memset(c_finished_key, 0, sizeof(c_finished_key));
    netd_tls_derive_finished_key(c_hs_ts, c_finished_key);

    uint8_t hs_msg[2048];
    uint32_t hs_msg_len = 0;
    memset(hs_msg, 0, sizeof(hs_msg));

    for (;;) {
        t->hs_step = NET_HTTP_TLS_STEP_RECV_SERVER_FINISHED;
        uint8_t hh[4];
        if (!netd_tls_peek_handshake_header(ctx, t, &hs_rx, hh, &deadline)) {
            t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        uint8_t ht = hh[0];
        uint32_t body_len = netd_tls_load_be24(hh + 1);
        uint32_t total_len = 4u + body_len;

        if (ht == NETD_TLS_HS_ENCRYPTED_EXTENSIONS || ht == NETD_TLS_HS_CERTIFICATE || ht == NETD_TLS_HS_CERTIFICATE_VERIFY) {
            if (!netd_tls_discard_handshake_message(ctx, t, &hs_rx, &transcript, &deadline)) {
                t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
                netd_tls_client_reset(t);
                goto cleanup;
            }
            continue;
        }

        if (total_len > (uint32_t)sizeof(hs_msg)) {
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        if (!netd_tls_recv_handshake_message(ctx, t, &hs_rx, hs_msg, (uint32_t)sizeof(hs_msg), &hs_msg_len, &deadline)) {
            t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        if (hs_msg_len < 4u) {
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        if (ht != NETD_TLS_HS_FINISHED) {
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        if (hs_msg_len != 4u + 32u) {
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        uint8_t th[32];
        netd_tls_transcript_hash(&transcript, th);

        uint8_t expected[32];
        netd_hmac_sha256(s_finished_key, 32u, th, 32u, expected);

        if (memcmp(expected, hs_msg + 4u, 32u) != 0) {
            netd_tls_wipe(expected, (uint32_t)sizeof(expected));
            netd_tls_wipe(th, (uint32_t)sizeof(th));
            t->hs_status = NET_STATUS_ERROR;
            netd_tls_client_reset(t);
            goto cleanup;
        }

        netd_tls_wipe(expected, (uint32_t)sizeof(expected));
        netd_tls_wipe(th, (uint32_t)sizeof(th));

        netd_tls_transcript_update(&transcript, hs_msg, hs_msg_len);
        break;
    }

    uint8_t derived_hs[32];
    memset(derived_hs, 0, sizeof(derived_hs));
    netd_tls_derive_secret(handshake_secret, "derived", empty_hash, derived_hs);

    uint8_t master_secret[32];
    memset(master_secret, 0, sizeof(master_secret));
    netd_hkdf_sha256_extract(
        derived_hs,
        32u,
        zero_ikm,
        (uint32_t)sizeof(zero_ikm),
        master_secret
    );

    uint8_t th2[32];
    memset(th2, 0, sizeof(th2));
    netd_tls_transcript_hash(&transcript, th2);

    uint8_t c_app_ts[32];
    uint8_t s_app_ts[32];
    memset(c_app_ts, 0, sizeof(c_app_ts));
    memset(s_app_ts, 0, sizeof(s_app_ts));
    netd_tls_derive_secret(master_secret, "c ap traffic", th2, c_app_ts);
    netd_tls_derive_secret(master_secret, "s ap traffic", th2, s_app_ts);

    netd_tls_derive_traffic_key_iv(c_app_ts, t->key_len, t->app_key_w, t->app_iv_w);
    netd_tls_derive_traffic_key_iv(s_app_ts, t->key_len, t->app_key_r, t->app_iv_r);

    t->hs_step = NET_HTTP_TLS_STEP_SEND_CLIENT_FINISHED;
    if (!netd_tls_send_finished(ctx, t, &transcript, c_finished_key, &deadline)) {
        t->hs_status = (t->tcp->last_err != 0) ? t->tcp->last_err : NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        goto cleanup;
    }

    t->prot_read = NETD_TLS_PROT_APPLICATION;
    t->prot_write = NETD_TLS_PROT_APPLICATION;
    t->ready = 1;
    t->hs_step = NET_HTTP_TLS_STEP_DONE;
    t->hs_status = NET_STATUS_OK;

cleanup:
    netd_tls_wipe(client_priv, (uint32_t)sizeof(client_priv));
    netd_tls_wipe(shared, (uint32_t)sizeof(shared));
    netd_tls_wipe(early_secret, (uint32_t)sizeof(early_secret));
    netd_tls_wipe(derived_early, (uint32_t)sizeof(derived_early));
    netd_tls_wipe(handshake_secret, (uint32_t)sizeof(handshake_secret));
    netd_tls_wipe(th1, (uint32_t)sizeof(th1));
    netd_tls_wipe(th2, (uint32_t)sizeof(th2));
    netd_tls_wipe(c_hs_ts, (uint32_t)sizeof(c_hs_ts));
    netd_tls_wipe(s_hs_ts, (uint32_t)sizeof(s_hs_ts));
    netd_tls_wipe(c_finished_key, (uint32_t)sizeof(c_finished_key));
    netd_tls_wipe(s_finished_key, (uint32_t)sizeof(s_finished_key));
    netd_tls_wipe(c_app_ts, (uint32_t)sizeof(c_app_ts));
    netd_tls_wipe(s_app_ts, (uint32_t)sizeof(s_app_ts));
    netd_tls_wipe(master_secret, (uint32_t)sizeof(master_secret));
    netd_tls_wipe(derived_hs, (uint32_t)sizeof(derived_hs));
    netd_tls_wipe(empty_hash, (uint32_t)sizeof(empty_hash));
    netd_tls_wipe(server_pub, (uint32_t)sizeof(server_pub));
    netd_tls_wipe(server_hello, (uint32_t)sizeof(server_hello));
    netd_tls_wipe(hrr_cookie, (uint32_t)sizeof(hrr_cookie));
    netd_tls_wipe(hs_msg, (uint32_t)sizeof(hs_msg));
    netd_tls_wipe(&hs_rx, (uint32_t)sizeof(hs_rx));
    netd_tls_wipe(client_hello, (uint32_t)sizeof(client_hello));

    if (!t->active || !t->ready) {
        return 0;
    }

    return 1;
}

int netd_tls_connect(netd_ctx_t* ctx, netd_tls_client_t* t, const char* host, uint32_t ip, uint16_t port, uint32_t timeout_ms) {
    if (!ctx || !t || !host || port == 0) {
        return 0;
    }

    uint32_t st = NET_STATUS_ERROR;
    netd_tcp_conn_t* tcp = netd_tcp_open(ctx, ip, port, timeout_ms, &st);
    if (!tcp) {
        t->hs_step = 0;
        t->hs_status = (st != 0) ? st : NET_STATUS_ERROR;
        netd_tls_client_reset(t);
        return 0;
    }

    if (!netd_tls_handshake(ctx, t, tcp, host, timeout_ms)) {
        (void)netd_tcp_close(ctx, tcp, timeout_ms);
        netd_tls_client_reset(t);
        return 0;
    }

    return 1;
}

static uint32_t netd_tls_ring_count(const netd_tls_client_t* t) {
    return t->rx_w - t->rx_r;
}

static uint32_t netd_tls_ring_cap(void) {
    return (uint32_t)NETD_TLS_RX_CAP;
}

static void netd_tls_ring_push(netd_tls_client_t* t, const uint8_t* data, uint32_t len) {
    uint32_t cap = netd_tls_ring_cap();
    uint32_t count = netd_tls_ring_count(t);
    if (len > cap) {
        data += (len - cap);
        len = cap;
        t->rx_r = 0;
        t->rx_w = 0;
        count = 0;
    }

    if (count + len > cap) {
        uint32_t drop = (count + len) - cap;
        t->rx_r += drop;
    }

    uint32_t mask = cap - 1u;
    uint32_t wi = t->rx_w & mask;
    uint32_t first = cap - wi;
    if (first > len) {
        first = len;
    }
    memcpy(&t->rx_buf[wi], data, first);
    if (len > first) {
        memcpy(&t->rx_buf[0], data + first, len - first);
    }
    t->rx_w += len;
}

static uint32_t netd_tls_ring_pop(netd_tls_client_t* t, uint8_t* out, uint32_t cap) {
    uint32_t count = netd_tls_ring_count(t);
    if (count == 0) {
        return 0;
    }
    uint32_t take = count;
    if (take > cap) {
        take = cap;
    }

    uint32_t ring_cap = netd_tls_ring_cap();
    uint32_t mask = ring_cap - 1u;
    uint32_t ri = t->rx_r & mask;
    uint32_t first = ring_cap - ri;
    if (first > take) {
        first = take;
    }
    memcpy(out, &t->rx_buf[ri], first);
    if (take > first) {
        memcpy(out + first, &t->rx_buf[0], take - first);
    }
    t->rx_r += take;
    return take;
}

static int netd_tls_read_app_record_into_buffer(netd_ctx_t* ctx, netd_tls_client_t* t, uint32_t timeout_ms) {
    uint8_t rec_hdr[5];
    if (!netd_tls_read_record_header(ctx, t, rec_hdr, timeout_ms)) {
        return 0;
    }

    uint16_t rec_len = netd_tls_load_be16(rec_hdr + 3);
    if (rec_len == 0 || rec_len > 16384u + 256u) {
        return 0;
    }

    uint8_t rec_body[16384u + 256u];
    if (!netd_tls_read_record_body(ctx, t, rec_body, rec_len, timeout_ms)) {
        return 0;
    }

    if (rec_hdr[0] != NETD_TLS_CT_APPLICATION_DATA) {
        return 0;
    }

    uint8_t* payload = 0;
    uint32_t payload_len = 0;
    uint8_t inner_type = 0;

    if (!netd_tls_open_record(rec_body, rec_len, t->suite, t->app_key_r, t->app_iv_r, &t->app_seq_r, rec_hdr, &inner_type, &payload, &payload_len)) {
        return 0;
    }

    if (inner_type == NETD_TLS_CT_ALERT) {
        if (netd_tls_is_close_notify(payload, payload_len)) {
            t->closed = 1;
            return 1;
        }
        return 0;
    }

    if (inner_type != NETD_TLS_CT_APPLICATION_DATA) {
        return 0;
    }

    if (payload_len > 0) {
        netd_tls_ring_push(t, payload, payload_len);
    }
    return 1;
}

int netd_tls_send(netd_ctx_t* ctx, netd_tls_client_t* t, const void* data, uint32_t len, uint32_t timeout_ms) {
    if (!ctx || !t || !t->active || !t->ready || t->closed) {
        return 0;
    }
    if (!data && len != 0) {
        return 0;
    }

    uint32_t off = 0;
    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > 1200u) {
            chunk = 1200u;
        }

        uint8_t rec_hdr[5];
        uint8_t rec_body[1200u + 1u + 16u];
        uint32_t rec_body_len = 0;

        if (!netd_tls_seal_record(
                rec_hdr,
                rec_body,
                &rec_body_len,
                t->suite,
                t->app_key_w,
                t->app_iv_w,
                &t->app_seq_w,
                (const uint8_t*)data + off,
                chunk,
                NETD_TLS_CT_APPLICATION_DATA
            )) {
            return 0;
        }

        if (!netd_tls_tcp_write_all(ctx, t, rec_hdr, 5u, timeout_ms)) {
            return 0;
        }
        if (!netd_tls_tcp_write_all(ctx, t, rec_body, rec_body_len, timeout_ms)) {
            return 0;
        }

        off += chunk;
    }

    return 1;
}

int netd_tls_recv(netd_ctx_t* ctx, netd_tls_client_t* t, void* out, uint32_t cap, uint32_t timeout_ms, uint32_t* out_n) {
    if (out_n) {
        *out_n = 0;
    }

    if (!ctx || !t || !t->active || !t->ready || t->closed || (!out && cap != 0)) {
        return 0;
    }

    uint32_t have = netd_tls_ring_count(t);
    if (have == 0) {
        if (!netd_tls_read_app_record_into_buffer(ctx, t, timeout_ms)) {
            return 0;
        }
        have = netd_tls_ring_count(t);
    }

    uint32_t got = netd_tls_ring_pop(t, (uint8_t*)out, cap);
    if (out_n) {
        *out_n = got;
    }
    return 1;
}

int netd_tls_close(netd_ctx_t* ctx, netd_tls_client_t* t, uint32_t timeout_ms) {
    if (!ctx || !t || !t->active) {
        return 0;
    }

    netd_tcp_conn_t* tcp = t->tcp;

    t->closed = 1;
    t->ready = 0;
    t->active = 0;

    netd_tls_wipe(t, (uint32_t)sizeof(*t));
    return tcp ? netd_tcp_close(ctx, tcp, timeout_ms) : 0;
}

