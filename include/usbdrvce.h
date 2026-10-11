/* usbdrvce.h - stand-in for the TI-84 Plus CE USB driver header.  lwIP-CE
 * refers to a few of its types; on the TI-84 Plus the USB device side is
 * os/usb.c and these are placeholders. */
#ifndef TI84P_USBDRVCE_H
#define TI84P_USBDRVCE_H
#include <stdint.h>
typedef int usb_error_t;
typedef int usb_event_t;
typedef void usb_callback_data_t;
typedef void *usb_device_t;
#define USB_SUCCESS 0
#define USB_ERROR_FAILED 1
#define USB_DEFAULT_INIT_FLAGS 0u
#endif
