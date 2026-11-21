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

Output also appears on HDMI (640x480 text console). A USB keyboard on the OTG
port (directly, or behind a USB 1.1 hub) types into the same console; a
keyboard behind a USB 2.0 hub is not supported yet. With HDMI and a keyboard
the serial adapter is optional.

QEMU with a USB keyboard: add `-device usb-kbd` (the Pi model puts it behind
a hub) and type with the monitor's `sendkey`.

Expected (QEMU shown; real Pi reports ~448-500 MiB RAM and 19200000 Hz timer):

    PiOS 0.6
    ...
    [BOOT] Running at EL1
    [BOOT] Exception vectors installed
    [MEM] ARM RAM 0-3c000000 (960 MiB)
    [MEM] MMU on: kernel at ffffff8000000000 (TTBR1), caches enabled
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
(image size must be a power of two). `tools/mksdimg.py sd.img [path=file ...]`
builds one, and `tools/qemu-drive.py` scripts a session (see the files).

### Networking

Plug a USB Ethernet adapter that supports CDC-ECM (the standard USB
Ethernet class; many RTL8153 adapters do, QEMU's `usb-net` does) into the OTG
port, directly or through a USB 1.1 hub. PiOS takes an address by DHCP and
writes /etc/resolv.conf. With BusyBox installed, `ping`, `wget`, `nslookup`
and `nc` work. Wi-Fi is not supported. QEMU:
`-device usb-net,netdev=n0 -netdev user,id=n0` (add
`,hostfwd=tcp:127.0.0.1:8080-:80` to reach a server in PiOS).

### BusyBox (about 300 more commands)

PiOS runs static Linux/AArch64 binaries. Copy Alpine's static BusyBox to the
card as `bin/busybox` (the `busybox-static` package for aarch64; the binary
inside is `bin/busybox.static`, GPLv2). At boot, init links every applet into
/usr/bin, so `ls -l`, `vi`, `grep`, `sed`, `awk`, `find`, `tar`, `sh` (ash) and
the rest work by name. For QEMU:
`tools/mksdimg.py sd.img bin/busybox=busybox.static`.
`|`, `<`, `>`, `>>`, quotes and backslash work in PiOS's own shell; for `;`,
`&&`, variables and scripts run BusyBox's `sh`. ^C kills the running command.

No output: check TX/RX swap, `config.txt` present, firmware files present.
Green ACT LED flashing a pattern then nothing = firmware could not find a file.

## Third-party code

- `userland/lua`: Lua 5.4.7, Copyright (C) 1994-2024 Lua.org, PUC-Rio, MIT
  license (full text at the end of `userland/lua/lua.h`).
- `drivers/fb/font.c`: bitmap rendering of DejaVu Sans Mono, Bitstream Vera
  license (notice in the file).
- `net/lwip`: lwIP 2.2.0 TCP/IP stack (core, IPv4, Ethernet), Copyright (c)
  2001-2004 Swedish Institute of Computer Science, BSD license
  (`net/lwip/COPYING`).
