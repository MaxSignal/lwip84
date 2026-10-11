/* fmt.c - small vsnprintf: %d %i %u %x %X %c %s %p %%, l modifier, width, '0' and '-' flags */
#include "kernel.h"
#ifdef DEBUG
#include "hw.h"
#endif

struct out {
    char *buf;
    size_t n, len;
};

static void put(struct out *o, char c)
{
    if (o->len + 1 < o->n)
        o->buf[o->len] = c;
    o->len++;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    struct out o;
    char tmp[12];
    o.buf = buf;
    o.n = n;
    o.len = 0;
    while (*fmt) {
        char c = *fmt++;
        uint8_t width = 0, zero = 0, left = 0, lng = 0, i = 0, neg = 0;
        uint32_t v;
        const char *s;
        if (c != '%') {
            put(&o, c);
            continue;
        }
        while (*fmt == '0' || *fmt == '-') {
            if (*fmt == '0')
                zero = 1;
            else
                left = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z') {
            if (*fmt == 'l')
                lng = 1;
            fmt++;
        }
        c = *fmt++;
        switch (c) {
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            tmp[1] = 0;
            s = tmp;
            goto str;
        case 's':
            s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
        str:
            for (i = 0; s[i]; i++)
                ;
            if (!left)
                while (width > i) {
                    put(&o, ' ');
                    width--;
                }
            while (*s)
                put(&o, *s++);
            while (width > i) {
                put(&o, ' ');
                width--;
            }
            break;
        case 'd':
        case 'i':
            if (lng) {
                int32_t x = va_arg(ap, int32_t);
                if (x < 0) {
                    neg = 1;
                    x = -x;
                }
                v = (uint32_t)x;
            } else {
                int x = va_arg(ap, int);
                if (x < 0) {
                    neg = 1;
                    x = -x;
                }
                v = (uint16_t)x;
            }
            goto num10;
        case 'u':
            v = lng ? va_arg(ap, uint32_t) : (uint16_t)va_arg(ap, unsigned);
        num10:
            do {
                tmp[i++] = '0' + (uint8_t)(v % 10);
                v /= 10;
            } while (v);
            goto emit;
        case 'p':
            lng = 0;
            zero = 1;
            width = 4;
            /* fall through */
        case 'x':
        case 'X':
            v = lng ? va_arg(ap, uint32_t) : (uint16_t)va_arg(ap, unsigned);
            do {
                uint8_t d = v & 15;
                tmp[i++] = d < 10 ? '0' + d : (c == 'X' ? 'A' : 'a') + d - 10;
                v >>= 4;
            } while (v);
        emit:
            if (neg)
                tmp[i++] = '-';
            if (!left)
                while (width > i) {
                    put(&o, zero ? '0' : ' ');
                    width--;
                }
            {
                uint8_t k = i;
                while (k)
                    put(&o, tmp[--k]);
            }
            while (width > i) {
                put(&o, ' ');
                width--;
            }
            break;
        case '%':
            put(&o, '%');
            break;
        default:
            put(&o, '%');
            put(&o, c);
            break;
        }
    }
    if (n)
        buf[o.len < n ? o.len : n - 1] = 0;
    return (int)o.len;
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    int r;
    va_list ap;
    va_start(ap, fmt);
    r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

void dbg_printf(const char *fmt, ...)
{
#ifdef DEBUG
    char buf[128];
    const char *p = buf;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    while (*p)
        port_emu_debug = *p++;
#else
    (void)fmt;
#endif
}

/* exit: used by lwIP-CE's fatal error path; there is nothing to return to */
void exit(int status)
{
    con_printf("\nFATAL %d - press ON\n", status);
    con_flush();
    dbg_printf("exit(%d)\n", status);
    __asm__("di");
    for (;;)
        __asm__("halt");
}
