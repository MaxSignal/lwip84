/* ui.c - full-screen layout for applications */
#include <string.h>
#include "kernel.h"
#include "ui.h"

void ui_clear(void)
{
    lcd_clear();
}

void ui_status(const char *s)
{
    memset(lcd_buf, 0, 7 * 12);
    lcd_draw_text(1, 1, s);
    lcd_invert_rect(0, 0, 96, 7);
}

/* characters with bit 7 set are drawn inverted */
void ui_text(uint8_t row, const char *s)
{
    uint8_t y = 8 + row * 6, x = 0;
    memset(lcd_buf + (uint16_t)y * 12, 0, 6 * 12);
    for (; *s && x < 96; s++, x += 4) {
        lcd_draw_char(x, y, *s & 0x7F);
        if (*s & 0x80)
            lcd_invert_rect(x, y, 4, 6);
    }
}
