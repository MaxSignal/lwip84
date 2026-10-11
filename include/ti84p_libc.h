/* ti84p_libc.h - libc pieces missing from SDCC's z80 library (os/fmt.c, os/heap.c) */
#ifndef TI84P_LIBC_H
#define TI84P_LIBC_H
#include <stddef.h>
#include <stdarg.h>
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *calloc(size_t n, size_t size);
size_t heap_free_bytes(void);
size_t heap_largest_block(void);
#endif
