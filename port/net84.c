/* net84.c - starts lwIP on the CDC-NCM interface and runs it from the main
 * loop.  DHCP starts when the USB link comes up; the interface becomes the
 * default route as soon as it has an address. */
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
#include "kernel.h"
#include "usb.h"
#include "ncm.h"
#include "net84.h"
#include "netif_ncm.h"
#include "sock84.h"

static struct netif ncm_if;
struct netif *net84_netif;
static uint8_t dhcp_on;

static void link_cb(struct netif *netif)
{
    if (netif_is_link_up(netif)) {
        if (!dhcp_on && dhcp_start(netif) == ERR_OK)
            dhcp_on = 1;
    } else if (dhcp_on) {
        dhcp_stop(netif);
        dhcp_on = 0;
    }
}

static void status_cb(struct netif *netif)
{
    if (netif_is_up(netif) && !ip4_addr_isany_val(*netif_ip4_addr(netif)))
        netif_set_default(netif);
}

void net84_init(void)
{
    lwip_init();
    if (!netif_add(&ncm_if, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, NULL, netif_ncm_init,
                   ethernet_input))
        return;
    netif_set_hostname(&ncm_if, "ti84plus");
    net84_netif = &ncm_if;
    netif_set_link_callback(&ncm_if, link_cb);
    netif_set_status_callback(&ncm_if, status_cb);
    netif_set_up(&ncm_if);
    usb_start();
}

void net84_poll(void)
{
    usb_watchdog();
    usb_poll();
    if (net84_netif)
        netif_ncm_poll(net84_netif);
    sys_check_timeouts();
    sock_poll();
}
