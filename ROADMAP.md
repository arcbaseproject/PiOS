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

## Phase 6 - Unix environment (next)
Minimal libc (stdio-ish printf, string, malloc via brk/sbrk), shell (fork/exec,
PATH, redirection `< > >>`, pipelines, cd/exit builtins, env vars), coreutils:
ls cd pwd cat echo mkdir rm cp mv ps kill uname clear. Needs: brk/sbrk, kill +
basic signals, a process listing interface for ps, uname syscall.

## Phase 3-7
Multitasking, EL0 userspace + syscalls + ELF, VFS/ramfs/devfs, libc/init/sh/coreutils,
ext2/SMP/USB/network. Details added when each phase starts.
