/* usb.h - TI-84 Plus USB controller (Mentor MUSBFDRC) in peripheral mode */
#ifndef TI84P_USB_H
#define TI84P_USB_H
#include <stdint.h>

/* ports */
__sfr __at(0x4A) port_usb_pull;
__sfr __at(0x4B) port_usb_vpwr;
__sfr __at(0x4C) port_usb_status;
__sfr __at(0x4D) port_usb_lines;
__sfr __at(0x54) port_usb_ctrl;
__sfr __at(0x55) port_usb_int;
__sfr __at(0x56) port_usb_line_ev;
__sfr __at(0x57) port_usb_line_mask;
__sfr __at(0x5B) port_usb_proto_ie;
__sfr __at(0x3A) port_gpio;
__sfr __at(0x80) port_usb_faddr;
__sfr __at(0x81) port_usb_power;
__sfr __at(0x82) port_usb_intrtx;
__sfr __at(0x84) port_usb_intrrx;
__sfr __at(0x86) port_usb_intrusb;
__sfr __at(0x87) port_usb_intrtxe;
__sfr __at(0x89) port_usb_intrrxe;
__sfr __at(0x8B) port_usb_intrusbe;
__sfr __at(0x8C) port_usb_frame;
__sfr __at(0x8E) port_usb_index;
__sfr __at(0x8F) port_usb_devctl;
__sfr __at(0x90) port_usb_txmaxp;
__sfr __at(0x91) port_usb_csr0;     /* CSR0 with index 0, TXCSR low otherwise */
__sfr __at(0x92) port_usb_txcsr2;
__sfr __at(0x93) port_usb_rxmaxp;
__sfr __at(0x94) port_usb_rxcsr;
__sfr __at(0x95) port_usb_rxcsr2;
__sfr __at(0x96) port_usb_count;
__sfr __at(0x97) port_usb_count_hi;
__sfr __at(0x98) port_usb_txtype;
__sfr __at(0x9A) port_usb_rxtype;

#define USB_FIFO(ep) (0xA0 + (ep))

/* CSR0, peripheral mode */
#define CSR0_RXPKTRDY    0x01
#define CSR0_TXPKTRDY    0x02
#define CSR0_SENTSTALL   0x04
#define CSR0_DATAEND     0x08
#define CSR0_SETUPEND    0x10
#define CSR0_SENDSTALL   0x20
#define CSR0_SVDRXPKTRDY 0x40
#define CSR0_SVDSETUPEND 0x80
/* TXCSR low */
#define TXCSR_TXPKTRDY   0x01
#define TXCSR_FIFONE     0x02
#define TXCSR_UNDERRUN   0x04
#define TXCSR_FLUSH      0x08
#define TXCSR_SENDSTALL  0x10
#define TXCSR_SENTSTALL  0x20
#define TXCSR_CLRDATATOG 0x40
/* RXCSR low */
#define RXCSR_RXPKTRDY   0x01
#define RXCSR_FLUSH      0x10
#define RXCSR_SENDSTALL  0x20
#define RXCSR_SENTSTALL  0x40
#define RXCSR_CLRDATATOG 0x80

void usb_fifo_out(const uint8_t *buf, uint16_t count_port);
void usb_fifo_in(uint8_t *buf, uint16_t count_port);
#define USB_WRITE(ep, buf, n) usb_fifo_out((buf), ((uint16_t)(n) << 8) | USB_FIFO(ep))
#define USB_READ(ep, buf, n)  usb_fifo_in((buf), ((uint16_t)(n) << 8) | USB_FIFO(ep))

/* events latched by the interrupt handler (reading 82h/84h/86h clears them) */
extern volatile uint8_t usb_ev_tx, usb_ev_rx, usb_ev_bus, usb_ev_line;
void usb_isr(void);

/* device state */
enum { USB_OFF, USB_WAIT_HOST, USB_DEFAULT, USB_ADDRESS, USB_CONFIGURED };
extern uint8_t usb_state;
extern uint8_t usb_alt;             /* NCM data interface alternate setting */
extern uint8_t usb_last_error;

void usb_start(void);
void usb_stop(void);
void usb_poll(void);

/* endpoints used by the NCM function */
#define EP_BULK      1              /* 0x81 bulk IN, 0x01 bulk OUT */
#define EP_NOTIFY    2              /* 0x82 interrupt IN */
#define EP_BULK_SIZE 64

/* ncm.c: called by usb.c */
void ncm_reset(void);
void ncm_enable(uint8_t on);
uint8_t ncm_class_request(const uint8_t *setup, uint8_t **reply, uint16_t *len);
uint8_t ncm_class_out(const uint8_t *setup, const uint8_t *data, uint16_t len);
void ncm_poll(void);
extern const uint8_t ncm_host_mac[6];
#endif
