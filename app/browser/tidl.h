/* tidl.h - the TI-OS browser app only (app/browser/tidl.c): a link to a
 * TI file (.8xp, .8xv, .8xg) or a ZIP of them is not shown but read into
 * RAM page 85h; its programs and appvars are listed to choose from, and
 * the chosen ones go into TI-OS after the app (platform/tios/launch.s). */
#ifndef TIDL_H
#define TIDL_H
#include <stdint.h>

#define TIDL_STAY 0
#define TIDL_BACK 1
#define TIDL_INSTALL 2          /* the table is ready: end the app */

uint8_t tidl_kind(const char *url);     /* 0 none, 1 TI file, 2 ZIP */
void tidl_begin(uint8_t zip);
void tidl_feed(const uint8_t *p, uint16_t n);
void tidl_end(void);
void tidl_draw(const char *url);
int8_t tidl_key(uint8_t k);
#endif
