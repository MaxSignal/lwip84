/* sntp_time.c - wall clock for the TI-84 Plus port.
 * The 84+ ASIC has a free-running seconds counter (ports 45h-48h); SNTP
 * stores the offset between it and Unix time. */
#include <stdbool.h>
#include <stdint.h>
#include "lwip/sntp_time.h"

__sfr __at(0x45) port_rtc0;
__sfr __at(0x46) port_rtc1;
__sfr __at(0x47) port_rtc2;
__sfr __at(0x48) port_rtc3;

volatile bool g_sntp_time_set = false;
static uint32_t unix_offset;

uint32_t lwip_sntp_read_rtc_raw(void)
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

void lwip_sntp_reset_flag(void)
{
    g_sntp_time_set = false;
}

bool lwip_sntp_time_was_set(void)
{
    return g_sntp_time_set;
}

void lwip_sntp_set_time(uint32_t seconds)
{
    unix_offset = seconds - lwip_sntp_read_rtc_raw();
    g_sntp_time_set = true;
}

uint32_t lwip_sntp_get_unix_time(void)
{
    return lwip_sntp_read_rtc_raw() + unix_offset;
}

void lwip_sntp_clamp_rtc_floor(uint32_t floor_unix_seconds)
{
    if (lwip_sntp_get_unix_time() < floor_unix_seconds)
        unix_offset = floor_unix_seconds - lwip_sntp_read_rtc_raw();
}
