#pragma once
#include <stddef.h>
#include <stdint.h>

void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

#define read_sysreg(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " #r : "=r"(_v)); _v; })
#define write_sysreg(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((uint64_t)(v)))
#define irq_enable()  __asm__ volatile("msr daifclr, #2" ::: "memory")
#define irq_disable() __asm__ volatile("msr daifset, #2" ::: "memory")
