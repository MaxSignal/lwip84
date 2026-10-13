/* font.h - the external font on flash data pages (os/font.c, written by
 * tools/mkfont84.py from fonts/fonts.txt).
 *
 * In text a font glyph takes two cells: a lead byte and a trail byte
 * 01h-FFh that together give the glyph number.  Lead bytes are the values
 * that are neither ASCII nor inverted ASCII: 01h-1Fh, 7Fh, 80h-9Fh and FFh
 * (65 of them, so up to 16575 glyphs).  Text stays a NUL-terminated
 * string, and bit 7 on printable ASCII still means inverted. */
#ifndef FONT_H
#define FONT_H
#include <stdint.h>

#define FONT_PAGE 0x08          /* first flash page of the font */
#define FONT_NONE 0xFFFFu

#define FONT_IS_LEAD(c) ((uint8_t)((uint8_t)(c) - 1) < 0x1F || (uint8_t)(c) == 0x7F || \
                         (uint8_t)((uint8_t)(c) - 0x80) < 0x20 || (uint8_t)(c) == 0xFF)
char font_lead(uint16_t g);
#define FONT_LEAD(g) font_lead(g)
#define FONT_TRAIL(g) ((char)(1 + (g) % 255))
uint16_t font_glyph_of(char lead, char trail);
#define FONT_GLYPH(l, t) font_glyph_of(l, t)

extern uint8_t font_ok;
void font_init(void);                   /* checks the font; font_ok */
uint16_t font_find(uint16_t cp);        /* glyph of a code point or FONT_NONE */
/* rows of a glyph (bit 7 = leftmost pixel); returns its width in pixels:
 * 4 or 8, 0 for a mark drawn over the glyph before it */
uint8_t font_glyph(uint16_t g, uint8_t rows[8]);
uint8_t font_is_rtl(uint16_t g);        /* Arabic: written right to left */

#ifdef HTML_HOST
void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len);
#endif
#endif
