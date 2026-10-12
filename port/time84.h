/* time84.h - wall clock: the ASIC's seconds counter plus an offset set by
 * SNTP (port/time84.c) */
#ifndef TIME84_H
#define TIME84_H
#include <stdint.h>
void time84_set(uint32_t unix_seconds);    /* SNTP_SET_SYSTEM_TIME */
uint8_t time84_valid(void);
uint32_t time84_now(void);                 /* Unix time */
#endif
