/* far.c - access to other flash/RAM pages (page 0: may switch bank A) */
#include <string.h>
#include "hw.h"
#include "kernel.h"

uint8_t k_is_emulator;

/* Copy len bytes from page:addr (addr in 4000h-7FFFh) to RAM below 8000h..FFFFh. */
void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len)
{
    uint8_t old = port_bank_a;
    port_bank_a = page;
    memcpy(dst, (const void *)addr, len);
    port_bank_a = old;
}

void k_detect_platform(void)
{
    char sig[6];
    uint8_t boot_page = (port_hw_type & 1) ? 0x7F : 0x3F;
    far_read(boot_page, 0x4010, sig, 6);
    k_is_emulator = memcmp(sig, "emu84", 6) == 0;
}
