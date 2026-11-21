# PiOS

Original Unix-like OS for Raspberry Pi Zero 2 W (BCM2710A1, Cortex-A53, AArch64).
Status: Phase 7 (in progress). SD card FAT32 boot partition at /boot (read/write). Shell with pipelines and redirection, coreutils, kill and ^C, user processes, VFS + ramfs, pipes, /dev/console. See ROADMAP.md, DESIGN.md.

## Build (Void Linux)

Needs only `clang`, `make`, and host GNU `binutils` (Void's binutils is multi-target
and includes aarch64 `ld`/`objcopy`). Check with `ld -V | grep aarch64elf`.

    make            # -> kernel8.img
    make qemu       # QEMU raspi3b (same SoC family; not proof of Pi hardware boot)

## microSD card

1. FAT32 first partition (MBR). Existing Raspberry Pi OS card's boot partition works.
2. Copy from https://github.com/raspberrypi/firmware/tree/master/boot :
   `bootcode.bin`, `start.elf`, `fixup.dat`, `bcm2710-rpi-zero-2-w.dtb`,
   `overlays/disable-bt.dtbo`.
3. Copy `kernel8.img` and `boot/config.txt` to the partition root.
   Remove any other `kernel*.img` and `cmdline.txt` to avoid confusion.

## Serial console (3.3 V USB-UART adapter only, never 5 V)

| Pi header pin | Signal       | Adapter |
|---------------|--------------|---------|
| 6             | GND          | GND     |
| 8             | GPIO14 TXD0  | RX      |
| 10            | GPIO15 RXD0  | TX      |

Leave adapter VCC unconnected; power the Pi via USB. Then:

    picocom -b 115200 /dev/ttyUSB0

Output also appears on HDMI (640x480 text console). Typing still goes through
the serial adapter: there is no USB keyboard support.

Expected (QEMU shown; real Pi reports ~448-500 MiB RAM and 19200000 Hz timer):

    PiOS 0.6
    ...
    [BOOT] Running at EL1
    [BOOT] Exception vectors installed
    [MEM] ARM RAM 0-3c000000 (960 MiB)
    [MEM] MMU on: identity map, caches enabled
    [MEM] Page allocator: 245622 free pages (959 MiB)
    [TIMER] generic timer 62500000 Hz, tick 100 Hz
    [KERNEL] Hello from PiOS! Interrupts on.
    [PROC] init created, pid 1
    ...
    [KERNEL] Hello from PiOS! Interrupts on.
    [init] running as pid 1
    [init] PASS getpid() == 1
    ...
    [init] all tests passed
    [init] starting /bin/sh (^D or exit to restart it)
    / $

Commands: ls pwd cat echo mkdir rm cp mv ps kill uname clear sleep uptime ed edit lua,
builtins cd exit. `lua` runs Lua 5.4 scripts or an interactive prompt. The shell prompt supports arrow-key editing and up/down history. Programs copied to /boot/bin run by name.
The card's boot partition is at /boot and changes persist. New file names
there must be 8.3 (`notes.txt` ok, `my-notes.txt` not); existing long names
read fine. Keep a backup of the boot files: this FAT driver is new.

QEMU with a disk image: `qemu-system-aarch64 -M raspi3b -kernel kernel8.img
-serial stdio -display none -drive file=sd.img,if=sd,format=raw`
(image size must be a power of two).
`|`, `<`, `>`, `>>` and quotes work. ^C kills the running command.

No output: check TX/RX swap, `config.txt` present, firmware files present.
Green ACT LED flashing a pattern then nothing = firmware could not find a file.

## Third-party code

- `userland/lua`: Lua 5.4.7, Copyright (C) 1994-2024 Lua.org, PUC-Rio, MIT
  license (full text at the end of `userland/lua/lua.h`).
- `drivers/fb/font.c`: bitmap rendering of DejaVu Sans Mono, Bitstream Vera
  license (notice in the file).
