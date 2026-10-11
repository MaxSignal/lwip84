/* netif_ncm.c - lwIP network interface on top of the CDC-NCM function */
#include <string.h>
#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "netif/ethernet.h"
#include "ti84p_usb_ethernet.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"

static err_t ncm_linkoutput(struct netif *netif, struct pbuf *p)
{
    uint8_t *buf;
    uint32_t start = k_now_ms();
    (void)netif;
    if (p->tot_len > 1514)
        return ERR_VAL;
    /* one NTB in flight at a time: push the previous one out first */
    while (!(buf = ncm_tx_buffer())) {
        if (!ncm_link_up() || (uint32_t)(k_now_ms() - start) > 200)
            return ERR_IF;
        usb_poll();
    }
    pbuf_copy_partial(p, buf, p->tot_len, 0);
    ncm_tx_send(p->tot_len);
    usb_poll();
    return ERR_OK;
}

err_t netif_ncm_init(struct netif *netif)
{
    netif->name[0] = 'u';
    netif->name[1] = 's';
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, ncm_dev_mac, 6);
    netif->mtu = 1500;
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET;
    netif->output = etharp_output;
    netif->linkoutput = ncm_linkoutput;
    return ERR_OK;
}

/* Feed received frames to lwIP and track the USB link state. */
void netif_ncm_poll(struct netif *netif)
{
    uint8_t *frame;
    uint16_t len;
    uint8_t n = 0;
    uint8_t up = ncm_link_up();
    if (up && !netif_is_link_up(netif))
        netif_set_link_up(netif);
    else if (!up && netif_is_link_up(netif))
        netif_set_link_down(netif);
    while (n < 8 && ncm_rx_next(&frame, &len)) {
        struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
        n++;
        if (!p)
            continue;
        pbuf_take(p, frame, len);
        if (netif->input(p, netif) != ERR_OK)
            pbuf_free(p);
    }
}
