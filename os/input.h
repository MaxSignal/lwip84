/* input.h - keypad text input and a one-line editor (os/input.c) */
#ifndef TI84P_INPUT_H
#define TI84P_INPUT_H
#include <stdint.h>

enum { IN_NUM, IN_UPPER, IN_LOWER };
#define IN_MODE_CHANGED (-2)
extern uint8_t in_mode, in_2nd;

/* character for a key in the current mode, -1 for none,
 * IN_MODE_CHANGED for ALPHA and 2nd */
int16_t in_char(uint8_t k);
char in_mode_char(void);        /* '1', 'A', 'a' or '2' (2nd pending) */

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
