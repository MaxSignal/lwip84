/* kernel.h - services of the lwIP-84+ OS */
#ifndef TI84P_KERNEL_H
#define TI84P_KERNEL_H
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>

/* time: hardware timer 1 runs at 512 Hz */
#define TICK_HZ 512u
extern volatile uint32_t k_ticks;
uint32_t k_now_ms(void);
void k_delay_ms(uint16_t ms);

/* LCD / console (96x64, 3x5 font in 4x6 cells: 24 columns, 10 rows) */
#define CON_COLS 24
#define CON_ROWS 9                /* below the status line */
extern uint8_t lcd_buf[768];
void lcd_init(void);
void lcd_update(void);
void lcd_clear(void);
void lcd_invert_rect(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
void lcd_draw_char(uint8_t x, uint8_t y, char c);
void lcd_draw_text(uint8_t x, uint8_t y, const char *s);
void con_clear(void);
void con_goto(uint8_t col, uint8_t row);
void con_putc(char c);
void con_puts(const char *s);
void con_printf(const char *fmt, ...);
void con_flush(void);            /* push console changes to the LCD */
void con_refresh(void);          /* redraw everything on the next con_flush */
void con_status(const char *s);  /* inverted status line at the top */
void con_scroll(int8_t d);       /* scroll back (d > 0) or forward */

/* keypad */
enum {
    K_NONE = 0, K_DOWN, K_LEFT, K_RIGHT, K_UP, K_ENTER, K_ADD, K_SUB, K_MUL,
    K_DIV, K_POW, K_CLEAR, K_NEG, K_3, K_6, K_9, K_RPAREN, K_TAN, K_VARS,
    K_DOT, K_2, K_5, K_8, K_LPAREN, K_COS, K_PRGM, K_STAT, K_0, K_1, K_4,
    K_7, K_COMMA, K_SIN, K_APPS, K_XT, K_STO, K_LN, K_LOG, K_SQR, K_INV,
    K_MATH, K_ALPHA, K_GRAPH, K_TRACE, K_ZOOM, K_WINDOW, K_YEQU, K_2ND,
    K_MODE, K_DEL, K_ON
};
uint8_t kbd_scan(void);          /* raw: key currently held, or K_NONE */
uint8_t kbd_get(void);           /* edge-triggered with auto-repeat, non-blocking */

/* formatting and heap: see include/ti84p_libc.h */
#include "ti84p_libc.h"
void heap_init(void);

/* debug output (DEBUG builds): emulator console when running in emu84,
 * otherwise the LCD console */
void dbg_printf(const char *fmt, ...);

/* far.c (page 0) */
extern uint8_t k_is_emulator;
void k_detect_platform(void);
void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len);

/* power */
extern volatile uint8_t k_on_pressed, k_on_masked;
void k_power_off(void);

#endif
