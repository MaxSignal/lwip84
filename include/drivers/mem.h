#ifndef TI84P_DRIVERS_MEM_H
#define TI84P_DRIVERS_MEM_H
/* lwip-ce routes PBUF_POOL through its custom allocator; on the TI-84 Plus
 * the plain lwIP heap is used. */
#define mem_buffer_custom_malloc_pbuf(sz) mem_malloc(sz)
#define mem_buffer_custom_free_pbuf(p)    mem_free(p)
#endif
