/* lwip_start84.c - lwIP-CE's start entry points on the TI-84 Plus.  On the CE
 * lwip_start() is the libload bootstrap; here the stack is linked into the
 * OS and lwip_start() just initializes it. */
#include <stdbool.h>
#include <stdint.h>
#include "lwIP.h"

bool lwip_stack_init(void);

bool lwip_start(void)
{
    return lwip_stack_init();
}

uint8_t lwip_get_start_errno(void)
{
    return lwip_start_last_error();
}
