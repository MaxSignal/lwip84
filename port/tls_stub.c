/* tls_stub.c - TLS is not available on the TI-84 Plus port (yet).
 *
 * lwIP-CE's TLS 1.3 stack depends on eZ80 assembly for AES/GHASH, SHA-256,
 * RSA and X25519; on a 15 MHz Z80 a handshake would take minutes and the
 * 16 KiB TLS records do not fit the 32 KiB data window.  These stubs make
 * TLS/WSS sockets fail cleanly (lwip_socket_create returns NULL), while
 * TCP, UDP and plain WebSocket sockets work. */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "lwip/opt.h"
#include "lwip/altcp.h"
#include "tls/includes/tls.h"
#include "tls/includes/random.h"
#include "apps/altcp_tls/altcp_tls_ce.h"

struct tls_context tls_ctx;

bool tls_init(void)
{
    return false;
}

void tls_cleanup(void)
{
}

struct altcp_tls_ce_config *altcp_tls_ce_create_config_client_ecdhe(const char *hostname)
{
    (void)hostname;
    return NULL;
}

void altcp_tls_ce_free_config(struct altcp_tls_ce_config *conf)
{
    (void)conf;
}

struct altcp_pcb *altcp_tls_ce_new(struct altcp_tls_ce_config *config, u8_t ip_type)
{
    (void)config;
    (void)ip_type;
    return NULL;
}

/* Used by WebSocket masking keys: unpredictable enough for that purpose,
 * not a cryptographic RNG. */
void *tls_random_bytes(void *buffer, size_t len)
{
    uint8_t *p = buffer;
    while (len) {
        uint32_t r = sys_rand32();
        uint8_t n = len < 4 ? (uint8_t)len : 4;
        memcpy(p, &r, n);
        p += n;
        len -= n;
    }
    return buffer;
}
