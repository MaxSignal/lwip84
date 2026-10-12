/* netif_ncm.h - lwIP network interface on the CDC-NCM function */
#ifndef NETIF_NCM_H
#define NETIF_NCM_H
#include "lwip/err.h"
struct netif;
err_t netif_ncm_init(struct netif *netif);
void netif_ncm_poll(struct netif *netif);
#endif
