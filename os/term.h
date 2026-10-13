/* term.h - terminal emulator for the SSH client (os/term.c) */
#ifndef TI84P_TERM_H
#define TI84P_TERM_H
#include <stdint.h>
#define TERM_COLS 24
#define TERM_ROWS_MAX 9
extern uint8_t term_rows;        /* 7 with the external font (8-pixel rows), else 9 */
extern uint8_t term_dirty;
uint8_t term_open(void);         /* allocate the screen; 0: no memory */
void term_close(void);
void term_reset(void);
void term_write(const uint8_t *s, uint16_t n);
void term_draw(uint16_t back);
uint16_t term_scrollback(void);
#endif
