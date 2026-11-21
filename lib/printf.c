// Minimal kernel printf: %s %c %d %u %x %p %%, optional 'l' modifier.
#include <stdarg.h>
#include "kernel.h"
#include "uart.h"

static void put_num(uint64_t v, unsigned base, int neg)
{
    char buf[24];
    int i = 0;
    do {
        buf[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    if (neg)
        uart_putc('-');
    while (i)
        uart_putc(buf[--i]);
}

static void vkprintf(const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            if (*fmt == '\n')
                uart_putc('\r');
            uart_putc(*fmt);
            continue;
        }
        int lng = 0;
        if (*++fmt == 'l') {
            lng = 1;
            fmt++;
        }
        switch (*fmt) {
        case 's': { const char *s = va_arg(ap, const char *); uart_puts(s ? s : "(null)"); break; }
        case 'c': uart_putc((char)va_arg(ap, int)); break;
        case 'd': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(v < 0 ? -(uint64_t)v : (uint64_t)v, 10, v < 0);
            break;
        }
        case 'u': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, 0); break;
        case 'x': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 0); break;
        case 'p': uart_puts("0x"); put_num((uintptr_t)va_arg(ap, void *), 16, 0); break;
        case '%': uart_putc('%'); break;
        case '\0': return;
        default: uart_putc('%'); uart_putc(*fmt); break;
        }
    }
}

void kprintf(const char *fmt, ...)
{
    uint64_t daif = read_sysreg(daif);
    irq_disable();      // keep a line whole when threads are preempted
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
    write_sysreg(daif, daif);
}

_Noreturn void panic(const char *fmt, ...)
{
    irq_disable();
    va_list ap;
    va_start(ap, fmt);
    uart_puts("\n[PANIC] ");
    vkprintf(fmt, ap);
    va_end(ap);
    uart_puts("\n[PANIC] System halted.\n");
    for (;;)
        __asm__ volatile("wfe");
}
