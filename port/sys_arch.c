/* sys_arch.c - lwIP system glue for the TI-84 Plus OS (NO_SYS) */
#include "lwip/arch.h"
#include "lwip/sys.h"
#include "kernel.h"

static uint32_t rand_state = 0x84840001UL;

u32_t sys_now(void)
{
    return k_now_ms();
}

u32_t sys_jiffies(void)
{
    return k_ticks;
}

void sys_init(void)
{
    rand_state ^= k_ticks;
}

/* xorshift32, seeded from the timer; good enough for ports and XIDs */
uint32_t sys_rand32(void)
{
    uint32_t x = rand_state ^ k_ticks;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rand_state = x;
    return x;
}
