/* apps.c - test applications of the network console on lib/net/sock84.c:
 * create a socket, react to its events, read from the main loop. */
#include <string.h>
#include "kernel.h"
#include "apps.h"
#include "sock84.h"
#include "time84.h"
#include "lwip/apps/sntp.h"
#include "x25519.h"
#include "chacha.h"
#include "aes.h"
#include "sha256.h"

static struct sock *app_sock;
static uint8_t app_lines;          /* reply lines still to print */
static uint16_t app_rx_total;
static char app_req[112];
static char app_line[CON_COLS + 1];
static uint8_t app_line_len;
static uint32_t app_started;

static const char *const status_names[] = {
    "init", "network", "resolving", "connecting", "connected", "closed", "error"};

static void app_event(struct sock *sock, uint8_t ev, uint8_t a, int8_t err)
{
    if (ev == SOCK_EV_STATE) {
        con_printf("[%s]\n", status_names[a]);
        if (a == SOCK_CONNECTED && app_req[0] && sock_write(sock, app_req, (uint16_t)strlen(app_req)))
            con_puts("write failed\n");
    } else {
        con_printf("error %u/%d\n", a, err);
    }
}

static void app_start(uint8_t type, const char *host, uint16_t port, uint8_t lines)
{
    if (app_sock) {
        con_puts("busy\n");
        return;
    }
    app_sock = sock_new(type, type == SOCK_TLS ? 120000u : 30000u, app_event);
    if (!app_sock) {
        con_puts("socket: no memory\n");
        return;
    }
    app_lines = lines;
    app_rx_total = 0;
    app_line_len = 0;
    app_started = k_now_ms();
    con_printf("%s:%u\n", host, port);
    if (sock_connect(app_sock, host, port)) {
        con_puts("connect failed\n");
        sock_free(app_sock);
        app_sock = NULL;
    }
}

void app_http_get(const char *host)
{
    snprintf(app_req, sizeof(app_req),
             "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: lwIP-84+\r\n\r\n", host);
    app_start(SOCK_TCP, host, 80, 3);
}

void app_https_get(const char *host, uint16_t port)
{
    snprintf(app_req, sizeof(app_req),
             "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: lwIP-84+\r\n\r\n", host);
    app_start(SOCK_TLS, host, port, 4);
}

void app_download(const char *host, uint16_t port)
{
    app_req[0] = 0;
    app_start(SOCK_TCP, host, port, 0);
}

void app_tcp_echo(const char *host, uint16_t port)
{
    strcpy(app_req, "hello from a TI-84 Plus\n");
    app_start(SOCK_TCP, host, port, 2);
}

static void app_consume(const uint8_t *buf, size_t n)
{
    size_t i;
    for (i = 0; i < n && app_lines; i++) {
        char c = buf[i];
        if (c == '\r')
            continue;
        if (c == '\n' || app_line_len == CON_COLS) {
            app_line[app_line_len] = 0;
            con_printf("%s\n", app_line);
            app_line_len = 0;
            app_lines--;
            if (c == '\n')
                continue;
        }
        app_line[app_line_len++] = c;
    }
}

void app_poll(void)
{
    uint8_t buf[64];
    size_t n;
    if (!app_sock)
        return;
    while ((n = sock_available(app_sock)) != 0) {
        n = sock_read(app_sock, buf, n > sizeof(buf) ? sizeof(buf) : n);
        if (!n)
            break;
        app_rx_total += n;
        app_consume(buf, n);
    }
    if (!sock_is_active(app_sock)) {
        uint32_t ms = k_now_ms() - app_started;
        con_printf("done: %u bytes, %lu ms\n", app_rx_total, ms);
        if (ms)
            con_printf("%lu bytes/s\n", (uint32_t)app_rx_total * 1000u / ms);
        sock_free(app_sock);
        app_sock = NULL;
    }
}

void app_sntp(void)
{
    sntp_stop();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, SNTP_SERVER_ADDRESS);
    sntp_init();
    con_puts("sntp started\n");
}

/* days since 1970-01-01 to a civil date (H. Hinnant's algorithm) */
static void civil_from_days(int32_t z, uint16_t *y, uint8_t *m, uint8_t *d)
{
    int32_t era, yy;
    uint32_t doe, yoe, doy, mp;
    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (uint32_t)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = (int32_t)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    *d = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    *m = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    *y = (uint16_t)(yy + (*m <= 2));
}

void app_show_time(void)
{
    uint32_t t;
    uint16_t y;
    uint8_t mo, d;
    if (!time84_valid()) {
        con_puts("time not set (MODE)\n");
        return;
    }
    t = time84_now();
    civil_from_days((int32_t)(t / 86400u), &y, &mo, &d);
    t %= 86400u;
    /* note: SDCC passes arguments cast to (uint8_t) as single bytes to
     * variadic functions, so use unsigned here */
    con_printf("%u-%02u-%02u %02u:%02u:%02u UTC\n", y, (unsigned)mo, (unsigned)d,
               (unsigned)(t / 3600u), (unsigned)((t / 60u) % 60u), (unsigned)(t % 60u));
}

/* crypto self test: RFC 7748 X25519 vector, with timing */
void app_selftest(void)
{
    static const uint8_t s[32] = {
        0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d, 0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
        0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18, 0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4};
    static const uint8_t u[32] = {
        0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb, 0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
        0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b, 0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c};
    static const uint8_t want[4] = {0xc3, 0xda, 0x55, 0x37};
    uint8_t out[32], i;
    uint32_t t;
    x25519_ctx *c;
    {
        /* throughput of the primitives */
        static uint32_t blk[16];
        uint8_t *buf = malloc(1024);
        sha256_ctx sc;
        poly1305_ctx pc;
        uint16_t k;
        if (buf) {
            memset(buf, 0x5a, 1024);
            t = k_now_ms();
            for (k = 0; k < 64; k++)
                chacha_block(blk, buf);
            con_printf("chacha 4KB %lu ms\n", k_now_ms() - t);
            {
                /* the two TLS ciphers as used for received records */
                chacha_ctx *cc = malloc(sizeof(*cc));
                aes_ctr_ctx *ac = malloc(sizeof(*ac));
                if (cc && ac) {
                    t = k_now_ms();
                    chacha_init(cc, buf, buf, 12, 1);
                    for (k = 0; k < 4; k++)
                        chacha_xor(cc, buf, 1024);
                    con_printf("chacha20 dec 4KB %lu ms\n", k_now_ms() - t);
                    ac->ready = 0;
                    t = k_now_ms();
                    aes_ctr_init(ac, buf, buf, 2);
                    for (k = 0; k < 4; k++)
                        aes_ctr_xor(ac, buf, 1024);
                    con_printf("aes128 dec 4KB %lu ms\n", k_now_ms() - t);
                    t = k_now_ms();
                    aes_gcm_seal(buf, buf, buf, 5, buf + 16, 1000, out);
                    con_printf("aes-gcm seal 1KB %lu ms\n", k_now_ms() - t);
                    /* FIPS 197 C.1 */
                    for (k = 0; k < 16; k++) {
                        buf[k] = (uint8_t)k;
                        buf[16 + k] = (uint8_t)(k * 0x11);
                    }
                    aes128_expand(ac->rk, buf);
                    aes128_encrypt(ac->rk, buf + 16, out);
                    con_printf("aes %s\n", out[0] == 0x69 && out[1] == 0xc4 && out[15] == 0x5a ? "ok" : "FAIL");
                }
                free(cc);
                free(ac);
            }
            t = k_now_ms();
            sha256_init(&sc);
            for (k = 0; k < 4; k++)
                sha256_update(&sc, buf, 1024);
            sha256_final(&sc, out);
            con_printf("sha256 4KB %lu ms\n", k_now_ms() - t);
            {
                uint8_t d[32];
                static const char msg_bk[] = "abc";
                char m[4];
                memcpy(m, msg_bk, 4);
                sha256(m, 3, d);
                dbg_printf("sha abc %02x%02x%02x%02x\n", d[0], d[1], d[2], d[3]);
                __asm__("di");
                sha256(m, 3, d);
                __asm__("ei");
                dbg_printf("sha abc di %02x%02x%02x%02x\n", d[0], d[1], d[2], d[3]);
                for (k = 0; k < 200; k++)
                    buf[k] = (uint8_t)k;
                sha256(buf, 200, d);
                dbg_printf("sha 200 %02x%02x%02x%02x\n", d[0], d[1], d[2], d[3]);
            }
            t = k_now_ms();
            poly1305_init(&pc, buf);
            poly1305_update(&pc, buf, 1024);
            poly1305_final(&pc, out);
            con_printf("poly1305 1KB %lu ms\n", k_now_ms() - t);
            t = k_now_ms();
            for (k = 0; k < 100; k++)
                xram_write(XRAM_BROWSER + k * 10u, buf, 1024);
            con_printf("xram 100KB %lu ms\n", k_now_ms() - t);
            free(buf);
        }
        con_flush();
    }
    c = malloc(sizeof(*c));
    if (!c) {
        con_puts("no memory\n");
        return;
    }
    t = k_now_ms();
    x25519_start(c, s, u);
    for (i = 0; i < 100; i++)
        x25519_step(c);
    con_printf("100 fe ops %lu ms\n", k_now_ms() - t);
    con_flush();
    while (!x25519_step(c))
        ;
    x25519_result(c, out);
    free(c);
    con_printf("x25519 %s %lu ms\n", memcmp(out, want, 4) ? "FAIL" : "ok", k_now_ms() - t);
    dbg_printf("x25519 %02x%02x%02x%02x\n", out[0], out[1], out[2], out[3]);
}
