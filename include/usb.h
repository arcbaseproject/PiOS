#pragma once
#include <stdint.h>

void usb_init(void);    // starts the USB thread (DWC2 host): hub, keyboard, CDC-ECM Ethernet

enum usb_kind { USB_NONE, USB_HUB, USB_KEYBOARD, USB_NET };
enum { USB_ERR = -1, USB_NAK = -2, USB_STALL = -3 };

struct usb_dev {
    enum usb_kind kind;
    int addr, low_speed, mps0;
    uint16_t mps[32];                   // per endpoint: ep | 16 for IN
    uint8_t toggle[32];
    // keyboard
    int kbd_ep;
    uint8_t prev[8], held, held_mod;
    uint64_t held_since, last_repeat;
    // CDC-ECM
    int net_in, net_out;
};

int usb_control(struct usb_dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                void *data, uint16_t len);
int usb_bulk(struct usb_dev *d, int ep, int in, void *buf, int len);    // bytes or USB_*
int usb_intr(struct usb_dev *d, int ep, void *buf, int len);
// Non-blocking bulk IN (one endpoint at a time): USB_NAK while nothing has
// arrived; the transfer stays armed between calls.
int usb_bulk_rx(struct usb_dev *d, int ep, void *buf, int len);
void usb_bulk_rx_cancel(void);

// drivers/usb/ecm.c
int usb_ecm_probe(struct usb_dev *d, const uint8_t *cfg, int total);   // 1 = configured
int usb_ecm_poll(struct usb_dev *d);    // frames received
void usb_ecm_gone(struct usb_dev *d);
