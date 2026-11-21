# Design decisions

- **Toolchain**: clang `--target=aarch64-none-elf` + host multi-target GNU ld/objcopy.
  No extra packages on Void. Kernel built with `-mgeneral-regs-only` so FP/SIMD
  state never needs saving in kernel code.
- **Boot**: firmware loads `kernel8.img` at 0x80000 on core 0 at EL2; cores 1-3
  stay in the firmware spin table (0xd8..0xf0). `start.S` parks non-zero cores
  anyway, drops EL2 -> EL1h (AArch64, DAIF masked, EL1 timer access enabled),
  sets stack, clears BSS, calls `kernel_main`. EL3 entry is not supported.
- **Memory layout**: image at 0x80000, BSS after, 16 KiB boot stack above BSS
  (not below 0x80000, avoiding firmware-used low memory).
- **Peripherals**: BCM2837 ARM-side base 0x3F000000 (BCM2710A1 shares it).
- **Console**: PL011 UART0 on GPIO14/15 at 115200 8N1. `dtoverlay=disable-bt`
  frees it from Bluetooth; `init_uart_clock=48000000` pins UARTCLK so the
  divisor (IBRD 26, FBRD 3) is fixed. Chosen over mini UART because mini UART's
  clock follows the VPU core clock, and QEMU emulates PL011 as serial0.

## Phase 2

- **Exceptions**: `vectors.S` saves x0-x30, ELR, SPSR into a 272-byte
  `struct trap_frame`. Only current-EL/SP_ELx sync and IRQ are wired; every
  other slot dumps registers and panics. Sync faults are fatal for now.
- **Memory size**: queried from firmware via mailbox property tag 0x00010005
  (never hardcoded; the GPU split changes it). Buffer address passed as bus
  address (`| 0xC0000000`), done before caches are on.
- **MMU**: the kernel is linked at KERNEL_VA (0xFFFFFF8000000000) + 0x80000
  and runs in TTBR1, where every physical address pa appears at KERNEL_VA + pa
  (P2V/V2P in mm.h). start.S builds the tables before any C runs: one L1
  shared by TTBR0 (temporary identity map, for the jump) and TTBR1, an L2 of
  2 MiB blocks (RAM normal WB cacheable, 0x3F000000+ device), and a 1 GiB
  device block for the local peripherals at 0x40000000. It enables the MMU
  and branches to the high address. mmu_init then unmaps VideoCore RAM and
  points TTBR0 at an empty table. 4 KiB granule, T0SZ=T1SZ=25.
- **Pages**: free list through free pages, from end of boot stack to end of ARM
  RAM. `page_alloc` returns zeroed pages. No contiguous allocation yet.
- **Timer**: EL1 non-secure physical timer at 100 Hz, frequency from
  CNTFRQ_EL0 (firmware-set). Routed to core 0 IRQ via local controller
  0x40000040; source read from 0x40000060. The BCM2835 peripheral IRQ
  controller (0x3F00B200) is not used yet: no device IRQs needed until UART RX.
- **Build**: `-mstrict-align` because before the MMU is on all memory is
  Device type, where unaligned access faults.
- **No spinlocks yet**: single core; IRQ masking is enough until SMP.

## Phase 3

- **Tasks**: static table of 64 `struct task` (no heap needed). Slot 0 is the
  boot thread, which becomes the idle task (pid 0, runs `wfi`) and runs only
  when nothing else is runnable. Each kernel thread gets one 4 KiB page stack.
- **Context switch**: `cpu_switch` saves/loads only AAPCS64 callee-saved regs
  (x19-x28, fp, lr, sp). Caller-saved state is already on the stack (C call or
  IRQ trap frame). Kernel uses no FP/SIMD, so none is saved.
- **New threads** start at `task_entry` (x19=fn, x20=arg): reap any zombie,
  unmask IRQs, call fn, then `task_exit`.
- **Preemption**: every 10 ms timer IRQ calls `schedule()` from the IRQ
  handler; the interrupted thread's trap frame stays on its own kernel stack
  and is restored when it is switched back in. Round-robin over the table.
- **Exit/cleanup**: an exiting thread becomes ZOMBIE; the next thread to run
  frees its stack in `schedule_tail` (can't free the stack you're running on).
  No parent/wait yet: that arrives with processes in Phase 4.
- `kprintf` masks IRQs per call so lines don't tear under preemption.

## Phase 4

- **Address spaces**: each process has its own L1 table in TTBR0 holding
  only user mappings (the kernel is in TTBR1, AP=00 and UXN, so EL0 cannot
  touch it). PiOS programs use 0x80000000-0xBFFFFFFF, 4 KiB pages, PXN, nG;
  the rest of the 512 GiB TTBR0 range is free (Linux binaries load at
  0x400000). Page tables store physical addresses; the kernel edits them
  through P2V. Kernel threads run with an empty TTBR0.
- **Switch**: `schedule()` writes TTBR0 and does `tlbi vmalle1` when the next
  task's table differs. No ASIDs yet.
- **Syscall ABI** (`include/uapi/uapi.h`): `svc #0`, number in x8, args
  x0-x5, result x0, negative = -errno. Other registers preserved (whole trap
  frame restored). Implemented: exit(1) fork(2) write(3) getpid(4) execve(5)
  waitpid(6). write() goes to the console for fd 1/2 only until Phase 5.
  (Phase 9 replaced the numbering with Linux's; see there.)
- **User pointers**: `vm_check` walks the current table and requires each page
  to be present, EL0-accessible, and writable if needed; then the kernel
  dereferences the user VA directly. Strings copied byte-wise with a check at
  each page crossing.
- **Process entry**: kernel enters `_start` with x0=argc, x1=argv, x2=envp,
  sp = 16-byte aligned pointer to argv array; strings in the top stack page.
  Stack is 256 KiB at 0xBFFC0000, fixed size, mapped eagerly, NX.
- **ELF**: static ET_EXEC AArch64 only; PT_LOAD segments mapped with RO/NX
  from p_flags; D-cache cleaned to PoU and I-cache invalidated after load and
  after fork copies.
- **fork**: eager copy of every user page (no COW). Child gets copied trap
  frame with x0=0 and starts at `ret_from_fork`.
- **exit/wait**: exit frees the address space immediately, reparents children
  to init, becomes ZOMBIE, wakes a waiting parent. Kernel stack freed by the
  next task (`schedule_tail`); slot freed by `waitpid`. Status uses POSIX
  encoding (exit code << 8, or signal number for faults).
- **User faults**: any non-SVC sync exception from EL0 prints ESR/FAR/ELR and
  kills the process (SIGSEGV, or SIGILL for undefined instruction). No signal
  delivery yet.
- **Programs**: no filesystem yet, so user ELFs are built separately
  (`userland/`, linked at 0x80000000) and embedded with `.incbin`
  (`kernel/process/initrd.S`). `execve` looks names up there.
- **FP/SIMD**: userland is built with `-mgeneral-regs-only` because FP state is
  not saved on context switch and CPACR_EL1 traps FP. Must be fixed before a
  real libc/compiler-generated FP code.

## Phase 5

- **VFS**: `struct inode` + `struct inode_ops` vtable (read/write/truncate/
  close/release, and lookup/create/link/unlink/readdir for directories).
  Path walk handles absolute/relative, `.`, `..`, repeated slashes; `..` of
  root is root. No mount table yet: ramfs is the only filesystem. Mounting
  arrives with ext2.
- **Lifetime**: `nlink` counts directory entries, `refs` counts open files,
  cwds and in-kernel holders. Inode freed when both reach 0, so an unlinked
  open file stays readable until closed.
- **Files**: `struct file` {inode, offset, flags, refs}; per-task `fd[16]`.
  dup/dup2/fork share the object (shared offset, as POSIX requires).
  Offsets only advance on regular files.
- **ramfs**: file data in pages via one index page (max 2 MiB/file, holes
  read as zero); directories are linked lists of names. `size` of a
  directory = entry count.
- **/dev**: device inodes (`console`, `null`) linked into a ramfs directory,
  not a separate filesystem.
- **Console**: PL011 RX + receive-timeout interrupts via BCM2835 controller
  (IRQ 57, bank 2 bit 25) routed to core 0 as GPU IRQ. IRQ-side line
  discipline: echo, backspace/DEL, ^C discards line (no signals yet), ^D
  flushes or signals EOF on an empty line. read() returns at most one line.
- **Pipes**: one-page ring; reader blocks while empty with writers, EOF when
  no writers; writer blocks while full, -EPIPE with no readers (no SIGPIPE).
- **Blocking**: generic `sleep_on(chan)`/`wakeup(chan)` replaced the ad hoc
  waitpid wakeup. Syscalls run with IRQs masked, so check-then-sleep is
  race-free on one core.
- **kmalloc**: power-of-two classes 32..2048 B with a 16 B header; freed
  objects go back to their class, never to the page allocator.
- **Boot tree**: /bin /dev /etc /home /proc /sbin /tmp /usr /var created at
  boot; embedded programs copied into /sbin/init and /bin/*. execve reads
  ELFs through the VFS. /proc is an empty directory for now.
- **Syscalls added**: read(7) open(8) close(9) lseek(10) stat(11) fstat(12)
  dup(13) dup2(14) pipe(15) chdir(16) getcwd(17, returns length incl. NUL)
  mkdir(18) unlink(19) rmdir(20) rename(21) getdents(22, fixed 64-byte
  `struct dirent` records, `.` and `..` synthesized).
- **No permissions/uids**: mode bits stored, never enforced. Single user.

## Phase 6

- **Signals**: no handlers or masks. `kill(pid, sig)` sets `task->killed`;
  the task exits with that signal status on its next return to EL0 (after a
  syscall or an IRQ taken from EL0). A sleeping target is made runnable and
  `sleep_on` returns -EINTR, so every blocking loop (pipe, console, waitpid)
  bails out. Init and kernel threads cannot be killed.
- **Process groups**: `pgid` inherited on fork; `setpgid` on self or own
  child. `tcsetpgrp(pgid)` names the console's ^C target (0 = none, ^C just
  discards the line). The shell gives each pipeline its own group, hands it the
  console while waiting, then resets to 0 so ^C at the prompt cannot kill it.
- **ps**: `getprocs(buf, max)` fills fixed `struct procinfo` records instead
  of a /proc filesystem.
- **uname**: no syscall; `PIOS_VERSION` in uapi.h is shared by kernel banner
  and userland, which ship in one image.
- **Shell**: one read() per line (console is cooked); commands without `/`
  run from /bin. No heap anywhere in userland yet: static buffers only.
- **Syscalls added**: kill(23) setpgid(24) tcsetpgrp(25) getprocs(26).

## Phase 7

- **SD**: firmware leaves the SD slot on SDHOST; `sd_init` switches GPIO 48-53
  to ALT3, routing it to the Arasan EMMC (SDHCI v3) at 0x3F300000. That takes
  EMMC from the Wi-Fi chip, which is unused. Base clock from mailbox (clock id
  1); 400 kHz identification, then 25 MHz, 1-bit bus, single-block CMD17/24,
  polled. Runs with IRQs masked like every syscall, so I/O stalls the system
  (fine at this scale). No card = boot continues without /boot.
- **Mailbox** now cleans/invalidates its buffer line around each call, so it
  works after caches are on.
- **Mounting**: no mount table. The FAT root inode is linked into the ramfs
  root as `boot`, the same way /dev nodes are linked; path walk, `..` and
  getcwd needed no changes. rename across filesystems returns -EXDEV.
- **FAT32**: partition 1 of an MBR (type 0x0B/0x0C) or a partitionless volume.
  512-byte sectors only. Write-through with one cached data sector and one
  cached FAT sector; FAT updates go to every FAT copy. FSInfo free count is
  set to "unknown" at mount rather than maintained. Inodes are cached per
  directory-entry location and never evicted, so getcwd/readdir see stable
  inode numbers. rename = new entry (link) then delete old (unlink); the inode
  remembers its old location so unlink doesn't free the moved data.
  Long names are read (up to NAME_MAX); new names must be 8.3, single case
  (lowercase stored via the NT case flags, as Windows/Linux do). Gaps from
  seeking past EOF are zero-filled. Dates fixed at 1980-01-01 (no RTC).

## Phase 8

- **FP/SIMD**: CPTR_EL2.TFP=0 and CPACR_EL1.FPEN=11 at boot. The kernel never
  touches FP registers, so user state stays live in them across syscalls and
  IRQs; `schedule()` saves prev's q0-q31/fpsr/fpcr into `task->fp` and loads
  next's (eager, 528 bytes per switch). fork copies the live registers;
  execve zeroes them.
- **Heap**: `brk` grows from the page-rounded end of the highest PT_LOAD
  segment up to one guard page below the stack. Pages are mapped (zeroed) as
  the break rises; shrinking only moves the break. fork copies heap pages with
  everything else.
- **libc** (userland/lib): K&R first-fit malloc with coalescing over sbrk (16
  KiB growth steps, 16-byte aligned blocks); printf family without floating
  point, 1 KiB per printf call.
- **Time**: tick-based nanosleep (10 ms resolution, never early, -EINTR on
  kill); clock_gettime reads CNTPCT directly. No wall clock: the board has
  no RTC.
- **Raw console**: ttymode(TTY_RAW) makes the console pass every byte,
  including ^C, straight to read() without echo or line buffering (like
  cfmakeraw). The shell resets cooked mode after every pipeline, so a raw
  program that dies cannot leave the console unusable.
- **edit**: full-screen editor; assumes an 80x24 VT100 terminal and repaints
  the whole screen per key (~2 KB, ~170 ms at 115200 baud on real serial).
- **Signals**: per-task handler table (SIG_DFL, SIG_IGN, or address), pending
  and blocked bitmasks. Ignored signals are dropped at send time; default
  ones set `killed` (exit at the next return to EL0) as before. Caught
  signals are delivered on every return to EL0: the kernel pushes a sigframe
  (FP/SIMD state, trap frame, old mask) on the user stack, blocks the signal,
  and enters the handler with x0 = sig and lr = the restorer libc passed to
  sigaction (`__sigtramp`, which calls sigreturn). sigreturn restores the
  frame but keeps only NZCV from the saved SPSR, so a forged frame cannot
  leave EL0t or mask IRQs. A caught signal makes blocking syscalls return
  -EINTR (no SA_RESTART). Faults (SIGSEGV/SIGILL) and SIGKILL are not
  catchable; init cannot be signalled. exec resets handlers, keeps ignores.
- **HDMI console**: framebuffer 640x480x32 from mailbox tags 0x48003-0x48006,
  0x40001, 0x40008. It lives in VideoCore RAM, which mmu_map_nc maps as
  Normal non-cacheable (MAIR attr2 = 0x44) so the GPU sees writes without
  cache maintenance. uart_putc feeds every byte to fbcon, so kernel and user
  output appear on both; input is still serial only. Font: 8x16 rasterized
  from DejaVu Sans Mono by tools/mkfont.py. Scrolling copies the screen.
- **USB keyboard**: DWC2 at 0x3F980000, powered via mailbox (device 3),
  forced into host mode with buffer DMA (bus address = phys | 0xC0000000,
  cache cleaned/invalidated around each transfer). No USB IRQ: a kernel
  thread waits for a root-port connection, enumerates the device (and, for a
  hub, devices on its ports, rescanning ports whose connection changed), then
  polls the keyboard's interrupt endpoint every 10 ms with HID boot reports.
  New key presses become console_input() bytes, arrows etc. as VT100
  sequences, so programs cannot tell it from serial input. Repeat: 500 ms,
  then 25/s. No split transactions: a low/full-speed keyboard behind a USB 2.0
  hub does not work. US layout, no LEDs.
- **C library for ports** (userland/libc, linked as an archive): stdio
  (buffered FILE over fds, stdout line-buffered and flushed by exit() via
  `__exit_hook`), strtod/strtol, string/ctype extras, "C" locale only,
  setjmp/longjmp (x19-x30, sp, d8-d15), libm with series kernels (a few ulp,
  not correctly rounded), time.h counting from boot as the epoch (no RTC).
  printf gained %f %e %g %o and + space # flags; float digits come from
  double-precision scaling, so digits past ~15 significant ones may be off.
- **malloc**: requests up to 512 bytes are recycled through per-size free
  lists (O(1)); larger ones use the K&R coalescing arena. Without this,
  Lua's GC freeing many interleaved small blocks took quadratic time.
- **Lua**: upstream 5.4.7 sources unmodified in userland/lua, built with
  LUA_USE_C89 (Lua parses hex floats itself; no %a, no popen, no dlopen).
  User stacks are 256 KiB so Lua's C recursion limit (200 levels) fits.
- **Syscalls added**: brk(27) nanosleep(28) clock_gettime(29) ttymode(30)
  sigaction(31) sigreturn(32).

## Phase 9

- **Linux ABI**: PiOS adopted the Linux/AArch64 syscall interface outright
  (asm-generic numbers, struct stat/dirent64/termios/sigaction layouts,
  -errno returns) instead of carrying two ABIs. PiOS's own programs reach it
  through ulib wrappers (open -> openat(AT_FDCWD), fork -> clone(SIGCHLD),
  waitpid -> wait4, ttymode/tcsetpgrp -> termios ioctls), so static musl
  binaries such as Alpine's busybox.static run unmodified. PiOS-only calls
  live at >= 1000 (getprocs).
- **Process entry**: Linux initial stack (argc, argv, envp, auxv with
  AT_PHDR/PHNUM/PAGESZ/ENTRY/RANDOM/HWCAP), registers zero. TPIDR_EL0 is
  saved per task (musl's thread pointer), copied on fork, cleared on exec.
- **Coverage**: what BusyBox needs. clone only as fork/vfork (vfork copies
  instead of sharing memory: fine for vfork+exec); mmap anonymous only
  (file mappings -> ENODEV), carved downward from below the stack, PROT_NONE
  pages left unmapped, mprotect a no-op; *at() calls take AT_FDCWD or a
  directory fd (used as a temporary cwd), renameat only within one dirfd;
  ppoll re-checks readiness every tick (no wait queues); wait4 honours
  WNOHANG and pid/pgid selection; signals with default action "ignore"
  (SIGCHLD, SIGWINCH, ...) and the stop signals are dropped; SIGCHLD is sent
  on child exit; FD_CLOEXEC/O_CLOEXEC close on exec; sendfile, select,
  sockets, /proc are missing (ENOSYS), and applets that need them fail.
- **Symlinks**: ramfs only (FAT refuses with EPERM). Path walks restart at
  the link target, at most 8 levels (ELOOP). lstat/readlink/symlinkat.
- **BusyBox**: installed by copying busybox.static to /boot/bin/busybox.
  init asks `busybox --list` at boot and symlinks each applet into /usr/bin
  (ramfs), skipping ones that need /proc (ps, uptime, ...) so PiOS's own
  stay in use; PATH=/usr/bin:/bin:/boot/bin. `make STRACE=1` logs syscalls
  failing with ENOSYS/EINVAL/ENOTTY/EFAULT when porting more binaries.
- **Address space**: PiOS programs still link at 0x80000000; Linux binaries
  load at their ELF addresses (0x400000 for BusyBox). The stack is the top
  256 KiB below 0xC0000000, mmap grows down beneath it, brk grows up from
  the image end; each keeps a guard page from the other.

## Phase 10

- **USB rework**: a device table (root device plus hub ports) with class
  drivers for hubs, HID boot keyboards and CDC-ECM. Endpoint packet sizes
  and data toggles are tracked per endpoint. Control/interrupt/bulk-out
  transfers are synchronous on channel 0. Bulk IN cannot be: in buffer-DMA
  mode the core retries NAKs itself, so a receive only finishes when data
  arrives. Channel 1 therefore stays armed and is checked without blocking
  (usb_bulk_rx); the old blocking attempt timed out after 100 ms and dropped
  frames that arrived as the channel was disabled. While frames flow the USB
  thread polls back-to-back (yielding), otherwise every tick.
- **CDC-ECM**: picks the configuration with an ECM control interface, reads
  the MAC from the iMACAddress string, selects the data interface's bulk
  alternate setting, sets the packet filter. Frames are raw Ethernet.
- **lwIP**: 2.2.0 unmodified in net/lwip (core, IPv4, Ethernet), NO_SYS
  raw API. The lock is "IRQs masked": syscalls already run that way, and the
  USB and net threads mask IRQs around every lwIP call and USB transfer. A
  "net" kernel thread runs lwIP timers and loopback delivery every tick.
  ARP_QUEUEING is on: without it, of two packets sent while ARP resolved only
  the last survived (musl sends its A and AAAA DNS queries back to back).
- **Sockets** (net/socket.c): S_IFSOCK inodes, so read/write/close/ppoll
  work on them. TCP receive data is a pbuf chain acknowledged (tcp_recved)
  as it is read; accepted connections queue on the listener; lwIP callbacks
  wake sleepers. UDP and raw ICMP keep a datagram queue; raw sockets see
  the IP header like Linux. Unsupported: AF_UNIX, IPv6, IP_HDRINCL, socket
  timeouts, ancillary data.
- **setitimer** (ITIMER_REAL): per-task deadline in ticks, checked in the
  timer IRQ, delivers SIGALRM (BusyBox ping's interval timer).
