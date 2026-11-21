# Cross build with clang + host multi-target GNU binutils (Void: clang, binutils).
.DEFAULT_GOAL := all
CC      := clang --target=aarch64-none-elf
LD      := ld -m aarch64elf
OBJCOPY := objcopy

CFLAGS  := -std=c17 -ffreestanding -fno-builtin -nostdlib -mcpu=cortex-a53 \
           -mgeneral-regs-only -mstrict-align -O2 -g -Wall -Wextra -Werror -Iinclude -Iinclude/uapi -MMD -MP
ASFLAGS := -mcpu=cortex-a53 -Iinclude -Iinclude/uapi -MMD -MP

ifdef STRACE
CFLAGS += -DSTRACE
endif

SRCS := arch/aarch64/boot/start.S arch/aarch64/exception/vectors.S \
        arch/aarch64/exception/exception.c arch/aarch64/mmu/mmu.c \
        kernel/main.c drivers/uart/pl011.c drivers/timer/timer.c \
        drivers/mailbox/mailbox.c mm/page.c lib/printf.c lib/string.c \
        arch/aarch64/cpu/switch.S kernel/sched/sched.c mm/vm.c \
        kernel/process/proc.c kernel/process/elf.c mm/kmalloc.c \
        fs/vfs/vfs.c fs/vfs/sysfile.c fs/vfs/pipe.c fs/ramfs/ramfs.c fs/devfs/devfs.c \
        kernel/process/initrd.S kernel/syscall/syscall.c \
        drivers/sd/emmc.c fs/fat/fat32.c drivers/fb/fbcon.c drivers/fb/font.c \
        drivers/usb/dwc2.c drivers/usb/ecm.c net/net.c net/socket.c
# lwIP 2.2.0 (BSD, net/lwip/COPYING): IPv4, TCP, UDP, DHCP, DNS, ARP.
LWIP_SRCS := $(wildcard net/lwip/core/*.c net/lwip/core/ipv4/*.c net/lwip/netif/*.c)
SRCS += $(LWIP_SRCS)
OBJS := $(patsubst %,build/%.o,$(SRCS))
NET_INC := -nostdlibinc -Inet/port -Inet/lwip/include
build/net/%: CFLAGS += $(NET_INC)
build/drivers/usb/%: CFLAGS += $(NET_INC)
build/net/lwip/%: CFLAGS := $(filter-out -Werror,$(CFLAGS)) $(NET_INC) -Wno-unused-parameter

# Userland: static ELFs linked at USER_BASE, embedded via kernel/process/initrd.S.
UTESTS := hello segv spin fpu
UBINS  := sh ls pwd cat echo mkdir rm cp mv ps kill uname clear sleep uptime ed edit
UPROGS := init $(UTESTS) $(UBINS) lua
ULIB   := build/userland/lib/crt0.S.o build/userland/lib/syscall.S.o build/userland/lib/ulib.c.o \
          build/userland/lib/malloc.c.o build/userland/lib/printf.c.o
UELFS  := $(patsubst %,build/user/%.elf,$(UPROGS))
UCFLAGS := $(subst -mgeneral-regs-only ,,$(subst -Iinclude ,,$(CFLAGS))) -Iuserland/lib
build/userland/%: CFLAGS := $(UCFLAGS)

# Standard C library subset for ported programs (an archive: programs pull only what they use).
LIBC_OBJS := $(patsubst %,build/%.o,$(wildcard userland/libc/*.c userland/libc/*.S))
LIBC      := build/userland/libc.a
build/userland/libc/%: CFLAGS := $(UCFLAGS) -nostdlibinc -Iuserland/libc/include
build/userland/libc/%: ASFLAGS += -Iuserland/libc/include
$(LIBC): $(LIBC_OBJS)
	rm -f $@
	ar rcs $@ $^

# Lua 5.4.7, unmodified upstream sources (userland/lua, MIT license in lua.h).
LUA_OBJS := $(patsubst %,build/%.o,$(wildcard userland/lua/*.c))
build/userland/lua/%: CFLAGS := $(filter-out -Werror -Iuserland/lib,$(UCFLAGS)) \
                                -nostdlibinc -Iuserland/libc/include -DLUA_USE_C89
build/userland/%: ASFLAGS := -mcpu=cortex-a53 -Iinclude/uapi -MMD -MP

all: kernel8.img

build/user/init.elf: build/userland/init/init.c.o
$(UTESTS:%=build/user/%.elf): build/user/%.elf: build/userland/tests/%.c.o
$(UBINS:%=build/user/%.elf): build/user/%.elf: build/userland/bin/%.c.o
build/user/lua.elf: $(LUA_OBJS) $(LIBC)
$(UELFS): $(ULIB) userland/user.ld
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -S -z max-page-size=4096 -T userland/user.ld -o $@ $(filter %.o,$^) $(filter %.a,$^)

build/kernel/process/initrd.S.o: $(UELFS)

kernel8.img: build/kernel8.elf
	$(OBJCOPY) -O binary $< $@

build/kernel8.elf: $(OBJS) linker.ld
	$(LD) -nostdlib -T linker.ld -o $@ $(OBJS)

build/%.c.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.S.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# QEMU raspi3b: same BCM2837 peripherals, NOT proof of Pi Zero 2 W boot.
qemu: kernel8.img
	qemu-system-aarch64 -M raspi3b -kernel kernel8.img -serial stdio -display none

clean:
	rm -rf build kernel8.img

-include $(OBJS:.o=.d) $(ULIB:.o=.d) $(UTESTS:%=build/userland/tests/%.c.d) $(UBINS:%=build/userland/bin/%.c.d) build/userland/init/init.c.d \
         $(LIBC_OBJS:.o=.d) $(LUA_OBJS:.o=.d)
.PHONY: all qemu clean
