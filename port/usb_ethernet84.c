/* usb_ethernet84.c - lwIP-CE's USB Ethernet driver entry points on top of the
 * TI-84 Plus CDC-NCM device function.  Mirrors the CE driver's behaviour:
 * DHCP is started once when the link comes up, and the interface becomes
 * the default route as soon as it has an address. */
#include <string.h>
#include "lwip/opt.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/teardown.h"
#include "netif/ethernet.h"
#include "ti84p_usb_ethernet.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"

static struct netif ncm_if;
struct netif *eth_ncm_netif;
static bool dhcp_auto_started;
static bool shutting_down;
static uint32_t last_rx_ms;

static void ncm_link_cb(struct netif *netif)
{
    if (shutting_down)
        return;
    if (netif_is_link_up(netif)) {
        if (!dhcp_auto_started && ip4_addr_isany_val(*netif_ip4_addr(netif))) {
            if (dhcp_start(netif) == ERR_OK)
                dhcp_auto_started = true;
        }
    } else {
        lwip_teardown_abort_pcbs_on_netif(netif);
        if (dhcp_auto_started) {
            dhcp_stop(netif);
            dhcp_auto_started = false;
        }
    }
}

static void ncm_status_cb(struct netif *netif)
{
    if (shutting_down)
        return;
    if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)))
        netif_set_default(netif);
}

static usb_error_t ncm_usb_init(ti84p_usb_event_cb_t cb, const void *dev_desc,
                                const void *conf_desc, uint32_t flags)
{
    (void)cb;
    (void)dev_desc;
    (void)conf_desc;
    (void)flags;
    shutting_down = false;
    dhcp_auto_started = false;
    if (!netif_add(&ncm_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL,
                   netif_ncm_init, ethernet_input))
        return USB_ERROR_FAILED;
    eth_ncm_netif = &ncm_if;
    netif_set_link_callback(&ncm_if, ncm_link_cb);
    netif_set_status_callback(&ncm_if, ncm_status_cb);
    netif_set_up(&ncm_if);
    usb_start();
    return USB_SUCCESS;
}

static usb_error_t ncm_usb_handle_events(void)
{
    uint16_t before = ncm_stats.rx_frames;
    usb_poll();
    if (eth_ncm_netif)
        netif_ncm_poll(eth_ncm_netif);
    if (ncm_stats.rx_frames != before)
        last_rx_ms = k_now_ms();
    return USB_SUCCESS;
}

static void ncm_usb_cleanup(void)
{
    usb_stop();
}

const struct ti84p_usb_fn ti84p_usb_fn = {
    ncm_usb_init,
    ncm_usb_handle_events,
    ncm_usb_cleanup,
};

usb_error_t eth_usb_event_callback(usb_event_t event, void *event_data,
                                   usb_callback_data_t *callback_data)
{
    (void)event;
    (void)event_data;
    (void)callback_data;
    return USB_SUCCESS;
}

void lwip_usb_callback_chain_clear(void) {}
void eth_halt_all_endpoints(void) {}

void eth_prepare_shutdown(void)
{
    shutting_down = true;
}

void eth_finish_shutdown(void)
{
    if (eth_ncm_netif) {
        netif_remove(eth_ncm_netif);
        eth_ncm_netif = NULL;
    }
}

void eth_reset_shutdown(void)
{
    shutting_down = false;
}

uint32_t eth_last_rx_activity_ms(void)
{
    return last_rx_ms;
}

uint8_t eth_get_interfaces(void)
{
    return eth_ncm_netif ? 1 : 0;
}
