/* ui.h - full-screen layout for applications (os/ui.c): an inverted
 * status line and 9 text rows of 24 characters */
#ifndef TI84P_UI_H
#define TI84P_UI_H
#include <stdint.h>
#define UI_ROWS 9
void ui_clear(void);
void ui_status(const char *s);
void ui_text(uint8_t row, const char *s);   /* clears the row first */
#endif
