#pragma once
void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_puthex(unsigned long v);
void uart_enable_rx_irq(void);
int uart_irq_pending(void);
void uart_irq(void);
