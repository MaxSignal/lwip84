/* tls84.c - a minimal TLS 1.3 client for the TI-84 Plus as an lwIP altcp
 * layer.  It replaces lwIP-CE's TLS stack (eZ80 assembly) behind the same
 * entry points, so lwip_socket_create(LWIP_SOCKET_TLS / _WSS) works.
 *
 * Only what is needed to talk to a server is done:
 *   - one cipher suite, TLS_CHACHA20_POLY1305_SHA256, and one group, X25519
 *   - no certificate, CertificateVerify or Finished checks: the server's
 *     encrypted handshake is only hashed into the transcript.  The
 *     connection is encrypted but NOT authenticated (no protection against
 *     an active man in the middle).
 *   - incoming records are decrypted as a stream and their Poly1305 tags are
 *     skipped, so 16 KiB records need no 16 KiB buffer; outgoing records
 *     are sealed properly (the server checks them)
 *   - no HelloRetryRequest, resumption, early data or KeyUpdate
 * The X25519 shared secret takes seconds and is computed in slices from
 * tls84_poll() so the rest of the system keeps running. */
#include <stdlib.h>
#include <string.h>
#include "lwip/opt.h"
#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/priv/altcp_priv.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "tls/includes/tls.h"
#include "tls/includes/random.h"
#include "tls/includes/bytes.h"
#include "apps/altcp_tls/altcp_tls_ce.h"
#include "kernel.h"
#include "sha256.h"
#include "chacha.h"
#include "x25519.h"
#include "rng.h"
#include "kx84.h"
#include "tls84.h"

#define ACC_MAX 1024            /* records up to this size are buffered whole */
#define CHUNK 128               /* ciphertext processed per step */
#define OVERHEAD 22             /* record header + inner type + tag */

enum { P_WAIT_KEY, P_WAIT_SH, P_KEX, P_HS, P_APP, P_CLOSED, P_FAIL };
enum { R_HDR, R_SKIP, R_PLAIN, R_BODY, R_TAG };

struct tls84 {
    struct tls84 *next;
    struct altcp_pcb *conn;
    uint8_t phase, rstate, buffered, stream_type, s_switch, eof, tcp_up;
    uint8_t hdr[5], hdr_n, tag_left;
    uint16_t body_left;
    chacha_ctx dec;
    uint8_t *acc;
    uint16_t acc_n, zeros;
    uint8_t pend, pend_ok;
    uint8_t hm_hdr[4], hm_hdr_n, cert_req;
    uint32_t hm_left;
    sha256_ctx th;
    uint8_t secret[32];         /* handshake secret, then master secret */
    uint8_t c_hs[32];           /* transcript hash (CH..SH), then client hs traffic secret */
    uint8_t c_key[32], c_iv[12], s_key[32], s_iv[12];
    uint32_t c_seq, s_seq;
    x25519_ctx *kex;
    uint32_t t_hello;                   /* for the debug log */
    struct pbuf *rx, *app;
    uint16_t to_ack, overhead;
    char host[DNS_MAX_NAME_LENGTH + 1];
};

struct tls_context tls_ctx;
const char *tls84_last_error = "";
static struct tls84 *tls_list;

static const uint8_t derived_early[32] = {
    0x6f, 0x26, 0x15, 0xa1, 0x08, 0xc7, 0x02, 0xc5, 0x67, 0x8f, 0x54, 0xfc, 0x9d, 0xba, 0xb6, 0x97,
    0x16, 0xc0, 0x76, 0x18, 0x9c, 0x48, 0x25, 0x0c, 0xeb, 0xea, 0xc3, 0x57, 0x6c, 0x36, 0x11, 0xba};
static const uint8_t empty_hash[32] = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
static const uint8_t zeros32[32];

/* ------------------------------------------------------------------ */

static void fail(struct tls84 *st, const char *why)
{
    if (st->phase != P_FAIL)
        dbg_printf("tls: %s\n", why);
    tls84_last_error = why;
    st->phase = P_FAIL;
}

static void mknonce(uint8_t n[12], const uint8_t iv[12], uint32_t seq)
{
    memcpy(n, iv, 12);
    n[8] ^= (uint8_t)(seq >> 24);
    n[9] ^= (uint8_t)(seq >> 16);
    n[10] ^= (uint8_t)(seq >> 8);
    n[11] ^= (uint8_t)seq;
}

static void traffic_keys(const uint8_t secret[32], uint8_t key[32], uint8_t iv[12])
{
    hkdf_expand_label(secret, "key", NULL, 0, key, 32);
    hkdf_expand_label(secret, "iv", NULL, 0, iv, 12);
}

static err_t send_raw(struct tls84 *st, const void *data, uint16_t len)
{
    struct altcp_pcb *inner = st->conn->inner_conn;
    err_t e;
    if (!inner)
        return ERR_CONN;
    e = altcp_write(inner, data, len, TCP_WRITE_FLAG_COPY);
    if (e == ERR_OK)
        st->overhead += len;
    return e;
}

static void poly_pad(poly1305_ctx *p, uint16_t n)
{
    n &= 15;
    if (n)
        poly1305_update(p, zeros32, 16 - n);
}

/* seal one record (RFC 8446 5.2, RFC 8439 2.8) and pass it to TCP;
 * hidden: the bytes are not reported to the application as sent */
static err_t send_record(struct tls84 *st, uint8_t type, const void *data, uint16_t len,
                         uint8_t hidden)
{
    chacha_ctx cc;
    poly1305_ctx pc;
    uint8_t nonce[12], *rec;
    uint16_t ct = len + 1;
    uint8_t lens[16];
    err_t e;
    rec = malloc(ct + OVERHEAD - 1);
    if (!rec)
        return ERR_MEM;
    rec[0] = 23;
    rec[1] = 3;
    rec[2] = 3;
    rec[3] = (uint8_t)((ct + 16) >> 8);
    rec[4] = (uint8_t)(ct + 16);
    memcpy(rec + 5, data, len);
    rec[5 + len] = type;
    mknonce(nonce, st->c_iv, st->c_seq);
    chacha_init(&cc, st->c_key, nonce, 12, 0);
    chacha_block(cc.s, cc.ks);          /* block 0: the Poly1305 key */
    poly1305_init(&pc, cc.ks);
    cc.s[12] = 1;
    chacha_xor(&cc, rec + 5, ct);
    poly1305_update(&pc, rec, 5);
    poly_pad(&pc, 5);
    poly1305_update(&pc, rec + 5, ct);
    poly_pad(&pc, ct);
    memset(lens, 0, 16);
    lens[0] = 5;
    lens[8] = (uint8_t)ct;
    lens[9] = (uint8_t)(ct >> 8);
    poly1305_update(&pc, lens, 16);
    poly1305_final(&pc, rec + 5 + ct);
    e = altcp_write(st->conn->inner_conn, rec, ct + OVERHEAD - 1, TCP_WRITE_FLAG_COPY);
    free(rec);
    if (e == ERR_OK) {
        st->c_seq++;
        st->overhead += hidden ? ct + OVERHEAD - 1 : OVERHEAD;
    }
    return e;
}

static uint8_t *put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return p + 2;
}

static uint8_t is_ip_literal(const char *h)
{
    for (; *h; h++)
        if ((*h < '0' || *h > '9') && *h != '.')
            return 0;
    return 1;
}

static void send_client_hello(struct tls84 *st)
{
    static const uint8_t fixed_ext_bk[] = {
        0x00, 0x0a, 0x00, 0x04, 0x00, 0x02, 0x00, 0x1d,             /* groups: x25519 */
        0x00, 0x0d, 0x00, 0x14, 0x00, 0x12,                         /* signature algorithms */
        0x04, 0x03, 0x08, 0x04, 0x04, 0x01, 0x05, 0x03, 0x08, 0x05,
        0x05, 0x01, 0x08, 0x06, 0x06, 0x01, 0x08, 0x07,
        0x00, 0x2b, 0x00, 0x03, 0x02, 0x03, 0x04,                   /* TLS 1.3 */
        0x00, 0x33, 0x00, 0x26, 0x00, 0x24, 0x00, 0x1d, 0x00, 0x20  /* key share */
    };
    uint8_t hl = is_ip_literal(st->host) ? 0 : (uint8_t)strlen(st->host);
    uint16_t ext = sizeof(fixed_ext_bk) + 32 + (hl ? 9 + hl : 0);
    uint16_t body = 2 + 32 + 33 + 4 + 2 + 2 + ext;
    uint8_t *m = malloc(9 + body), *p;
    if (!m) {
        fail(st, "no memory");
        return;
    }
    p = m;
    *p++ = 22; *p++ = 3; *p++ = 1;
    p = put16(p, body + 4);
    *p++ = 1; *p++ = 0;
    p = put16(p, body);
    *p++ = 3; *p++ = 3;
    rng_bytes(p, 32);                   /* random */
    p += 32;
    *p++ = 32;                          /* legacy session id (middlebox compatibility) */
    rng_bytes(p, 32);
    p += 32;
    p = put16(p, 2);
    p = put16(p, 0x1303);               /* TLS_CHACHA20_POLY1305_SHA256 */
    *p++ = 1; *p++ = 0;                 /* null compression */
    p = put16(p, ext);
    if (hl) {
        p = put16(p, 0);                /* server_name */
        p = put16(p, hl + 5);
        p = put16(p, hl + 3);
        *p++ = 0;
        p = put16(p, hl);
        memcpy(p, st->host, hl);
        p += hl;
    }
    memcpy(p, fixed_ext_bk, sizeof(fixed_ext_bk));
    p += sizeof(fixed_ext_bk);
    memcpy(p, kx_pub(), 32);
    sha256_init(&st->th);
    sha256_update(&st->th, m + 5, body + 4);
    if (send_raw(st, m, 9 + body) != ERR_OK)
        fail(st, "cannot send ClientHello");
    else
        altcp_output(st->conn->inner_conn);
    free(m);
    st->phase = P_WAIT_SH;
    st->t_hello = k_now_ms();
}

/* ------------------------------------------------------------------ */
/* receive side                                                        */

static void app_input(struct tls84 *st, const uint8_t *p, uint16_t n)
{
    struct pbuf *q;
    if (!n)
        return;
    q = pbuf_alloc(PBUF_RAW, n, PBUF_RAM);
    if (!q) {
        fail(st, "no memory");
        return;
    }
    memcpy(q->payload, p, n);
    if (st->app)
        pbuf_cat(st->app, q);
    else
        st->app = q;
}

static void server_finished(struct tls84 *st)
{
    sha256_ctx t;
    uint8_t th[32], sec[32], fin[36];
    static const uint8_t ccs[6] = {20, 3, 3, 0, 1, 1};
    memcpy(&t, &st->th, sizeof(t));
    sha256_final(&t, th);
    hkdf_expand_label(st->secret, "s ap traffic", th, 32, sec, 32);
    traffic_keys(sec, st->s_key, st->s_iv);
    st->s_switch = 1;                   /* sequence 0 from the next record on */
    send_raw(st, ccs, sizeof(ccs));
    if (st->cert_req) {                 /* empty client Certificate */
        static const uint8_t cert[8] = {11, 0, 0, 4, 0, 0, 0, 0};
        sha256_update(&st->th, cert, 8);
        sha256_final(&st->th, fin + 4);
        send_record(st, 22, cert, 8, 1);
    } else {
        memcpy(fin + 4, th, 32);
    }
    hkdf_expand_label(st->c_hs, "finished", NULL, 0, sec, 32);
    hmac_sha256(sec, 32, fin + 4, 32, fin + 4);
    fin[0] = 20; fin[1] = 0; fin[2] = 0; fin[3] = 32;
    if (send_record(st, 22, fin, 36, 1) != ERR_OK) {
        fail(st, "cannot send Finished");
        return;
    }
    altcp_output(st->conn->inner_conn);
    hkdf_expand_label(st->secret, "c ap traffic", th, 32, sec, 32);
    traffic_keys(sec, st->c_key, st->c_iv);
    st->c_seq = 0;
    tls_secure_memzero(st->secret, 32);
    tls_secure_memzero(st->c_hs, 32);
    tls_secure_memzero(sec, 32);
    st->phase = P_APP;
    dbg_printf("tls: handshake done after %lu ms\n", k_now_ms() - st->t_hello);
    if (st->conn->connected)
        st->conn->connected(st->conn->arg, st->conn, ERR_OK);
}

/* encrypted handshake messages: hashed, framed, otherwise ignored */
static void hs_input(struct tls84 *st, const uint8_t *p, uint16_t n)
{
    while (n && st->phase < P_CLOSED) {
        uint16_t k;
        if (st->hm_hdr_n < 4) {
            st->hm_hdr[st->hm_hdr_n++] = *p;
            if (st->phase == P_HS)
                sha256_update(&st->th, p, 1);
            p++;
            n--;
            if (st->hm_hdr_n < 4)
                continue;
            st->hm_left = (uint32_t)st->hm_hdr[1] << 16 | (uint16_t)st->hm_hdr[2] << 8 |
                          st->hm_hdr[3];
            if (st->hm_hdr[0] == 13)
                st->cert_req = 1;
        } else {
            k = n < st->hm_left ? n : (uint16_t)st->hm_left;
            if (st->phase == P_HS)
                sha256_update(&st->th, p, k);
            p += k;
            n -= k;
            st->hm_left -= k;
        }
        if (st->hm_left == 0) {
            st->hm_hdr_n = 0;
            if (st->phase == P_HS && st->hm_hdr[0] == 20)
                server_finished(st);
        }
    }
}

static void dispatch(struct tls84 *st, uint8_t type, const uint8_t *p, uint16_t n)
{
    if (type == 22)
        hs_input(st, p, n);
    else if (type == 23 && st->phase == P_APP)
        app_input(st, p, n);
    else if (type == 21 && n >= 2 && p[1] == 0)
        st->eof = 1;                    /* close_notify */
    else if (type == 21)
        fail(st, "alert from server");
    else {
        dbg_printf("tls: inner type %u n %u phase %u\n", type, n, st->phase);
        fail(st, "unexpected record");
    }
}

/* streamed records: hold back the last non-zero byte (the content type)
 * and any zero padding after it until the record ends */
static void stream_bytes(struct tls84 *st, const uint8_t *p, uint16_t n)
{
    int16_t j = (int16_t)n - 1;
    while (j >= 0 && !p[j])
        j--;
    if (j < 0) {
        st->zeros += n;
        return;
    }
    if (st->pend_ok)
        dispatch(st, st->stream_type, &st->pend, 1);
    while (st->zeros) {
        uint8_t k = st->zeros > 32 ? 32 : (uint8_t)st->zeros;
        dispatch(st, st->stream_type, zeros32, k);
        st->zeros -= k;
    }
    if (j > 0)
        dispatch(st, st->stream_type, p, (uint16_t)j);
    st->pend = p[j];
    st->pend_ok = 1;
    st->zeros = n - 1 - (uint16_t)j;
}

static void server_hello(struct tls84 *st)
{
    static const uint8_t hrr_bk[8] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11};
    const uint8_t *p = st->acc, *key = NULL;
    uint16_t n = st->acc_n, pos, end;
    uint32_t blen;
    sha256_ctx t;
    if (n < 4 + 38 || p[0] != 2) {
        fail(st, "bad ServerHello");
        return;
    }
    blen = (uint32_t)p[1] << 16 | (uint16_t)p[2] << 8 | p[3];
    if (blen + 4 > n) {
        fail(st, "fragmented ServerHello");
        return;
    }
    if (!memcmp(p + 6, hrr_bk, 8)) {
        fail(st, "HelloRetryRequest");
        return;
    }
    pos = 4 + 2 + 32;
    pos += 1 + p[pos];                  /* session id */
    end = (uint16_t)blen + 4;
    if (pos + 5 > end || p[pos] != 0x13 || p[pos + 1] != 0x03) {
        fail(st, "server does not do ChaCha20");
        return;
    }
    pos += 3 + 2;                       /* suite, compression, extensions length */
    while (pos + 4 <= end) {
        uint16_t type = (uint16_t)p[pos] << 8 | p[pos + 1];
        uint16_t len = (uint16_t)p[pos + 2] << 8 | p[pos + 3];
        pos += 4;
        if (pos + len > end)
            break;
        if (type == 0x0033 && len == 36 && p[pos + 1] == 0x1d)
            key = p + pos + 4;
        pos += len;
    }
    if (!key) {
        fail(st, "no X25519 key share");
        return;
    }
    sha256_update(&st->th, p, end);
    memcpy(&t, &st->th, sizeof(t));
    sha256_final(&t, st->c_hs);
    st->kex = malloc(sizeof(x25519_ctx));
    if (!st->kex) {
        fail(st, "no memory");
        return;
    }
    x25519_start(st->kex, kx_priv(), key);
    st->phase = P_KEX;
}

static void kex_done(struct tls84 *st)
{
    dbg_printf("tls: +%lu kex\n", k_now_ms() - st->t_hello);
    uint8_t shared[32], hs[32], s_hs[32];
    x25519_result(st->kex, shared);
    free(st->kex);
    st->kex = NULL;
    hkdf_extract(derived_early, shared, hs);
    hkdf_expand_label(hs, "s hs traffic", st->c_hs, 32, s_hs, 32);
    hkdf_expand_label(hs, "c hs traffic", st->c_hs, 32, st->c_hs, 32);
    traffic_keys(s_hs, st->s_key, st->s_iv);
    traffic_keys(st->c_hs, st->c_key, st->c_iv);
    st->s_seq = st->c_seq = 0;
    hkdf_expand_label(hs, "derived", empty_hash, 32, s_hs, 32);
    hkdf_extract(s_hs, zeros32, st->secret);
    tls_secure_memzero(shared, 32);
    tls_secure_memzero(hs, 32);
    st->phase = P_HS;
}

static void free_acc(struct tls84 *st)
{
    free(st->acc);
    st->acc = NULL;
}

static uint8_t want_acc(struct tls84 *st)
{
    if (!st->acc)
        st->acc = malloc(ACC_MAX);
    st->acc_n = 0;
    if (!st->acc)
        fail(st, "no memory");
    return st->acc != NULL;
}

static void rec_start(struct tls84 *st)
{
    uint8_t type = st->hdr[0];
    uint16_t len = (uint16_t)st->hdr[3] << 8 | st->hdr[4];
    dbg_printf("tls: +%lu rec %u len %u ph %u\n", k_now_ms() - st->t_hello, type, len, st->phase);
    st->body_left = len;
    if (len > 16384 + 256) {
        fail(st, "bad record");
    } else if (type == 20) {
        st->rstate = R_SKIP;            /* ChangeCipherSpec */
    } else if (st->phase == P_WAIT_SH) {
        if (type == 22 && len <= ACC_MAX && want_acc(st))
            st->rstate = R_PLAIN;
        else if (type == 21)
            fail(st, "handshake refused (alert)");
        else {
            dbg_printf("tls: record type %u len %u before ServerHello\n", type, len);
            fail(st, "unexpected record");
        }
    } else if (type == 23 && len > 16) {
        uint8_t nonce[12];
        mknonce(nonce, st->s_iv, st->s_seq);
        chacha_init(&st->dec, st->s_key, nonce, 12, 1);
        st->body_left = len - 16;
        st->buffered = st->body_left <= ACC_MAX;
        if (st->buffered && !want_acc(st))
            return;
        st->pend_ok = 0;
        st->zeros = 0;
        st->stream_type = st->phase == P_HS ? 22 : 23;
        st->rstate = R_BODY;
    } else {
        dbg_printf("tls: record type %u len %u phase %u\n", type, len, st->phase);
        fail(st, "unexpected record");
    }
}

static void rec_end(struct tls84 *st)
{
    dbg_printf("tls: +%lu rec end\n", k_now_ms() - st->t_hello);
    if (st->buffered) {
        uint16_t n = st->acc_n;
        while (n && !st->acc[n - 1])
            n--;
        if (n)
            dispatch(st, st->acc[n - 1], st->acc, n - 1);
        else
            fail(st, "empty record");
    } else if (!st->pend_ok || st->pend != st->stream_type) {
        fail(st, "unexpected record type");
    }
    free_acc(st);
    if (st->s_switch) {
        st->s_switch = 0;
        st->s_seq = 0;
    } else {
        st->s_seq++;
    }
}

static uint16_t rec_feed(struct tls84 *st, uint8_t *b, uint16_t n)
{
    uint16_t used = 0, k;
    while (used < n && st->phase < P_CLOSED && st->phase != P_KEX) {
        uint8_t *p = b + used;
        uint16_t avail = n - used;
        k = avail < st->body_left ? avail : st->body_left;
        switch (st->rstate) {
        case R_HDR:
            st->hdr[st->hdr_n++] = *p;
            used++;
            if (st->hdr_n == 5) {
                st->hdr_n = 0;
                rec_start(st);
            }
            continue;
        case R_SKIP:
            break;
        case R_PLAIN:
            memcpy(st->acc + st->acc_n, p, k);
            st->acc_n += k;
            break;
        case R_BODY:
            chacha_xor(&st->dec, p, k);
            if (st->buffered) {
                memcpy(st->acc + st->acc_n, p, k);
                st->acc_n += k;
            } else {
                stream_bytes(st, p, k);
            }
            break;
        case R_TAG:
            k = avail < st->tag_left ? avail : st->tag_left;
            used += k;
            st->tag_left -= (uint8_t)k;
            if (!st->tag_left) {
                st->rstate = R_HDR;
                rec_end(st);
            }
            continue;
        }
        used += k;
        st->body_left -= k;
        if (st->body_left)
            continue;
        if (st->rstate == R_BODY) {
            st->rstate = R_TAG;
            st->tag_left = 16;
        } else if (st->rstate == R_PLAIN) {
            st->rstate = R_HDR;
            server_hello(st);
            free_acc(st);
        } else {
            st->rstate = R_HDR;
        }
    }
    return used;
}

/* returns false if the connection was aborted (conn and st are gone) */
static bool tls_process(struct tls84 *st)
{
    struct altcp_pcb *conn = st->conn;
    uint8_t buf[CHUNK];
    while (st->rx && st->phase < P_CLOSED && st->phase != P_KEX &&
           (!st->app || st->app->tot_len < 2048)) {
        struct pbuf *q = st->rx;
        uint16_t n = q->len > CHUNK ? CHUNK : q->len, used;
        if (!n) {
            st->rx = q->next;
            if (st->rx)
                pbuf_ref(st->rx);
            q->next = NULL;
            pbuf_free(q);
            continue;
        }
        memcpy(buf, q->payload, n);
        used = rec_feed(st, buf, n);
        st->rx = pbuf_free_header(q, used);
        st->to_ack += used;
        if (used < n)
            break;
    }
    if (st->phase == P_FAIL) {
        if (conn->inner_conn)
            altcp_abort(conn->inner_conn);   /* calls lower_err: frees conn */
        return false;
    }
    if (st->to_ack && conn->inner_conn) {
        altcp_recved(conn->inner_conn, st->to_ack);
        st->to_ack = 0;
    }
    if (st->app && conn->recv) {
        if (conn->recv(conn->arg, conn, st->app, ERR_OK) == ERR_OK)
            st->app = NULL;
    }
    if (!st->app && st->eof == 1 && !st->rx) {
        st->eof = 2;
        if (conn->recv)
            conn->recv(conn->arg, conn, NULL, ERR_OK);
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* lower (TCP) callbacks                                               */

static err_t lower_connected(void *arg, struct altcp_pcb *inner, err_t err)
{
    struct altcp_pcb *conn = arg;
    struct tls84 *st = conn->state;
    (void)inner;
    if (err != ERR_OK) {
        if (conn->connected)
            return conn->connected(conn->arg, conn, err);
        return ERR_OK;
    }
    st->tcp_up = 1;
    if (kx_ready())
        send_client_hello(st);
    else
        kx_start();                     /* tls84_poll sends the hello when ready */
    return ERR_OK;
}

static err_t lower_recv(void *arg, struct altcp_pcb *inner, struct pbuf *p, err_t err)
{
    struct altcp_pcb *conn = arg;
    struct tls84 *st = conn->state;
    (void)inner;
    if (err != ERR_OK || !st) {
        if (p)
            pbuf_free(p);
        return ERR_OK;
    }
    if (!p) {
        if (st->phase != P_APP)
            fail(st, "server closed the connection");
        else if (!st->eof)
            st->eof = 1;
    } else if (st->rx) {
        pbuf_cat(st->rx, p);
    } else {
        st->rx = p;
    }
    return tls_process(st) ? ERR_OK : ERR_ABRT;
}

static err_t lower_sent(void *arg, struct altcp_pcb *inner, u16_t len)
{
    struct altcp_pcb *conn = arg;
    struct tls84 *st = conn->state;
    uint16_t d;
    (void)inner;
    if (!st)
        return ERR_OK;
    d = len < st->overhead ? len : st->overhead;
    st->overhead -= d;
    len -= d;
    if (len && conn->sent)
        return conn->sent(conn->arg, conn, len);
    return ERR_OK;
}

static err_t lower_poll(void *arg, struct altcp_pcb *inner)
{
    struct altcp_pcb *conn = arg;
    (void)inner;
    if (conn->poll)
        return conn->poll(conn->arg, conn);
    return ERR_OK;
}

static void lower_err(void *arg, err_t err)
{
    struct altcp_pcb *conn = arg;
    dbg_printf("tls: tcp error %d\n", err);
    if (!conn)
        return;
    conn->inner_conn = NULL;            /* already freed by TCP */
    if (conn->err)
        conn->err(conn->arg, err);
    altcp_free(conn);
}

static void setup_callbacks(struct altcp_pcb *conn, struct altcp_pcb *inner)
{
    altcp_arg(inner, conn);
    altcp_recv(inner, lower_recv);
    altcp_sent(inner, lower_sent);
    altcp_err(inner, lower_err);
}

static void remove_callbacks(struct altcp_pcb *inner)
{
    altcp_arg(inner, NULL);
    altcp_recv(inner, NULL);
    altcp_sent(inner, NULL);
    altcp_err(inner, NULL);
    altcp_poll(inner, NULL, inner->pollinterval);
}

/* ------------------------------------------------------------------ */
/* altcp functions                                                     */

static void f_set_poll(struct altcp_pcb *conn, u8_t interval)
{
    if (conn->inner_conn)
        altcp_poll(conn->inner_conn, lower_poll, interval);
}

static void f_recved(struct altcp_pcb *conn, u16_t len)
{
    (void)conn;                         /* ciphertext is acknowledged as it is decrypted */
    (void)len;
}

static err_t f_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr, u16_t port,
                       altcp_connected_fn connected)
{
    if (!conn->inner_conn)
        return ERR_VAL;
    conn->connected = connected;
    return altcp_connect(conn->inner_conn, ipaddr, port, lower_connected);
}

static struct altcp_pcb *f_listen(struct altcp_pcb *conn, u8_t backlog, err_t *err)
{
    (void)conn;
    (void)backlog;
    if (err)
        *err = ERR_VAL;
    return NULL;
}

static void f_abort(struct altcp_pcb *conn)
{
    if (conn->inner_conn)
        altcp_abort(conn->inner_conn);
}

static void send_close_notify(struct altcp_pcb *conn)
{
    static const uint8_t alert[2] = {1, 0};
    struct tls84 *st = conn->state;
    if (st && st->phase == P_APP && conn->inner_conn) {
        send_record(st, 21, alert, 2, 1);
        st->phase = P_CLOSED;
    }
}

static err_t f_close(struct altcp_pcb *conn)
{
    struct altcp_pcb *inner = conn->inner_conn;
    if (inner) {
        err_t e;
        send_close_notify(conn);
        remove_callbacks(inner);
        e = altcp_close(inner);
        if (e != ERR_OK) {
            setup_callbacks(conn, inner);
            altcp_poll(inner, lower_poll, inner->pollinterval);
            return e;
        }
        conn->inner_conn = NULL;
    }
    altcp_free(conn);
    return ERR_OK;
}

static err_t f_shutdown(struct altcp_pcb *conn, int shut_rx, int shut_tx)
{
    if (shut_rx && shut_tx)
        return f_close(conn);
    if (shut_tx)
        send_close_notify(conn);
    if (conn->inner_conn)
        return altcp_shutdown(conn->inner_conn, shut_rx, shut_tx);
    return ERR_VAL;
}

static err_t f_write(struct altcp_pcb *conn, const void *data, u16_t len, u8_t flags)
{
    struct tls84 *st = conn->state;
    (void)flags;
    if (!st || st->phase != P_APP || !conn->inner_conn)
        return ERR_CONN;
    if (altcp_sndbuf(conn->inner_conn) < len + OVERHEAD ||
        altcp_sndqueuelen(conn->inner_conn) + 2 > TCP_SND_QUEUELEN)
        return ERR_MEM;
    return send_record(st, 23, data, len, 0);
}

static u16_t f_mss(struct altcp_pcb *conn)
{
    u16_t m = conn->inner_conn ? altcp_mss(conn->inner_conn) : 0;
    return m > OVERHEAD ? m - OVERHEAD : 0;
}

static u16_t f_sndbuf(struct altcp_pcb *conn)
{
    struct tls84 *st = conn->state;
    u16_t s;
    if (!st || st->phase != P_APP || !conn->inner_conn)
        return 0;
    s = altcp_sndbuf(conn->inner_conn);
    return s > OVERHEAD ? s - OVERHEAD : 0;
}

static void f_dealloc(struct altcp_pcb *conn)
{
    struct tls84 *st = conn->state, **pp;
    if (!st)
        return;
    for (pp = &tls_list; *pp; pp = &(*pp)->next)
        if (*pp == st) {
            *pp = st->next;
            break;
        }
    if (st->rx)
        pbuf_free(st->rx);
    if (st->app)
        pbuf_free(st->app);
    free(st->acc);
    free(st->kex);
    tls_secure_memzero(st, sizeof(*st));
    free(st);
    conn->state = NULL;
}

static const struct altcp_functions tls84_functions = {
    f_set_poll, f_recved, altcp_default_bind, f_connect, f_listen, f_abort, f_close,
    f_shutdown, f_write, altcp_default_output, f_mss, f_sndbuf, altcp_default_sndqueuelen,
    altcp_default_nagle_disable, altcp_default_nagle_enable, altcp_default_nagle_disabled,
    altcp_default_setprio, f_dealloc, altcp_default_get_tcp_addrinfo, altcp_default_get_ip,
    altcp_default_get_port
#if LWIP_TCP_KEEPALIVE
    , altcp_default_keepalive_disable, altcp_default_keepalive_enable
#endif
#ifdef LWIP_DEBUG
    , altcp_default_dbg_get_tcp_state
#endif
};

/* ------------------------------------------------------------------ */
/* lwIP-CE entry points                                                */

bool tls_init(void)
{
    tls_ctx.initialized = true;
    return true;
}

void tls_cleanup(void)
{
}

struct altcp_tls_ce_config *altcp_tls_ce_create_config_client_ecdhe(const char *hostname)
{
    struct altcp_tls_ce_config *c = calloc(1, sizeof(*c));
    if (c)
        c->hostname = hostname;
    return c;
}

void altcp_tls_ce_free_config(struct altcp_tls_ce_config *conf)
{
    free(conf);
}

struct altcp_pcb *altcp_tls_ce_new(struct altcp_tls_ce_config *config, u8_t ip_type)
{
    struct altcp_pcb *inner, *conn;
    struct tls84 *st;
    if (!config || !config->hostname)
        return NULL;
    inner = altcp_tcp_new_ip_type(ip_type);
    if (!inner)
        return NULL;
    conn = altcp_alloc();
    st = calloc(1, sizeof(*st));
    if (!conn || !st) {
        free(st);
        if (conn)
            altcp_free(conn);
        altcp_close(inner);
        return NULL;
    }
    strncpy(st->host, config->hostname, sizeof(st->host) - 1);
    st->conn = conn;
    st->phase = P_WAIT_KEY;
    st->next = tls_list;
    tls_list = st;
    conn->inner_conn = inner;
    conn->fns = &tls84_functions;
    conn->state = st;
    setup_callbacks(conn, inner);
    altcp_poll(inner, lower_poll, 4);
    kx_start();
    tls84_last_error = "";
    return conn;
}

/* main loop: background work of the TLS connections */
void tls84_poll(void)
{
    struct tls84 *st;
    for (st = tls_list; st; st = st->next) {
        if (!st->conn->inner_conn)
            continue;
        if (st->phase == P_WAIT_KEY && st->tcp_up) {
            if (kx_run(100))
                send_client_hello(st);
        } else if (st->phase == P_KEX) {
            if (kx_shared_step(st->kex, 100))
                kex_done(st);
        } else if (!st->rx && !st->app && st->eof != 1) {
            continue;
        }
        if (!tls_process(st))
            return;                     /* list changed */
    }
}

/* Used by WebSocket masking keys and the TLS randoms */
void *tls_random_bytes(void *buffer, size_t len)
{
    rng_bytes(buffer, len);
    return buffer;
}

/* what the newest TLS connection is doing, for status lines */
const char *tls84_status(char *buf, uint8_t n)
{
    struct tls84 *st = tls_list;
    if (!st)
        return NULL;
    if (st->phase == P_WAIT_KEY)
        snprintf(buf, n, "TLS: key pair %u%%", kx_progress());
    else if (st->phase == P_KEX && st->kex)
        snprintf(buf, n, "TLS: key exchange %u%%", x25519_progress(st->kex));
    else if (st->phase < P_APP)
        snprintf(buf, n, "TLS: handshake");
    else
        return NULL;
    return buf;
}
