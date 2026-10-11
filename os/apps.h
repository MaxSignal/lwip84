/* apps.h - test applications on lwIP-CE's socket API (os/apps.c) */
#ifndef TI84P_APPS_H
#define TI84P_APPS_H
#include <stdint.h>
void app_http_get(const char *host);
void app_tcp_echo(const char *host, uint16_t port);
void app_https_get(const char *host, uint16_t port);
void app_download(const char *host, uint16_t port);
void app_sntp(void);
void app_show_time(void);
void app_poll(void);
void app_selftest(void);
#endif
