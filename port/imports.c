/* imports.c - lwIP-CE's import table (libload on the CE) for the TI-84 Plus.
 * There is no file system: the configuration lives in RAM only. */
#include <stddef.h>
#include <stdint.h>
#include "ti84p_libc.h"
#include "../../src/lwip-imports.h"

static uint8_t no_open(const char *name, const char *mode)
{
    (void)name;
    (void)mode;
    return 0;
}

static int no_delete(const char *name)
{
    (void)name;
    return 0;
}

struct lwip_imports fn_imports_table = {
    malloc,
    free,
    realloc,
    {no_open, NULL, NULL, NULL, NULL, NULL, NULL, NULL, no_delete, NULL},
};

void lwip_fileio_self_init(void)
{
}
