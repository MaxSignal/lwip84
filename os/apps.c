/* apps.c - test applications on lwIP-CE's socket API (lwIP.h), written the
 * same way as the CE examples: create a socket, react to events, read the
 * receive ring from the main loop. */
#include <string.h>
#include "kernel.h"
#include "apps.h"
#include "lwIP.h"
#include "lwip/sntp_time.h"

static struct lwip_socket *app_sock;
static uint8_t app_lines;          /* reply lines still to print */
static uint16_t app_rx_total;
static char app_req[112];
static char app_line[CON_COLS + 1];
static uint8_t app_line_len;
static uint32_t app_started;

static const char *const status_names[] = {
    "init", "services", "resolving", "connecting", "connected",
    "closing", "closed", "reset", "error"};

static void app_event(struct lwip_socket *sock, lwip_socket_event_type_t type,
                      const void *ev_data, void *arg)
{
    (void)arg;
    if (type == LWIP_SOCKET_EV_STATE_CHANGE) {
        const lwip_socket_state_data_t *st = ev_data;
        con_printf("[%s]\n", status_names[st->current]);
        if (st->current == LWIP_STATUS_CONNECTED) {
            lwip_error_t e = lwip_socket_write(sock, (const uint8_t *)app_req,
                                               strlen(app_req));
            if (e != LWIP_OK)
                con_printf("write: %u\n", e);
        }
    } else if (type == LWIP_SOCKET_EV_ERROR) {
        const lwip_socket_error_data_t *er = ev_data;
        con_printf("error c%u o%u r%d e%u\n", er->component, er->operation, er->raw_error,
                   er->err);
    }
}

static void app_start(lwip_socket_type_t type, const char *host, uint16_t port, uint8_t lines)
{
    lwip_error_t e;
    if (app_sock) {
        con_puts("busy\n");
        return;
    }
    app_sock = lwip_socket_create(type, LWIP_NETIF_EXT, NULL, 30000);
    if (!app_sock) {
        con_puts("socket: no memory\n");
        return;
    }
    app_lines = lines;
    app_rx_total = 0;
    app_line_len = 0;
    app_started = k_now_ms();
    lwip_socket_on_event(app_sock, LWIP_SOCKET_EVENTF_ALL, app_event, NULL);
    con_printf("%s:%u\n", host, port);
    e = lwip_socket_connect(app_sock, host, port);
    if (e != LWIP_OK) {
        con_printf("connect: %u\n", e);
        lwip_socket_destroy(app_sock);
        app_sock = NULL;
    }
}

void app_http_get(const char *host)
{
    snprintf(app_req, sizeof(app_req),
             "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: lwIP-84+\r\n\r\n", host);
    app_start(LWIP_SOCKET_TCP, host, 80, 3);
}

void app_tcp_echo(const char *host, uint16_t port)
{
    strcpy(app_req, "hello from a TI-84 Plus\n");
    app_start(LWIP_SOCKET_TCP, host, port, 2);
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
    while ((n = lwip_socket_available(app_sock)) != 0) {
        n = lwip_socket_read(app_sock, buf, n > sizeof(buf) ? sizeof(buf) : n);
        if (!n)
            break;
        app_rx_total += n;
        app_consume(buf, n);
    }
    if (!lwip_socket_is_active(app_sock)) {
        con_printf("done: %u bytes, %lu ms\n", app_rx_total, k_now_ms() - app_started);
        lwip_socket_destroy(app_sock);
        app_sock = NULL;
    }
}

void app_sntp(void)
{
    lwip_error_t e = lwip_request_services(LWIP_SOCKET_SVC_SNTP, 15000);
    con_printf("sntp request: %u\n", e);
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
    if (!lwip_sntp_time_was_set()) {
        con_puts("time not set (MODE)\n");
        return;
    }
    t = lwip_sntp_get_unix_time();
    civil_from_days((int32_t)(t / 86400u), &y, &mo, &d);
    t %= 86400u;
    /* note: SDCC passes arguments cast to (uint8_t) as single bytes to
     * variadic functions, so use unsigned here */
    con_printf("%u-%02u-%02u %02u:%02u:%02u UTC\n", y, (unsigned)mo, (unsigned)d,
               (unsigned)(t / 3600u), (unsigned)((t / 60u) % 60u), (unsigned)(t % 60u));
}
