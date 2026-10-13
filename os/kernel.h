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
uint8_t kbd_get(void);           /* next key press from the queue, or K_NONE */
void kbd_isr(void);              /* timer interrupt: scan and queue */

/* formatting and heap: see include/ti84p_libc.h */
#include "ti84p_libc.h"
void heap_init(void);

/* debug output (DEBUG builds): emulator console when running in emu84,
 * otherwise the LCD console */
void dbg_printf(const char *fmt, ...);

/* far.c (page 0) */
extern uint8_t k_is_emulator;
extern uint8_t k_ram48;         /* 48 KiB of RAM: pages 82h-87h are one page */
void k_detect_platform(void);
void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len);
/* extended RAM pages 82h-87h: 96 KiB for application buffers (page text,
 * terminal scrollback); buffers passed here must lie in 8000h-FFFFh.  The
 * calculators with 48 KiB of RAM have one such page (make RAM48=1). */
#ifdef TI84P_RAM48
#define XRAM_SIZE 0x4000UL
#define XRAM_BROWSER 0x0000UL           /* 11.2 KiB: web page (app/browser/html.h) */
#define XRAM_BROWSER_STATE 0x2CC0UL     /* url and host while it is not running */
#define XRAM_HIST 0x2E10UL              /* browser history: 2 pages */
#define XRAM_HIST_SIZE 0x0530u
#define XRAM_CON 0x3340UL               /* console history */
#define XRAM_CON_LINES 32u
#define XRAM_SSH_STATE 0x3640UL         /* SSH login while it is not running */
#define XRAM_SSH 0x3690UL               /* terminal scrollback */
#define XRAM_SSH_LINES 100u
#else
#define XRAM_SIZE 0x18000UL
#define XRAM_BROWSER 0x00000UL          /* 61.5 KiB: web page (app/browser/html.h) */
#define XRAM_BROWSER_STATE 0x0FC00UL    /* url and host while it is not running */
#define XRAM_SSH 0x10000UL              /* 24 KiB: terminal scrollback */
#define XRAM_SSH_LINES 1024u
#define XRAM_CON 0x16000UL              /* 3 KiB: console history */
#define XRAM_CON_LINES 128u
#define XRAM_SSH_STATE 0x16C00UL        /* SSH login while it is not running */
#define XRAM_HIST 0x16D00UL             /* 4.75 KiB: browser history */
#define XRAM_HIST_SIZE 0x1300u
#endif
void xram_read(uint32_t addr, void *dst, uint16_t len);
void xram_write(uint32_t addr, const void *src, uint16_t len);

/* power */
extern volatile uint8_t k_on_pressed, k_on_masked;
void k_power_off(void);

#endif
