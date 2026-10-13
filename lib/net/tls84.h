/* tls84.h - minimal TLS 1.3 client (lib/net/tls84.c) */
#ifndef TI84P_TLS84_H
#define TI84P_TLS84_H
#include <stdint.h>
struct altcp_pcb;
extern const char *tls84_last_error;
struct altcp_pcb *tls84_new(const char *host);  /* connect it with altcp_connect() */
void tls84_poll(void);
const char *tls84_status(char *buf, uint8_t n);   /* NULL when nothing to show */
#endif
