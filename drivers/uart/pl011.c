// PL011 UART0 on GPIO14 (TXD) / GPIO15 (RXD), 115200 8N1.
// Requires config.txt: dtoverlay=disable-bt (frees PL011 from Bluetooth)
// and init_uart_clock=48000000 (fixes UARTCLK the divisor below assumes).
#include "mmio.h"
#include "uart.h"
#include "vfs.h"

#define GPIO_BASE   (PERIPHERAL_BASE + 0x200000)
#define GPFSEL1     (GPIO_BASE + 0x04)
#define GPPUD       (GPIO_BASE + 0x94)
#define GPPUDCLK0   (GPIO_BASE + 0x98)

#define UART0_BASE  (PERIPHERAL_BASE + 0x201000)
#define UART_DR     (UART0_BASE + 0x00)
#define UART_FR     (UART0_BASE + 0x18)
#define UART_IBRD   (UART0_BASE + 0x24)
#define UART_FBRD   (UART0_BASE + 0x28)
#define UART_LCRH   (UART0_BASE + 0x2C)
#define UART_CR     (UART0_BASE + 0x30)
#define UART_IMSC   (UART0_BASE + 0x38)
#define UART_ICR    (UART0_BASE + 0x44)
#define INT_RX      (1u << 4)
#define INT_RT      (1u << 6)   // receive timeout: FIFO has data below trigger level

// BCM2835 ARM interrupt controller. UART0 is GPU IRQ 57 = bit 25 of bank 2.
#define IRQ_ENABLE2 (PERIPHERAL_BASE + 0xB214)
#define UART_IRQ_BIT (1u << 25)

#define FR_RXFE     (1u << 4)
#define FR_TXFF     (1u << 5)

void uart_init(void)
{
    mmio_write(UART_CR, 0);                 // disable while configuring

    // GPIO14/15 -> ALT0 (TXD0/RXD0): FSEL fields at bits 12-14 and 15-17 of GPFSEL1.
    uint32_t sel = mmio_read(GPFSEL1);
    sel &= ~((7u << 12) | (7u << 15));
    sel |= (4u << 12) | (4u << 15);
    mmio_write(GPFSEL1, sel);

    // Disable pull-up/down on 14/15. BCM2837 sequence: set GPPUD, wait 150
    // cycles, clock it into the pins, wait, then clear both.
    mmio_write(GPPUD, 0);
    delay_cycles(150);
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    delay_cycles(150);
    mmio_write(GPPUDCLK0, 0);

    mmio_write(UART_ICR, 0x7FF);            // clear pending interrupts
    // 48 MHz / (16 * 115200) = 26.0417 -> IBRD 26, FBRD round(0.0417 * 64) = 3
    mmio_write(UART_IBRD, 26);
    mmio_write(UART_FBRD, 3);
    mmio_write(UART_LCRH, (3u << 5) | (1u << 4));   // 8 bits, FIFO on
    mmio_write(UART_CR, (1u << 0) | (1u << 8) | (1u << 9)); // UARTEN, TXE, RXE
}

void uart_putc(char c)
{
    while (mmio_read(UART_FR) & FR_TXFF)
        ;
    mmio_write(UART_DR, (uint32_t)c);
}

void uart_puts(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s);
    }
}

void uart_puthex(unsigned long v)
{
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        uart_putc("0123456789abcdef"[(v >> i) & 0xf]);
}

void uart_enable_rx_irq(void)
{
    mmio_write(UART_IMSC, INT_RX | INT_RT);
    mmio_write(IRQ_ENABLE2, UART_IRQ_BIT);
}

int uart_irq_pending(void)
{
    return (mmio_read(PERIPHERAL_BASE + 0xB208) & UART_IRQ_BIT) != 0;   // IRQ pending 2
}

void uart_irq(void)
{
    while (!(mmio_read(UART_FR) & FR_RXFE))
        console_input((char)(mmio_read(UART_DR) & 0xff));
    mmio_write(UART_ICR, INT_RX | INT_RT);
}
