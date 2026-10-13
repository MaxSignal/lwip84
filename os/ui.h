/* ui.h - full-screen layout for applications (os/ui.c): an inverted
 * status line and text rows of 24 cells (bit 7: inverted; font glyphs take
 * two cells, see font.h) */
#ifndef TI84P_UI_H
#define TI84P_UI_H
#include <stdint.h>
#define UI_ROWS 9
void ui_clear(void);
void ui_status(const char *s);
void ui_text(uint8_t row, const char *s);       /* 6-pixel row 0..8, cleared first */
uint8_t ui_row_height(const char *s);           /* 8 with font glyphs, else 6 */
void ui_text_at(uint8_t y, const char *s, uint8_t h);   /* at pixel row y */
/* UTF-8 to cells: characters without a glyph become '?'; at most max
 * bytes with the NUL */
void ui_from_utf8(const char *s, char *out, uint8_t max);
/* one UTF-8 character at s: its length in bytes, code point in *cp */
uint8_t ui_utf8_char(const char *s, uint16_t *cp);
#endif
