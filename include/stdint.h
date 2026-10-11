/* stdint.h - SDCC's stdint.h plus the eZ80 24-bit types lwIP-CE uses */
#ifndef TI84P_STDINT_H
#define TI84P_STDINT_H
#include_next <stdint.h>
typedef uint32_t uint24_t;
typedef int32_t int24_t;
#endif
