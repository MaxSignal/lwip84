/* input.h - keypad text input and a one-line editor (os/input.c) */
#ifndef TI84P_INPUT_H
#define TI84P_INPUT_H
#include <stdint.h>
#include "ime.h"

/* IN_LANG: the language chosen from the list (lib/ime/ime.h) */
enum { IN_NUM, IN_UPPER, IN_LOWER, IN_LANG };
#define IN_MODE_CHANGED (-2)
#define IN_PICK (-3)
extern uint8_t in_mode, in_2nd;

/* character for a key in the current mode, -1 for none,
 * IN_MODE_CHANGED for ALPHA and 2nd, IN_PICK for 2nd+ALPHA */
int16_t in_char(uint8_t k);
/* the mode as cells for a status line: "1", "A", "a", "2" (2nd pending)
 * or the language's letter */
const char *in_mode_str(void);
/* text for a key, through the input method in the language mode: the
 * UTF-8 text it commits goes to out (IME_OUT_MAX bytes) - also when the
 * result is IN_MODE_CHANGED or IN_PICK, which commit the composition */
int16_t in_text(uint8_t k, char *out);

/* the input language list (2nd+ALPHA): drawn over the screen */
extern uint8_t in_picking;
void in_pick_open(void);
uint8_t in_pick_key(uint8_t k);         /* 1 when the list is closed */
void in_pick_draw(void);

/* the settings screen: languages offered in the list */
void in_setup_draw(void);
uint8_t in_setup_key(uint8_t k);        /* 0 to leave */

/* one-line editor; buf is UTF-8, len and cur in bytes */
struct line_edit {
    char *buf;
    uint8_t max, len, cur, secret;
};
#define LE_DONE 1
#define LE_CANCEL (-1)
void le_init(struct line_edit *le, char *buf, uint8_t max, uint8_t secret);
int8_t le_key(struct line_edit *le, uint8_t k);  /* LE_DONE on ENTER, LE_CANCEL */
void le_draw(struct line_edit *le, uint8_t row, const char *prompt);

#endif
