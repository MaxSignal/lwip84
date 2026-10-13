/* ime.h - input methods: Latin letters typed on the keypad become other
 * scripts (lib/ime/ime.c).
 *
 * The keys give lower-case letters and a few signs; ime_feed() turns them
 * into UTF-8 text.  What is still being typed (romaji that is not a kana
 * yet, a Hangul syllable, Japanese kana before conversion) is the
 * composition, ime_pre, shown at the cursor and committed by ime_flush(). */
#ifndef IME_H
#define IME_H
#include <stdint.h>

enum {
    IME_OFF,            /* no language: ALPHA cycles 1, A, a */
    IME_JA,             /* Japanese: hiragana, converted to kanji */
    IME_KATA,           /* katakana */
    IME_KO,             /* Hangul, dubeolsik */
    IME_RU,             /* Cyrillic, transliterated */
    IME_EL,             /* Greek, transliterated */
    IME_AR,             /* Arabic, transliterated */
    IME_HI,             /* Devanagari, ITRANS-like */
    IME_N
};

extern uint8_t ime_lang;        /* the language of the language mode */
extern uint8_t ime_upper;       /* capitals (Cyrillic, Greek) */
extern uint16_t ime_enabled;    /* bit (1 << language): offered in the list */

const char *ime_label(uint8_t l);       /* short UTF-8 label: "あ", "한", ... */
const char *ime_title(uint8_t l);       /* UTF-8 name for the list */
uint8_t ime_has_case(uint8_t l);
uint8_t ime_converts(uint8_t l);        /* the composition can be converted */

#define IME_PRE_MAX 48
#define IME_OUT_MAX (IME_PRE_MAX + 16)
extern char ime_pre[IME_PRE_MAX + 1];   /* the composition, UTF-8 */

void ime_reset(void);                   /* drop the composition */
/* a typed character (letter, sign, space); committed text goes to out
 * (IME_OUT_MAX bytes, NUL-terminated) */
void ime_feed(char c, char *out);
void ime_flush(char *out);              /* commit the composition */
uint8_t ime_back(void);                 /* DEL in the composition; 0 if empty */

#endif
