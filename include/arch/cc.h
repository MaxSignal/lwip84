/* arch/cc.h - lwIP compiler/platform glue for SDCC on the TI-84 Plus */
#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "ti84p_libc.h"

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_NO_INTTYPES_H 1
#define LWIP_NO_CTYPE_H 1
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

uint32_t sys_rand32(void);
#endif
