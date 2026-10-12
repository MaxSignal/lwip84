/* net84.h - lwIP on the CDC-NCM interface (port/net84.c) */
#ifndef NET84_H
#define NET84_H
#include <stdint.h>
struct netif;

extern struct netif *net84_netif;      /* the USB interface (NULL before net84_init) */
void net84_init(void);                  /* lwIP, the interface, USB */
void net84_poll(void);                  /* main loop: USB, frames, timers, sockets */
#endif
