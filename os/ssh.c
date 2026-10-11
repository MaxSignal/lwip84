/* ssh.c - a minimal SSH-2 client: curve25519-sha256 key exchange,
 * chacha20-poly1305@openssh.com, password (or keyboard-interactive)
 * authentication, one interactive shell on a 24x9 pty.
 *
 * Like the TLS client it skips what only protects against an active
 * attacker: the server's host key and signature are not checked, and the
 * Poly1305 tags of received packets are not verified.
 *
 * Keys:  ENTER  return        DEL  backspace      CLEAR  Ctrl-C
 *        VARS   tab          MODE  Esc            XT0n  Ctrl- next key
 *        arrows  cursor keys  2nd+UP/DOWN  scrollback
 *        2nd+MODE  disconnect and quit
 */
#include <stdlib.h>
#include <string.h>
#include "kernel.h"
#include "input.h"
#include "ui.h"
#include "term.h"
#include "lwIP.h"
#include "sha256.h"
#include "chacha.h"
#include "x25519.h"
#include "rng.h"
#include "kx84.h"
#include "lwip/arch.h"
uint32_t sys_rand32(void);

#define RXBUF 2560u
#define PBUF 512u

enum { F_HOST, F_USER, F_PASS, F_NONE };
enum {
    S_IDLE, S_CONNECTING, S_VERSION, S_KEXINIT, S_ECDH, S_COMPUTE, S_NEWKEYS, S_SERVICE,
    S_AUTH, S_CHANNEL, S_SHELL, S_OPEN, S_CLOSED
};

static const char version[] = "SSH-2.0-lwIP84_1.0";

static struct lwip_socket *sock;
static uint8_t st, field, ctrl_next, kbd_int, got_newkeys, enc_out, enc_in;
static uint16_t port, back;

static uint8_t *rx, *pb;                /* receive buffer, payload builder */
static uint16_t rx_n, pb_n, discard;
static uint32_t seq_out, seq_in;
static sha256_ctx *xh;                  /* exchange hash in progress */
static x25519_ctx *kex;
static uint32_t remote_chan, remote_window, local_window_used;
static uint32_t next_draw, t_conn;

/* allocated while the SSH client runs; host and user are kept in extended
 * RAM in between */
#define X_STATE (XRAM_CON + 0xC00UL)
struct ssh_mem {
    char f_host[48], f_user[32];        /* saved: keep first */
    char f_pass[48], msg[CON_COLS + 1];
    struct line_edit le;
    uint8_t h[32], sid[32], peer_pub[32];
    uint8_t k1_out[32], k2_out[32], k1_in[32], k2_in[32];
};
#define SAVED_SIZE (48 + 32)
static struct ssh_mem *sm;
#define f_host (sm->f_host)
#define f_user (sm->f_user)
#define f_pass (sm->f_pass)
#define msg (sm->msg)
#define le (sm->le)
#define h (sm->h)
#define sid (sm->sid)
#define peer_pub (sm->peer_pub)
#define k1_out (sm->k1_out)
#define k2_out (sm->k2_out)
#define k1_in (sm->k1_in)
#define k2_in (sm->k2_in)

/* ------------------------------------------------------------------ */
/* helpers                                                             */

static void put_u32(uint32_t v)
{
    pb[pb_n++] = (uint8_t)(v >> 24);
    pb[pb_n++] = (uint8_t)(v >> 16);
    pb[pb_n++] = (uint8_t)(v >> 8);
    pb[pb_n++] = (uint8_t)v;
}

static void put_bytes(const void *d, uint16_t n)
{
    if (pb_n + n > PBUF)
        n = PBUF - pb_n;
    memcpy(pb + pb_n, d, n);
    pb_n += n;
}

static void put_string(const void *d, uint16_t n)
{
    put_u32(n);
    put_bytes(d, n);
}

static void put_cstr(const char *s)
{
    put_string(s, (uint16_t)strlen(s));
}

static void begin(uint8_t type)
{
    pb_n = 0;
    pb[pb_n++] = type;
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint16_t)p[2] << 8 | p[3];
}

static void hash_string(const void *d, uint32_t n)
{
    uint8_t l[4];
    l[0] = (uint8_t)(n >> 24);
    l[1] = (uint8_t)(n >> 16);
    l[2] = (uint8_t)(n >> 8);
    l[3] = (uint8_t)n;
    sha256_update(xh, l, 4);
    sha256_update(xh, d, n);
}

/* SSH mpint of a 32-byte big-endian number */
static void hash_mpint(const uint8_t *k)
{
    uint8_t i = 0;
    while (i < 32 && !k[i])
        i++;
    if (i < 32 && (k[i] & 0x80)) {
        uint8_t l[5] = {0, 0, 0, 0, 0};
        l[3] = 33 - i;
        sha256_update(xh, l, 5);
    } else {
        uint8_t l[4] = {0, 0, 0, 0};
        l[3] = 32 - i;
        sha256_update(xh, l, 4);
    }
    sha256_update(xh, k + i, 32 - i);
}

static void nonce8(uint8_t n[8], uint32_t seq)
{
    memset(n, 0, 4);
    n[4] = (uint8_t)(seq >> 24);
    n[5] = (uint8_t)(seq >> 16);
    n[6] = (uint8_t)(seq >> 8);
    n[7] = (uint8_t)seq;
}

static void fail(const char *why)
{
    dbg_printf("ssh: fail '%s' in state %u\n", why, st);
    strncpy(msg, why, CON_COLS);
    msg[CON_COLS] = 0;
    if (st >= S_OPEN || st == S_SHELL) {
        term_write((const uint8_t *)"\r\n", 2);
        term_write((const uint8_t *)why, (uint16_t)strlen(why));
    }
    st = S_CLOSED;
    if (sock) {
        lwip_socket_destroy(sock);
        sock = NULL;
    }
}

/* send the payload in pb as one packet */
static void send_packet(void)
{
    uint16_t body = 1 + pb_n, pad, total;
    uint8_t *p, nonce[8];
    pad = 8 - ((enc_out ? body : body + 4) & 7);
    if (pad < 4)
        pad += 8;
    total = 4 + body + pad + (enc_out ? 16 : 0);
    p = malloc(total);
    if (!p) {
        fail("no memory");
        return;
    }
    p[0] = 0;
    p[1] = 0;
    p[2] = (uint8_t)((body + pad) >> 8);
    p[3] = (uint8_t)(body + pad);
    p[4] = (uint8_t)pad;
    memcpy(p + 5, pb, pb_n);
    {                                   /* padding needs no cryptographic randomness */
        uint8_t i;
        uint32_t r = sys_rand32();
        for (i = 0; i < pad; i++)
            p[5 + pb_n + i] = (uint8_t)(r >> (i & 3) * 8);
    }
    if (enc_out) {
        chacha_ctx c;
        poly1305_ctx m;
        nonce8(nonce, seq_out);
        chacha_init(&c, k1_out, nonce, 8, 0);
        chacha_xor(&c, p, 4);
        chacha_init(&c, k2_out, nonce, 8, 0);
        chacha_block(c.s, c.ks);
        poly1305_init(&m, c.ks);
        c.s[12] = 1;
        chacha_xor(&c, p + 4, body + pad);
        poly1305_update(&m, p, 4 + body + pad);
        poly1305_final(&m, p + 4 + body + pad);
    }
    seq_out++;
    if (lwip_socket_write(sock, p, total) != LWIP_OK)
        fail("send failed");
    free(p);
}

/* ------------------------------------------------------------------ */
/* protocol                                                            */

static void send_kexinit(void)
{
    uint8_t cookie[16];
    begin(20);
    rng_bytes(cookie, 16);
    put_bytes(cookie, 16);
    put_cstr("curve25519-sha256,curve25519-sha256@libssh.org");
    put_cstr("ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ssh-rsa");
    put_cstr("chacha20-poly1305@openssh.com");
    put_cstr("chacha20-poly1305@openssh.com");
    put_cstr("hmac-sha2-256");
    put_cstr("hmac-sha2-256");
    put_cstr("none");
    put_cstr("none");
    put_cstr("");
    put_cstr("");
    pb[pb_n++] = 0;
    put_u32(0);
    hash_string(pb, pb_n);              /* I_C */
    send_packet();
}

static void derive(uint8_t *out, const uint8_t *k, char letter)
{
    sha256_ctx c;
    uint8_t t[32];
    /* first 32 bytes: HASH(K || H || letter || session_id) */
    sha256_init(&c);
    xh = &c;
    hash_mpint(k);
    sha256_update(&c, h, 32);
    sha256_update(&c, &letter, 1);
    sha256_update(&c, sid, 32);
    sha256_final(&c, out);
    /* next 32: HASH(K || H || first 32) */
    sha256_init(&c);
    hash_mpint(k);
    sha256_update(&c, h, 32);
    sha256_update(&c, out, 32);
    sha256_final(&c, t);
    memcpy(out + 32, t, 32);
    xh = NULL;
}

static void kex_done(void)
{
    uint8_t k[32], key[64];
    x25519_result(kex, k);
    free(kex);
    kex = NULL;
    hash_mpint(k);
    sha256_final(xh, h);
    free(xh);
    xh = NULL;
    memcpy(sid, h, 32);
    derive(key, k, 'C');                /* client to server: K_2 | K_1 */
    memcpy(k2_out, key, 32);
    memcpy(k1_out, key + 32, 32);
    derive(key, k, 'D');
    memcpy(k2_in, key, 32);
    memcpy(k1_in, key + 32, 32);
    memset(k, 0, 32);
    memset(key, 0, 64);
    begin(21);                          /* NEWKEYS */
    send_packet();
    enc_out = 1;
    st = S_NEWKEYS;
    strcpy(msg, "authenticating");
    if (got_newkeys) {
        enc_in = 1;
        begin(5);
        put_cstr("ssh-userauth");
        send_packet();
        st = S_SERVICE;
    }
}

static void send_password_auth(void)
{
    begin(50);
    put_cstr(f_user);
    put_cstr("ssh-connection");
    if (kbd_int) {
        put_cstr("keyboard-interactive");
        put_cstr("");
        put_cstr("");
    } else {
        put_cstr("password");
        pb[pb_n++] = 0;
        put_cstr(f_pass);
    }
    send_packet();
}

static void send_window_adjust(void)
{
    begin(93);
    put_u32(remote_chan);
    put_u32(local_window_used);
    send_packet();
    local_window_used = 0;
}

static void handle_packet(uint8_t *p, uint16_t n)
{
    uint8_t type = p[0];
    switch (type) {
    case 1: {                           /* DISCONNECT */
        uint32_t l = n >= 9 ? get_u32(p + 5) : 0;
        char r[CON_COLS + 1];
        if (l > CON_COLS)
            l = CON_COLS;
        memcpy(r, p + 9, (uint16_t)l);
        r[l] = 0;
        fail(l ? r : "disconnected");
        return;
    }
    case 2: case 3: case 4: case 7:     /* IGNORE, UNIMPLEMENTED, DEBUG, EXT_INFO */
        return;
    case 20:                            /* KEXINIT */
        if (st != S_KEXINIT) {
            fail("rekeying unsupported");
            return;
        }
        hash_string(p, n);              /* I_S */
        begin(30);
        put_string(kx_pub(), 32);       /* Q_C */
        send_packet();
        st = S_ECDH;
        strcpy(msg, "key exchange");
        return;
    case 31: {                          /* KEX_ECDH_REPLY */
        uint32_t ks = get_u32(p + 1), ql;
        if (st != S_ECDH || 5 + ks + 4 > n)
            break;
        hash_string(p + 5, ks);         /* K_S */
        ql = get_u32(p + 5 + ks);
        if (ql != 32) {
            fail("bad server key");
            return;
        }
        memcpy(peer_pub, p + 9 + ks, 32);
        hash_string(kx_pub(), 32);      /* Q_C */
        hash_string(peer_pub, 32);      /* Q_S */
        kex = malloc(sizeof(x25519_ctx));
        if (!kex) {
            fail("no memory");
            return;
        }
        x25519_start(kex, kx_priv(), peer_pub);
        st = S_COMPUTE;
        strcpy(msg, "computing key");
        return;
    }
    case 21:                            /* NEWKEYS */
        got_newkeys = 1;
        if (st == S_NEWKEYS) {
            enc_in = 1;
            begin(5);
            put_cstr("ssh-userauth");
            send_packet();
            st = S_SERVICE;
        }
        return;
    case 6:                             /* SERVICE_ACCEPT */
        if (st == S_SERVICE) {
            send_password_auth();
            st = S_AUTH;
        }
        return;
    case 53: {                          /* USERAUTH_BANNER */
        uint32_t l = get_u32(p + 1);
        if (l + 5 <= n)
            term_write(p + 5, (uint16_t)l);
        return;
    }
    case 51: {                          /* USERAUTH_FAILURE */
        uint32_t l = get_u32(p + 1);
        p[5 + (l < n - 5 ? l : n - 5)] = 0;
        if (!kbd_int && strstr((char *)p + 5, "keyboard-interactive")) {
            kbd_int = 1;
            send_password_auth();
        } else {
            fail("login refused");
        }
        return;
    }
    case 60:                            /* USERAUTH_INFO_REQUEST */
        if (kbd_int) {
            uint16_t o = 1;
            uint32_t i, num;
            o += 4 + (uint16_t)get_u32(p + o);      /* name */
            o += 4 + (uint16_t)get_u32(p + o);      /* instruction */
            o += 4 + (uint16_t)get_u32(p + o);      /* language */
            num = o + 4 <= n ? get_u32(p + o) : 0;
            begin(61);
            put_u32(num);
            for (i = 0; i < num; i++)
                put_cstr(f_pass);
            send_packet();
        }
        return;
    case 52:                            /* USERAUTH_SUCCESS */
        memset(f_pass, 0, sizeof(f_pass));
        begin(90);
        put_cstr("session");
        put_u32(0);                     /* our channel */
        put_u32(2048);                  /* window */
        put_u32(1024);                  /* max packet */
        send_packet();
        st = S_CHANNEL;
        strcpy(msg, "opening shell");
        return;
    case 91:                            /* CHANNEL_OPEN_CONFIRMATION */
        remote_chan = get_u32(p + 5);
        remote_window = get_u32(p + 9);
        begin(98);
        put_u32(remote_chan);
        put_cstr("pty-req");
        pb[pb_n++] = 0;
        put_cstr("vt100");
        put_u32(TERM_COLS);
        put_u32(TERM_ROWS);
        put_u32(0);
        put_u32(0);
        put_string("\0", 1);            /* no terminal modes */
        send_packet();
        begin(98);
        put_u32(remote_chan);
        put_cstr("shell");
        pb[pb_n++] = 1;
        send_packet();
        st = S_SHELL;
        return;
    case 92:                            /* CHANNEL_OPEN_FAILURE */
        fail("channel refused");
        return;
    case 99:                            /* CHANNEL_SUCCESS */
        dbg_printf("ssh: shell after %lu ms\n", k_now_ms() - t_conn);
        st = S_OPEN;
        msg[0] = 0;
        return;
    case 100:                           /* CHANNEL_FAILURE */
        fail("no shell");
        return;
    case 93:                            /* WINDOW_ADJUST */
        remote_window += get_u32(p + 5);
        return;
    case 94:                            /* CHANNEL_DATA */
    case 95: {                          /* CHANNEL_EXTENDED_DATA */
        uint16_t o = type == 94 ? 5 : 9;
        uint32_t l = get_u32(p + o);
        if (o + 4 + l > n)
            l = n - o - 4;
        term_write(p + o + 4, (uint16_t)l);
        local_window_used += l;
        if (local_window_used >= 1024)
            send_window_adjust();
        if (st == S_SHELL)
            st = S_OPEN;
        return;
    }
    case 96:                            /* CHANNEL_EOF */
        return;
    case 97:                            /* CHANNEL_CLOSE */
        fail("connection closed");
        return;
    case 98:                            /* CHANNEL_REQUEST (exit-status...) */
        return;
    case 80:                            /* GLOBAL_REQUEST */
        if (n >= 5 && 5 + get_u32(p + 1) < n && p[5 + get_u32(p + 1)]) {
            begin(82);                  /* REQUEST_FAILURE */
            send_packet();
        }
        return;
    }
    if (st < S_OPEN) {
        char b[24];
        snprintf(b, sizeof(b), "unexpected msg %u", type);
        fail(b);
    }
}

/* ------------------------------------------------------------------ */
/* receive                                                             */

static void rx_process(void)
{
    for (;;) {
        uint32_t len;
        uint16_t need;
        if (st == S_VERSION) {
            uint8_t *nl = memchr(rx, '\n', rx_n);
            uint16_t l;
            if (!nl) {
                if (rx_n == RXBUF)
                    rx_n = 0;
                return;
            }
            l = (uint16_t)(nl - rx) + 1;
            if (l > 4 && !memcmp(rx, "SSH-", 4)) {
                uint16_t vl = l - 1;
                if (vl && rx[vl - 1] == '\r')
                    vl--;
                xh = malloc(sizeof(sha256_ctx));
                if (!xh) {
                    fail("no memory");
                    return;
                }
                sha256_init(xh);
                hash_string(version, sizeof(version) - 1);   /* V_C */
                hash_string(rx, vl);                         /* V_S */
                st = S_KEXINIT;
                send_kexinit();
            }
            memmove(rx, rx + l, rx_n - l);
            rx_n -= l;
            continue;
        }
        if (st == S_COMPUTE || st == S_CLOSED || st == S_IDLE)
            return;
        if (rx_n < 4)
            return;
        if (enc_in) {
            uint8_t nonce[8], l4[4];
            chacha_ctx c;
            memcpy(l4, rx, 4);
            nonce8(nonce, seq_in);
            chacha_init(&c, k1_in, nonce, 8, 0);
            chacha_xor(&c, l4, 4);
            len = get_u32(l4);
        } else {
            len = get_u32(rx);
        }
        if (len < 5 || len > 35000u) {
            fail("bad packet");
            return;
        }
        need = (uint16_t)len + 4 + (enc_in ? 16 : 0);
        if (len + 20 > RXBUF) {
            /* too big for us: drop it (never seen during the handshake) */
            discard = need - rx_n;
            rx_n = 0;
            seq_in++;
            return;
        }
        if (rx_n < need)
            return;
        if (enc_in) {
            uint8_t nonce[8];
            chacha_ctx c;
            nonce8(nonce, seq_in);
            chacha_init(&c, k2_in, nonce, 8, 1);
            chacha_xor(&c, rx + 4, (uint16_t)len);  /* the tag is not checked */
        }
        seq_in++;
        if (len > rx[4] + 1u)
            handle_packet(rx + 5, (uint16_t)len - rx[4] - 1);
        if (!sock)
            return;
        memmove(rx, rx + need, rx_n - need);
        rx_n -= need;
    }
}

static void sock_event(struct lwip_socket *s, lwip_socket_event_type_t type,
                       const void *data, void *arg)
{
    (void)arg;
    if (type == LWIP_SOCKET_EV_STATE_CHANGE) {
        const lwip_socket_state_data_t *e = data;
        if (e->current == LWIP_STATUS_CONNECTED && st == S_CONNECTING) {
            lwip_socket_write(s, (const uint8_t *)version, sizeof(version) - 1);
            lwip_socket_write(s, (const uint8_t *)"\r\n", 2);
            st = S_VERSION;
            strcpy(msg, kx_ready() ? "connected" : "making key");
        } else if (e->current == LWIP_STATUS_RESOLVING) {
            strcpy(msg, "resolving");
        }
    } else if (type == LWIP_SOCKET_EV_ERROR) {
        const lwip_socket_error_data_t *e = data;
        char b[24];
        snprintf(b, sizeof(b), "error %u/%u", e->component, e->err);
        strcpy(msg, b);
    }
}

/* ------------------------------------------------------------------ */
/* user interface                                                      */

static void cleanup(void)
{
    if (sock) {
        lwip_socket_destroy(sock);
        sock = NULL;
    }
    free(rx);
    free(pb);
    free(xh);
    free(kex);
    rx = pb = NULL;
    xh = NULL;
    kex = NULL;
    memset(k1_out, 0, 32);
    memset(k2_out, 0, 32);
    memset(k1_in, 0, 32);
    memset(k2_in, 0, 32);
}

static void ssh_connect(void)
{
    static char host[sizeof(f_host)];  /* lwIP may keep the pointer until DNS is done */
    char *c;
    strcpy(host, f_host);
    c = strchr(host, ':');
    port = 22;
    if (c) {
        *c = 0;
        port = (uint16_t)atoi(c + 1);
    }
    cleanup();
    rx = malloc(RXBUF);
    pb = malloc(PBUF);
    /* no inactivity timeout: the key exchange and an idle shell are silent */
    sock = lwip_socket_create_ex(LWIP_SOCKET_TCP, LWIP_NETIF_EXT, NULL, 0x7FFFFFFFUL, 2048u);
    if (!rx || !pb || !sock) {
        fail("no memory");
        return;
    }
    rx_n = discard = 0;
    seq_out = seq_in = 0;
    enc_in = enc_out = got_newkeys = kbd_int = 0;
    local_window_used = 0;
    term_reset();
    kx_start();
    lwip_socket_on_event(sock, LWIP_SOCKET_EVENTF_ALL, sock_event, NULL);
    if (lwip_socket_connect(sock, host, port) != LWIP_OK) {
        fail("connect failed");
    } else {
        st = S_CONNECTING;
        strcpy(msg, "connecting");
        t_conn = k_now_ms();
    }
}

static void start_field(uint8_t f)
{
    field = f;
    if (f == F_HOST)
        le_init(&le, f_host, sizeof(f_host) - 1, 0);
    else if (f == F_USER)
        le_init(&le, f_user, sizeof(f_user) - 1, 0);
    else if (f == F_PASS)
        le_init(&le, f_pass, sizeof(f_pass) - 1, 1);
    in_mode = IN_LOWER;
}

static void draw(void)
{
    char b[CON_COLS + 1];
    ui_clear();
    if (field != F_NONE) {
        snprintf(b, sizeof(b), "%c SSH login", in_mode_char());
        ui_status(b);
        ui_text(0, "host[:port], user and");
        ui_text(1, "password. ENTER = next");
        ui_text(2, "MODE = quit");
        ui_text(4, field == F_HOST ? "" : f_host);
        if (field == F_PASS)
            ui_text(5, f_user);
        le_draw(&le, field == F_HOST ? 4 : field == F_USER ? 5 : 6,
                field == F_HOST ? "host " : field == F_USER ? "user " : "pass ");
    } else {
        if (st == S_OPEN && !msg[0])
            snprintf(b, sizeof(b), "%c%s %s", in_mode_char(), ctrl_next ? "^" : " ", f_host);
        else if (st == S_COMPUTE && kex)
            snprintf(b, sizeof(b), "key exchange %u%%", x25519_progress(kex));
        else if ((st == S_VERSION || st == S_KEXINIT) && !kx_ready())
            snprintf(b, sizeof(b), "key pair %u%%", kx_progress());
        else
            snprintf(b, sizeof(b), "%c %s", in_mode_char(), msg);
        ui_status(b);
        term_draw(back);
    }
    lcd_update();
}

/* returns 0 if there is not enough memory */
uint8_t ssh_enter(void)
{
    static uint8_t saved;
    if (!sm) {
        sm = calloc(1, sizeof(*sm));
        if (!sm || !term_open()) {
            free(sm);
            sm = NULL;
            return 0;
        }
        if (saved)
            xram_read(X_STATE, sm, SAVED_SIZE);
        saved = 1;
        st = S_IDLE;
    }
    if (st == S_IDLE || st == S_CLOSED) {
        st = S_IDLE;
        start_field(F_HOST);
    }
    kx_start();                         /* the key pair is needed soon */
    draw();
    return 1;
}

static void ssh_leave(void)
{
    cleanup();
    st = S_IDLE;
    memset(f_pass, 0, sizeof(f_pass));
    xram_write(X_STATE, sm, SAVED_SIZE);
    memset(sm, 0, sizeof(*sm));
    free(sm);
    sm = NULL;
    term_close();
}

static void send_keys(const uint8_t *s, uint8_t n)
{
    if (st != S_OPEN || !n)
        return;
    begin(94);
    put_u32(remote_chan);
    put_string(s, n);
    send_packet();
}

/* returns 0 when the user quits */
uint8_t ssh_key(uint8_t k)
{
    uint8_t buf[4];
    int16_t c;
    if (k == K_NONE || !sm)
        return 1;
    if (field != F_NONE) {
        int8_t r = le_key(&le, k);
        if (r == LE_CANCEL) {
            ssh_leave();
            return 0;
        }
        if (r == LE_DONE) {
            if (field == F_HOST && f_host[0])
                start_field(F_USER);
            else if (field == F_USER && f_user[0])
                start_field(F_PASS);
            else if (field == F_PASS) {
                field = F_NONE;
                ssh_connect();
            }
        }
        draw();
        return 1;
    }
    if (in_2nd && k == K_MODE) {        /* QUIT */
        in_2nd = 0;
        ssh_leave();
        return 0;
    }
    if (in_2nd && (k == K_UP || k == K_DOWN)) {
        if (k == K_UP && back < term_scrollback())
            back += 4;
        else if (k == K_DOWN)
            back = back > 4 ? back - 4 : 0;
        draw();
        return 1;
    }
    back = 0;
    if (st == S_CLOSED) {               /* any key: back to the login form */
        if (k == K_ENTER) {
            st = S_IDLE;
            start_field(F_HOST);
        }
        draw();
        return 1;
    }
    switch (k) {
    case K_ENTER: buf[0] = '\r'; send_keys(buf, 1); break;
    case K_DEL: buf[0] = 0x7F; send_keys(buf, 1); break;
    case K_CLEAR: buf[0] = 3; send_keys(buf, 1); break;
    case K_VARS: buf[0] = '\t'; send_keys(buf, 1); break;
    case K_MODE: buf[0] = 27; send_keys(buf, 1); break;
    case K_XT: ctrl_next = 1; break;
    case K_UP: case K_DOWN: case K_RIGHT: case K_LEFT:
        buf[0] = 27;
        buf[1] = '[';
        buf[2] = k == K_UP ? 'A' : k == K_DOWN ? 'B' : k == K_RIGHT ? 'C' : 'D';
        send_keys(buf, 3);
        break;
    default:
        c = in_char(k);
        if (c >= 0) {
            if (ctrl_next) {
                ctrl_next = 0;
                c &= 0x1F;
            }
            buf[0] = (uint8_t)c;
            send_keys(buf, 1);
        }
    }
    draw();
    return 1;
}

void ssh_poll(void)
{
    if (!sm || !sock)
        return;
    if (st == S_COMPUTE) {
        if (kex && kx_shared_step(kex, 100)) {
            dbg_printf("ssh: key exchange done at %lu ms\n", k_now_ms() - t_conn);
            kex_done();
            rx_process();               /* NEWKEYS may already be waiting */
        }
    } else if ((st == S_VERSION || st == S_KEXINIT) && !kx_ready()) {
        kx_run(100);                    /* the shared key pair must exist first */
    }
    while (sock && st != S_COMPUTE && (kx_ready() || st > S_KEXINIT)) {
        size_t n = lwip_socket_available(sock);
        if (!n)
            break;
        if (discard) {
            uint8_t tmp[64];
            n = lwip_socket_read(sock, tmp, n > discard ? (discard > 64 ? 64 : discard)
                                                        : (n > 64 ? 64 : n));
            discard -= (uint16_t)n;
            continue;
        }
        if (n > RXBUF - rx_n)
            n = RXBUF - rx_n;
        if (!n)
            break;
        n = lwip_socket_read(sock, rx + rx_n, n);
        rx_n += (uint16_t)n;
        rx_process();
    }
    if (sock && !lwip_socket_is_active(sock) && st != S_CLOSED)
        fail(msg[0] && !memcmp(msg, "error", 5) ? msg : "connection lost");
    if ((term_dirty || st != S_OPEN) && (int32_t)(k_now_ms() - next_draw) >= 0) {
        next_draw = k_now_ms() + (st == S_OPEN ? 150 : 1000);
        draw();
    }
}
