/* dict.h - the conversion dictionaries on flash data pages (lib/ime/dict.c,
 * written by tools/mkdict84.py from dict/) */
#ifndef DICT_H
#define DICT_H
#include <stdint.h>

#ifndef DICT_PAGE
#define DICT_PAGE 0x08          /* first flash page (the SE build: 40h) */
#endif

extern uint8_t dict_ok;
void dict_init(void);           /* checks the header: dict_ok */

/* Japanese: key is the reading in dictionary codes (01h-56h hiragana,
 * 57h long vowel), n bytes.  Returns the number of words (0: none) and
 * where they are in *at */
uint8_t dict_ja_find(const uint8_t *key, uint8_t n, uint32_t *at);
/* word i of the reading found at `at`, as UTF-8 (out: 3 * 32 + 1 bytes) */
void dict_ja_word(uint32_t at, uint8_t i, char *out);

/* Chinese: the characters of a syllable (letters, v for u umlaut);
 * returns how many (0: not a syllable), *at for dict_zh_char */
uint16_t dict_zh_find(const char *syl, uint8_t n, uint32_t *at);
uint16_t dict_zh_char(uint32_t at, uint16_t i);

#ifdef DICT_HOST
void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len);
#endif
#endif
