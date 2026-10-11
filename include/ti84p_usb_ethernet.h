/* drivers/usb_ethernet.h - TI-84 Plus version of lwIP-CE's USB Ethernet
 * driver interface.  The calculator is a CDC-NCM *device* here (os/ncm.c);
 * port/usb_ethernet84.c provides the entry points lwIP.c expects. */
#ifndef TI84P_USB_ETHERNET_H
#define TI84P_USB_ETHERNET_H
#include <stdbool.h>
#include <stdint.h>
#include "usbdrvce.h"
#include "lwip/err.h"
struct netif;

/* used by port/usb_ethernet84.c */
err_t netif_ncm_init(struct netif *netif);
void netif_ncm_poll(struct netif *netif);

/* lwIP-CE driver API */
usb_error_t eth_usb_event_callback(usb_event_t event, void *event_data,
                                   usb_callback_data_t *callback_data);
void lwip_usb_callback_chain_clear(void);
void eth_halt_all_endpoints(void);
void eth_prepare_shutdown(void);
void eth_finish_shutdown(void);
void eth_reset_shutdown(void);
uint32_t eth_last_rx_activity_ms(void);
uint8_t eth_get_interfaces(void);

/* replaces usbdrvce's function table (usb_fn) */
typedef usb_error_t (*ti84p_usb_event_cb_t)(usb_event_t, void *, usb_callback_data_t *);
struct ti84p_usb_fn {
    usb_error_t (*init)(ti84p_usb_event_cb_t cb, const void *dev_desc, const void *conf_desc,
                        uint32_t flags);
    usb_error_t (*handle_events)(void);
    void (*cleanup)(void);
};
extern const struct ti84p_usb_fn ti84p_usb_fn;
#define usb_fn ti84p_usb_fn

/* the network interface (exported for the OS status screen) */
extern struct netif *eth_ncm_netif;
#endif
