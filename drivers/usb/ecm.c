// CDC-ECM (Ethernet Control Model) USB network adapters: QEMU's usb-net, and
// real adapters that offer an ECM configuration (e.g. many RTL8153 ones).
// Frames are plain Ethernet frames on the data interface's bulk endpoints.
#include "kernel.h"
#include "net.h"
#include "usb.h"

static struct usb_dev *active;      // one adapter at a time
static uint8_t rxbuf[1600];

static int hexval(int c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; }

static int ecm_send(const void *frame, int len)
{
    if (!active)
        return -1;
    int r = usb_bulk(active, active->net_out, 0, (void *)frame, len);
    if (r == len && len % active->mps[active->net_out] == 0)
        usb_bulk(active, active->net_out, 0, NULL, 0);     // zero-length packet ends the frame
    return r == len ? 0 : -1;
}

int usb_ecm_probe(struct usb_dev *d, const uint8_t *c, int total)
{
    int comm = -1, data = -1, alt = -1, mac_idx = 0, in = 0, out = 0, cur = -1, cur_alt = 0, cls = 0;
    for (int i = 0; i + 2 <= total && c[i] >= 2; i += c[i]) {
        if (c[i + 1] == 4) {                                    // interface
            cur = c[i + 2], cur_alt = c[i + 3], cls = c[i + 5];
            if (cls == 2 && c[i + 6] == 6)
                comm = cur;
        } else if (c[i + 1] == 0x24 && c[i + 2] == 0x0f && cur == comm) {  // Ethernet functional
            mac_idx = c[i + 3];
        } else if (c[i + 1] == 5 && cls == 0x0a && (c[i + 3] & 3) == 2) {  // data bulk endpoint
            if (c[i + 2] & 0x80)
                in = c[i + 2] & 0xf;
            else
                out = c[i + 2] & 0xf;
            data = cur, alt = cur_alt;
        }
    }
    if (comm < 0 || data < 0 || !in || !out || !mac_idx)
        return 0;
    uint8_t s[32], mac[6];
    if (usb_control(d, 0x00, 9, c[5], 0, NULL, 0) < 0 ||                       // SET_CONFIGURATION
        usb_control(d, 0x01, 11, (uint16_t)alt, (uint16_t)data, NULL, 0) < 0 ||  // SET_INTERFACE
        usb_control(d, 0x80, 6, (uint16_t)(0x0300 | mac_idx), 0x0409, s, sizeof s) < 26)
        return 0;
    for (int i = 0; i < 6; i++)     // MAC as 12 UTF-16 hex digits
        mac[i] = (uint8_t)(hexval(s[2 + 4 * i]) << 4 | hexval(s[4 + 4 * i]));
    // SET_ETHERNET_PACKET_FILTER: directed, broadcast, all multicast
    usb_control(d, 0x21, 0x43, 0x0e, (uint16_t)comm, NULL, 0);
    d->net_in = in, d->net_out = out;
    active = d;
    kprintf("[USB] Ethernet (CDC-ECM) at address %d, MAC %x:%x:%x:%x:%x:%x\n", d->addr,
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    net_attach(mac, ecm_send);
    return 1;
}

int usb_ecm_poll(struct usb_dev *d)
{
    int got = 0;
    for (int i = 0; i < 32; i++) {      // drain what has arrived
        int n = usb_bulk_rx(d, d->net_in, rxbuf, sizeof rxbuf);
        if (n == USB_NAK)
            break;
        if (n > 0)
            net_input(rxbuf, n), got++;
    }
    return got;
}

void usb_ecm_gone(struct usb_dev *d)
{
    if (active == d) {
        active = NULL;
        net_detach();
    }
}
