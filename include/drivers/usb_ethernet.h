/* drivers/usb_ethernet.h - TI-84 Plus replacement for lwIP-CE's usbdrvce
 * Ethernet driver header.  The network interface is os/ncm.c + port/netif_ncm.c. */
#ifndef TI84P_USB_ETHERNET_H
#define TI84P_USB_ETHERNET_H
#include "lwip/err.h"
struct netif;
err_t netif_ncm_init(struct netif *netif);
void netif_ncm_poll(struct netif *netif);
#endif
