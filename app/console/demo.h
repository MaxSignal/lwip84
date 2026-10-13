/* demo.h - network tests (app/console/demo.c) */
#ifndef TI84P_DEMO_H
#define TI84P_DEMO_H
#include <stdint.h>
#include "lwip/ip_addr.h"
void demo_ping(const ip_addr_t *target);
void demo_dns(const char *host);
void demo_http_get(const char *host);
void demo_tcp_echo(const char *host, uint16_t port);
void demo_poll(void);
#endif
