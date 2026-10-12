/* time84.c - wall clock for the TI-84 Plus.  The 84+ ASIC has a
 * free-running seconds counter (ports 45h-48h); SNTP stores the offset
 * between it and Unix time. */
#include <stdint.h>
#include "time84.h"

__sfr __at(0x45) port_rtc0;
__sfr __at(0x46) port_rtc1;
__sfr __at(0x47) port_rtc2;
__sfr __at(0x48) port_rtc3;

static uint8_t time_set;
static uint32_t unix_offset;

static uint32_t rtc_raw(void)
{
    uint32_t a, b;
    do {
        a = port_rtc0 | ((uint32_t)port_rtc1 << 8) | ((uint32_t)port_rtc2 << 16) |
            ((uint32_t)port_rtc3 << 24);
        b = port_rtc0 | ((uint32_t)port_rtc1 << 8) | ((uint32_t)port_rtc2 << 16) |
            ((uint32_t)port_rtc3 << 24);
    } while (a != b);
    return a;
}

void time84_set(uint32_t unix_seconds)
{
    unix_offset = unix_seconds - rtc_raw();
    time_set = 1;
}

uint8_t time84_valid(void)
{
    return time_set;
}

uint32_t time84_now(void)
{
    return rtc_raw() + unix_offset;
}
