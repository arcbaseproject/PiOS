# Roadmap

## Phase 1 - Bootable kernel
- [x] Toolchain (clang + host binutils)
- [x] start.S: core park, EL2->EL1, stack, BSS
- [x] Linker script, Makefile (incremental, -Werror)
- [x] PL011 UART output
- [x] Verified in QEMU raspi3b
- [ ] Verified on real Pi Zero 2 W  <- needs hardware run

## Phase 2 - Kernel foundations
- [x] Exception vectors, register dump + panic (BRK test verified in QEMU)
- [x] kprintf / panic
- [x] Mailbox ARM memory query
- [x] MMU identity map, D/I caches on
- [x] Page allocator + boot self-test
- [x] Generic timer 100 Hz IRQ via local interrupt controller
- [x] Verified in QEMU raspi3b
- [ ] Verified on real Pi Zero 2 W
- [ ] BCM2835 peripheral IRQ controller (deferred until a device IRQ is needed)

## Phase 3 - Multitasking
- [x] Task control blocks (static table, pids)
- [x] Kernel threads with own stacks
- [x] Context switch (callee-saved regs)
- [x] Preemptive round-robin (timer tick)
- [x] Thread exit + stack reclaim (self-test checks page count)
- [x] Verified in QEMU raspi3b
- [ ] Verified on real Pi Zero 2 W

## Phase 4 - Userspace
- [x] EL0 execution, SVC entry, syscall ABI documented
- [x] Per-process page tables, kernel EL1-only, user W^X per ELF segment
- [x] ELF64 loader, argv/envp on user stack
- [x] fork / execve / exit / waitpid / getpid / write(console)
- [x] User fault kills process; user pointer validation (-EFAULT)
- [x] EL0 preemption; no page leaks across 6 processes
- [x] init test suite: 10/10 pass in QEMU raspi3b
- [ ] Verified on real Pi Zero 2 W
- [ ] Deferred: COW fork, ASIDs, signals, FP/SIMD context, stack growth

## Phase 5 - Filesystems
- [x] VFS with inode ops, path resolution, refcounted inodes
- [x] Per-process fd table, shared file objects (dup/dup2/fork)
- [x] ramfs (files, directories), standard Unix tree
- [x] /dev/console (IRQ-driven input, line editing), /dev/null
- [x] Pipes with blocking and EOF/EPIPE
- [x] read open close lseek stat fstat dup dup2 pipe chdir getcwd mkdir unlink rmdir rename getdents
- [x] execve loads from VFS
- [x] init test suite: 43/43 pass in QEMU raspi3b; console echo verified with piped serial input
- [ ] Verified on real Pi Zero 2 W
- [ ] Moved to Phase 7: SD card driver, ext2, mount table

## Phase 6 - Unix environment
- [x] kill (every signal terminates), process groups, ^C kills the foreground group
- [x] getprocs syscall for ps
- [x] sh: pipelines, `< > >>`, quoting, /bin lookup, cd/exit builtins; init respawns it
- [x] coreutils: ls pwd cat echo mkdir rm cp mv ps kill uname clear
- [x] init test suite: 50/50 pass in QEMU raspi3b; shell session verified with piped serial input
- [ ] Verified on real Pi Zero 2 W
- [ ] Deferred until a program needs them: brk/malloc, printf, env var expansion,
      signal handlers, background jobs (`&`), `cp` into a directory, `rm -r`

## Phase 7 - Storage (in progress)
- [x] SD card driver: EMMC controller via GPIO 48-53 ALT3, polled PIO
- [x] FAT32 read/write on partition 1, attached at /boot; long names read, 8.3 names created
- [x] Verified in QEMU raspi3b: persistence across reboot, exec from /boot, delete frees clusters, fsck.fat clean
- [ ] Verified on real Pi Zero 2 W  <- highest risk: EMMC init timing on real silicon
- [ ] Deferred: long-name creation, 4-bit bus / multi-block I/O, timestamps (no RTC),
      mount syscall, ext2, cross-filesystem mv

## Phase 8 - Userland foundations
- [x] Programs in /boot/bin run by name (install = copy to the card)
- [x] FP/SIMD registers saved per process; userland built without -mgeneral-regs-only
- [x] brk syscall; libc malloc/free/calloc/realloc, printf/snprintf/dprintf, more string functions
- [x] nanosleep, clock_gettime(CLOCK_MONOTONIC); sleep, uptime
- [x] ed: line editor
- [x] Raw console mode (ttymode syscall); edit: full-screen editor (arrows, ^S save, ^Q quit)
- [x] sh line editing (arrows, Home/End, Delete) and 16-entry up/down history
- [x] Signal handlers: sigaction/sigreturn, libc signal(), SIG_IGN, signal blocked during its handler
- [x] HDMI text console: 640x480 framebuffer, 80x30, mirrors the serial console (VT100 subset)
- [x] USB keyboard: DWC2 host (polled, DMA), one hub level, HID boot protocol, key repeat
- [x] Lua 5.4.7 as /bin/lua, on a new C library subset (stdio, float printf/strtod, libm, setjmp, time)
- [x] init test suite: 67/67 pass in QEMU raspi3b
- [ ] Verified on real Pi Zero 2 W
- Next candidates: COW fork,
  RTC-less wall clock (set by user), SMP, USB, network.
