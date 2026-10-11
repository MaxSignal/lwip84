#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
/* SDCC / Z80 (TI-84 Plus) */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define __attribute__(x)
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_INTTYPES_H 1
#define X8_F  "02x"
#define U16_F "u"
#define S16_F "d"
#define X16_F "x"
#define U32_F "lu"
#define S32_F "ld"
#define X32_F "lx"
#define SZT_F "u"
#define LWIP_PLATFORM_DIAG(x) do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { } while (0)
#define LWIP_NO_CTYPE_H 1
#define LWIP_RAND() ((u32_t)sys_rand32())
unsigned long sys_rand32(void);
#endif
