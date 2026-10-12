/* font.h - the external font on flash data pages (os/font.c, written by
 * tools/mkfont84.py).
 *
 * In page text a font glyph takes two cells: a lead byte 01h-1Fh and a
 * trail byte 01h-FFh that together give the glyph number, so text stays a
 * NUL-terminated string and ASCII (and bit 7 for inverted ASCII) keeps its
 * meaning. */
#ifndef FONT_H
#define FONT_H
#include <stdint.h>

#define FONT_PAGE 0x08          /* first flash page of the font */
#define FONT_NONE 0xFFFFu

#define FONT_IS_LEAD(c) ((uint8_t)(c) >= 1 && (uint8_t)(c) <= 0x1F)
#define FONT_LEAD(g) ((char)(1 + (g) / 255))
#define FONT_TRAIL(g) ((char)(1 + (g) % 255))
#define FONT_GLYPH(l, t) ((uint16_t)((uint8_t)(l) - 1) * 255 + (uint8_t)(t) - 1)

extern uint8_t font_ok;
void font_init(void);                   /* checks the font; font_ok */
uint16_t font_find(uint16_t cp);        /* glyph of a code point or FONT_NONE */
/* rows of a glyph (bit 7 = leftmost pixel); returns its width in pixels */
uint8_t font_glyph(uint16_t g, uint8_t rows[8]);

#ifdef HTML_HOST
void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len);
#endif
#endif
