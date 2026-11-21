# Cross build with clang + host multi-target GNU binutils (Void: clang, binutils).
CC      := clang --target=aarch64-none-elf
LD      := ld -m aarch64elf
OBJCOPY := objcopy

CFLAGS  := -std=c17 -ffreestanding -fno-builtin -nostdlib -mcpu=cortex-a53 \
           -mgeneral-regs-only -mstrict-align -O2 -g -Wall -Wextra -Werror -Iinclude -Iinclude/uapi -MMD -MP
ASFLAGS := -mcpu=cortex-a53 -Iinclude -Iinclude/uapi -MMD -MP

SRCS := arch/aarch64/boot/start.S arch/aarch64/exception/vectors.S \
        arch/aarch64/exception/exception.c arch/aarch64/mmu/mmu.c \
        kernel/main.c drivers/uart/pl011.c drivers/timer/timer.c \
        drivers/mailbox/mailbox.c mm/page.c lib/printf.c lib/string.c \
        arch/aarch64/cpu/switch.S kernel/sched/sched.c mm/vm.c \
        kernel/process/proc.c kernel/process/elf.c mm/kmalloc.c \
        fs/vfs/vfs.c fs/vfs/sysfile.c fs/vfs/pipe.c fs/ramfs/ramfs.c fs/devfs/devfs.c \
        kernel/process/initrd.S kernel/syscall/syscall.c \
        drivers/sd/emmc.c fs/fat/fat32.c
OBJS := $(patsubst %,build/%.o,$(SRCS))

# Userland: static ELFs linked at USER_BASE, embedded via kernel/process/initrd.S.
UTESTS := hello segv spin fpu
UBINS  := sh ls pwd cat echo mkdir rm cp mv ps kill uname clear sleep uptime ed edit
UPROGS := init $(UTESTS) $(UBINS)
ULIB   := build/userland/lib/crt0.S.o build/userland/lib/syscall.S.o build/userland/lib/ulib.c.o \
          build/userland/lib/malloc.c.o build/userland/lib/printf.c.o
UELFS  := $(patsubst %,build/user/%.elf,$(UPROGS))
build/userland/%: CFLAGS := $(subst -mgeneral-regs-only ,,$(subst -Iinclude ,,$(CFLAGS))) -Iuserland/lib
build/userland/%: ASFLAGS := -mcpu=cortex-a53 -Iinclude/uapi -MMD -MP

all: kernel8.img

build/user/init.elf: build/userland/init/init.c.o
$(UTESTS:%=build/user/%.elf): build/user/%.elf: build/userland/tests/%.c.o
$(UBINS:%=build/user/%.elf): build/user/%.elf: build/userland/bin/%.c.o
$(UELFS): $(ULIB) userland/user.ld
	@mkdir -p $(dir $@)
	$(LD) -nostdlib -z max-page-size=4096 -T userland/user.ld -o $@ $(filter %.o,$^)

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

-include $(OBJS:.o=.d) $(ULIB:.o=.d) $(UTESTS:%=build/userland/tests/%.c.d) $(UBINS:%=build/userland/bin/%.c.d) build/userland/init/init.c.d
.PHONY: all qemu clean
