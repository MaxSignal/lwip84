/* ncm.h - CDC-NCM function: interface for the network driver */
#ifndef TI84P_NCM_H
#define TI84P_NCM_H
#include <stdint.h>

extern const uint8_t ncm_dev_mac[6];   /* MAC address of the calculator */

struct ncm_stats {
    uint16_t rx_ntbs, rx_frames, rx_bad, tx_frames;
};
extern struct ncm_stats ncm_stats;

uint8_t ncm_link_up(void);
/* next received Ethernet frame (valid until the next call), 0 if none */
uint8_t ncm_rx_next(uint8_t **frame, uint16_t *len);
/* transmit: get the frame buffer (NULL while the previous NTB is in flight),
 * fill it and send */
uint8_t *ncm_tx_buffer(void);
void ncm_tx_send(uint16_t len);
uint8_t ncm_tx_idle(void);
#endif
