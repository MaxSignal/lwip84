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

/* Extended RAM: pages 82h..87h (96 KiB, addresses 0..17FFFh) mapped into
 * bank A for the copy.  The buffer must be in 8000h..FFFFh. */
static void xram_copy(uint32_t addr, uint8_t *buf, uint16_t len, uint8_t write)
{
    uint8_t old = port_bank_a;
    while (len) {
        uint16_t off = (uint16_t)addr & 0x3FFF;
        uint16_t n = 0x4000 - off;
        uint8_t *p = (uint8_t *)(0x4000 + off);
        if (n > len)
            n = len;
        port_bank_a = 0x82 + (uint8_t)(addr >> 14);
        if (write)
            memcpy(p, buf, n);
        else
            memcpy(buf, p, n);
        addr += n;
        buf += n;
        len -= n;
    }
    port_bank_a = old;
}

void xram_read(uint32_t addr, void *dst, uint16_t len)
{
    xram_copy(addr, dst, len, 0);
}

void xram_write(uint32_t addr, const void *src, uint16_t len)
{
    xram_copy(addr, (uint8_t *)src, len, 1);
}
