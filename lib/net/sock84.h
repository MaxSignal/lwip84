/* sock84.h - TCP and TLS client sockets for the applications (lib/net/sock84.c).
 *
 * A thin layer over lwIP's altcp API: it waits for the network (DHCP),
 * resolves the host name, connects (through lib/net/tls84.c for TLS), queues
 * writes that do not fit into the TCP send buffer and keeps received data
 * as lwIP's pbufs until the application reads it (the TCP window opens as
 * it reads).  Events are delivered from sock_poll(), never from inside
 * lwIP, so a callback may free its socket. */
#ifndef SOCK84_H
#define SOCK84_H
#include <stdint.h>

enum { SOCK_TCP, SOCK_TLS };

/* states, in the order a connection goes through them */
enum {
    SOCK_INIT, SOCK_WAITING_NET, SOCK_RESOLVING, SOCK_CONNECTING, SOCK_CONNECTED,
    SOCK_CLOSED, SOCK_ERROR
};

/* where an error came from */
enum { SOCK_ERR_NET, SOCK_ERR_DNS, SOCK_ERR_TCP, SOCK_ERR_TLS, SOCK_ERR_MEM, SOCK_ERR_TIMEOUT };

enum { SOCK_EV_STATE, SOCK_EV_ERROR };

struct sock;
/* SOCK_EV_STATE: a = new state; SOCK_EV_ERROR: a = SOCK_ERR_*, err = lwIP err_t */
typedef void (*sock_cb_t)(struct sock *s, uint8_t ev, uint8_t a, int8_t err);

/* timeout_ms: until the connection is up (DHCP, DNS, TCP and TLS handshake) */
struct sock *sock_new(uint8_t type, uint32_t timeout_ms, sock_cb_t cb);
int8_t sock_connect(struct sock *s, const char *host, uint16_t port);  /* 0 or -1 */
int8_t sock_write(struct sock *s, const void *data, uint16_t len);     /* 0 or -1 */
uint16_t sock_available(const struct sock *s);
uint16_t sock_read(struct sock *s, void *buf, uint16_t len);
uint8_t sock_state(const struct sock *s);
/* connecting, connected, or closed with unread data */
uint8_t sock_is_active(const struct sock *s);
void sock_free(struct sock *s);

/* main loop (from net84_poll) */
void sock_poll(void);

#endif
