/* stdlib.h - SDCC's stdlib.h plus what the z80 library lacks */
#ifndef TI84P_STDLIB_H
#define TI84P_STDLIB_H
#include_next <stdlib.h>
#include "ti84p_libc.h"
void exit(int status);
#endif
