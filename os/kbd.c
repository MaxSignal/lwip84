/* kbd.c - keypad matrix scanning */
#include "hw.h"
#include "kernel.h"

/* key codes by group (port 01h select bit) and bit */
static const uint8_t keytab[7][8] = {
    {K_DOWN, K_LEFT, K_RIGHT, K_UP, 0, 0, 0, 0},
    {K_ENTER, K_ADD, K_SUB, K_MUL, K_DIV, K_POW, K_CLEAR, 0},
    {K_NEG, K_3, K_6, K_9, K_RPAREN, K_TAN, K_VARS, 0},
    {K_DOT, K_2, K_5, K_8, K_LPAREN, K_COS, K_PRGM, K_STAT},
    {K_0, K_1, K_4, K_7, K_COMMA, K_SIN, K_APPS, K_XT},
    {0, K_STO, K_LN, K_LOG, K_SQR, K_INV, K_MATH, K_ALPHA},
    {K_GRAPH, K_TRACE, K_ZOOM, K_WINDOW, K_YEQU, K_2ND, K_MODE, K_DEL},
};

static void key_settle(void) __naked
{
    __asm
        push    bc
        ld      b, #20
    00001$:
        djnz    00001$
        pop     bc
        ret
    __endasm;
}

uint8_t kbd_scan(void)
{
    uint8_t g, b, v, key = K_NONE;
    if (!(port_int_id & 0x08))
        return K_ON;
    for (g = 0; g < 7 && key == K_NONE; g++) {
        port_keypad = 0xFF;
        key_settle();
        port_keypad = ~(1 << g);
        key_settle();
        v = port_keypad;
        for (b = 0; b < 8; b++)
            if (!(v & (1 << b)) && keytab[g][b]) {
                key = keytab[g][b];
                break;
            }
    }
    port_keypad = 0xFF;
    return key;
}

static uint8_t last_key;
static uint32_t next_repeat;

uint8_t kbd_get(void)
{
    uint8_t k = kbd_scan();
    uint32_t now = k_now_ms();
    if (k == K_NONE) {
        last_key = K_NONE;
        return K_NONE;
    }
    if (k != last_key) {
        last_key = k;
        next_repeat = now + 400;
        return k;
    }
    if ((int32_t)(now - next_repeat) >= 0 &&
        (k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT || k == K_DEL)) {
        next_repeat = now + 80;
        return k;
    }
    return K_NONE;
}
