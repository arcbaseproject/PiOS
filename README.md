# PiOS

Original Unix-like OS for Raspberry Pi Zero 2 W (BCM2710A1, Cortex-A53, AArch64).
Status: Phase 5. User processes, VFS + ramfs, file descriptors, pipes, /dev/console with line editing. See ROADMAP.md, DESIGN.md.

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

Expected (QEMU shown; real Pi reports ~448-500 MiB RAM and 19200000 Hz timer):

    PiOS 0.5
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
    [init] console echo: type lines, ^D to finish
    >

Type a line and press Enter: it is echoed back. ^D on an empty line ends init.
    [KERNEL] init exited, status 0, free pages N (+1 kstack pending)

No output: check TX/RX swap, `config.txt` present, firmware files present.
Green ACT LED flashing a pattern then nothing = firmware could not find a file.
