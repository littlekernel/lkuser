# LKUser Development Notes

`lkuser` is an overlay repository that builds a user-space environment on top of:
- LK (`lk/` submodule) for the kernel and base build system
- newlib (`newlib/` submodule) as the user-space libc

Treat `lk/` and `newlib/` as upstream submodules unless intentional submodule changes are required. `build-*` and `install-*` directories are generated output — never treat as source or commit files from them.

## Quick Commands

Run from repository root:

```bash
make -j $(nproc)     # build everything: newlib (first time), LK kernel, user apps/libs
make test            # build + generate FAT image + boot QEMU (exit QEMU with Ctrl-A X)
make test-auto       # build + boot QEMU non-interactively, run the test apps, pass/fail (riscv only)
make clean           # clean kernel + user-space outputs (keeps newlib)
make spotless        # also remove newlib build/install trees
make fs              # just regenerate build-$(ARCH)/root.fat
make list-toolchain  # print the required cross toolchain prefix
```

Architecture is selected with `ARCH` (default `riscv` in the top-level `makefile`). Only riscv (64-bit) builds: the trap stubs (`lib/lku/syscalls.c`) and the kernel's syscall entry exist for riscv alone. `arch/arm`, `lib/lku/crt0_arm.S` and `sys/project/qemu-arm-usertest.mk` are left as they were and do not build. Never assume host architecture — always check/propagate `ARCH` on build and test targets. The toolchain prefix comes from `arch/<arch>/arch.mk`: `riscv64-elf-` for riscv (overridable via `TOOLCHAIN_PREFIX`; CI fetches prebuilt toolchains from newos.org/toolchains). Building the FAT image requires mtools (`mformat`/`mcopy`); `make test` requires `qemu-system-riscv64`.

`make test` is an interactive QEMU boot. `make test-auto` is the gate (`scripts/qemu-test.sh`, riscv only): an `lk.autorun` script mounts the image, runs each app in `TEST_APPS` with `lkuser load`/`run`, and powers off, with stdin closed. It passes only if:
- QEMU powered off by itself within `TEST_TIMEOUT` seconds (default 60; a run takes well under a second),
- no kernel panic or assert was logged,
- every app printed the exact line `<NAME>: PASS` (name upper-cased) and its process logged `exited with code 0`.

Each `TEST_APPS` entry is `name[:arg...]`: `rawtest:a:b:c` runs `lkuser load /bin/rawtest a b c`. The default is `rawtest:a:b:c` (no libc: user entry, the startup block, the trap ABI, `debug_write`'s checks on bad user pointers, unserved syscall numbers, exit). `TEST_SMP` sets `-smp`. The console log is kept in `build-$(ARCH)/test-auto.log`.

A user fault ends only the faulting LK thread (LK's default handler), so its process never exits and the gate fails by timeout; `TEST_TIMEOUT=5` shortens the wait while debugging.

Local make overrides can go in an untracked `local.mk` (auto-included).

## Architecture: Two Build Systems, One Makefile

The top-level `makefile` drives three mostly independent builds:

1. **newlib** — configured/built once per arch into `build-newlib-$(ARCH)` and installed into `install-newlib-$(ARCH)` (stamp-file gated; only rebuilt after `make spotless` or `make clean-newlib`). Configured with `--disable-newlib-supplied-syscalls`; its system calls come from this repo's `lib/lku`.
2. **LK kernel** — via `makefile.lk` (symlink to `lk/makefile`) with `PROJECT=qemu-$(ARCH)-usertest`. `lk_inc.mk` sets `LKINC := sys`, so LK's module system also searches `sys/`: the project files live in `sys/project/`, and the kernel-side module is `sys/lib/lkuser` (referenced as module `lib/lkuser` in LK's namespace — note repo-root `lib/` is a *different*, user-space directory). Output: `build-qemu-$(ARCH)-usertest/lk.elf`.
3. **User space** — the top-level makefile itself compiles apps and libs with its own simple rule system (not LK's): it `find`s every `apps/*/app.mk` and `lib/*/lib.mk`, which set `APP_SRCS`/`LIB_SRCS` etc. and include the shared `make/app.mk` / `make/lib.mk`. Output goes to `build-$(ARCH)/`; the generated `.d` files are included, so header changes rebuild their users. Apps link with `ld`: the objects, then `--start-group` `APP_LIBS` (`lib/lku/lku.a`), newlib's `libc.a`/`libm.a` `--end-group`, then libgcc; crt0 comes out of `lku.a`, pulled in by the `_start` entry symbol. Per-arch flags, linker scripts, and newlib multilib paths are in `arch/<arch>/arch.mk`.

To add a user app: create `apps/<name>/app.mk` modeled on `apps/boottest/app.mk` (newlib) or `apps/rawtest/app.mk` (no libc) — it is discovered automatically. Kernel-side code follows normal LK module conventions (`rules.mk`, `MODULE_DEPS`, ...).

## The Kernel Side (`sys/lib/lkuser/`)

Kernel objects share `object` (`object.h`): an `lk::ref_counted` count and a kind, no virtual functions. The last release calls `object::destroy`, which deletes the concrete type by kind; derived types are reached with `static_cast` after a kind check, never `containerof`.

- `proc.cpp`: a process owns an LK user aspace, its threads and its exit status. `proc::create` builds the startup block from argv; `start` maps a 64KB main stack, copies the block to its top and starts the main thread. `exit` prints `lkuser: process <pid> (<name>) exited with code N` (the gate counts the `exited with code 0` suffix, so it stays last on the line). A reaper thread (started from an `LK_INIT_HOOK`) joins the threads of exited processes and frees their address spaces.
- `thread.cpp`: a user thread embeds its LK `thread_t`, sets `TLS_ENTRY_LKUSER` to itself and enters user space with `arch_enter_uspace`. It must outlive the LK thread, so it is dropped only after the reaper joins it.
- `copy.cpp`: every access to user memory. A kernel-mode fault on a user address is fatal, so pages are looked up with `arch_mmu_query` under `vmm_aspace_lock` and copied through the physmap, returning `ERR_FAULT` for anything outside the aspace, unmapped, or lacking USER plus READ/WRITE. It works on any aspace, active or not. Syscall handlers receive user pointers as `user_ptr<T>`, which has no dereference.
- `user.cpp`: the ELF loader (borrows the target aspace with `vmm_set_active_aspace` while `elf_load` writes the segments) and the `lkuser` console command.
- `syscalls.cpp`: the riscv syscall entry, the typed dispatcher and the handlers.

Allocation failure is an error the caller sees, never a panic: every `lk::make_ref`, nothrow `new` and `malloc` is checked and unwinds with `ERR_NO_MEMORY`. Plain `new` (LK's panics) and asserts on allocation results are not used.

## Syscall Mechanism (the key cross-file flow)

The raw syscall table is a single X-macro list: `sys/lib/lkuser/include/sys/_syscalls.h`, 21 rows of `LK_SYSCALL_DEF(num, ret, name, args...)`. `exit`, `thread_exit` and `debug_write` have handlers; every other row returns `ERR_NOT_SUPPORTED`, as does a number past the table. Shared definitions live beside it: `sys/lk_abi.h` (handle and token types, the startup block and its tags, the width rule) and `sys/lk_err.h` (LK's status codes for user space). These headers compile on both sides, so they use only `<stdint.h>`/`<stddef.h>` types.

Trap ABI (riscv): number in `a7`, arguments in `a0`-`a5`, the result in `a0` as a single status (`NO_ERROR` or a negative `ERR_*`, never an errno). Every argument fits one register on 32 and 64 bit; a 64-bit value travels in an argument structure. Interrupts are enabled while a handler runs, and the thread is marked as inside a syscall.

- **Kernel side**: `syscalls.h` declares one `sys_<name>` per row from the row itself, with each pointer argument turned into a `user_ptr<T>`; a handler defined with another signature fails to link. `syscalls.cpp` converts the registers to the declared argument types (integers keep their low bits) and dispatches by number.
- **User side**: `lib/lku/syscalls.c` expands each row into a naked `lk_<name>` stub (`li a7, n; ecall; ret`); `lib/lku/include/lk/ipc.h` declares them.

To add a syscall: add a row with the next number, then define its `sys_<name>` handler in the kernel (replacing the `ERR_NOT_SUPPORTED` stub if the row exists). Both sides recompile from the same header, so numbers and signatures can never skew.

## User Space (`lib/lku`)

- `crt0_riscv.S`: `_start` sets `gp` and calls `_start_c` with `sp` (the startup block) in `a0`.
- `newlib.c`: `_start_c` for newlib programs (argc/argv from the startup block, libc init, `exit(main(argc, argv))`) and newlib's system calls. Only `_exit` reaches the kernel; the others fail with `ENOSYS` (`_sbrk` with `ENOMEM`), so newlib programs such as `boottest` and `hello` build but cannot print or allocate, and are not in the gate.
- A program without libc (`apps/rawtest`) defines its own `_start_c`, so `newlib.o` never links in.

The startup block (`struct lk_proc_args` in `sys/lk_abi.h`): `nargs` and `nhandles`, then the argument offsets, then (tag, handle) pairs, then the strings; offsets count from the start of the block.

## Packaging and Running

Every app/lib registers itself in `FS_LIST` (`<build file>:<fs path>` pairs; apps land stripped at `bin/<name>`, extra data files can be added — see `apps/hello/app.mk`). `make fs` assembles these into `build-$(ARCH)/root_dir/` and then a 64MB FAT image `build-$(ARCH)/root.fat`, which `make test` attaches to QEMU as a virtio-blk device.

Inside the booted LK console, user binaries are driven with the `lkuser` command:

```
lkuser load /bin/rawtest a b c   # create a process: load the ELF, argv = the path and what follows
lkuser run [&]                   # start it (& = don't wait for it to exit)
lkuser root <path>               # set the fs root prefix for paths given to load
```

If adding a new architecture: add `arch/<arch>/arch.mk` (toolchain, flags, newlib multilib paths), a crt0 in `lib/lku/`, its trap stub in `lib/lku/syscalls.c`, the kernel's syscall entry for that arch in `sys/lib/lkuser/syscalls.cpp`, a project in `sys/project/`, and a QEMU invocation in the top-level `test` target (and `scripts/qemu-test.sh` for the gate).
