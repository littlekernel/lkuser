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

Each `TEST_APPS` entry is `name[:arg...]`: `rawtest:a:b:c` runs `lkuser load /bin/rawtest a b c`. The default is `rawtest:a:b:c calltest`, both without libc. `rawtest` covers user entry, the startup block and its handles, the trap ABI, `debug_write`'s checks on bad user pointers, unserved syscall numbers and exit. `calltest` covers handles and rights, endpoints, every transport check of `call`, a 64KB echo, and the vm, time and process services. `TEST_SMP` sets `-smp`. The console log is kept in `build-$(ARCH)/test-auto.log`.

A user fault ends its process. lkuser prints `lkuser: process <pid> (<name>) faulted: cause C, epc E, tval T`, and the process exits with `ERR_FAULT` (-40), which the gate counts as a failure. The gate cannot yet expect a non-zero exit code, so a fault is checked by hand: `TEST_APPS=hello` faults on a NULL malloc result.

Local make overrides can go in an untracked `local.mk` (auto-included).

## Architecture: Two Build Systems, One Makefile

The top-level `makefile` drives three mostly independent builds:

1. **newlib** — configured/built once per arch into `build-newlib-$(ARCH)` and installed into `install-newlib-$(ARCH)` (stamp-file gated; only rebuilt after `make spotless` or `make clean-newlib`). Configured with `--disable-newlib-supplied-syscalls`; its system calls come from this repo's `lib/lku`.
2. **LK kernel** — via `makefile.lk` (symlink to `lk/makefile`) with `PROJECT=qemu-$(ARCH)-usertest`. `lk_inc.mk` sets `LKINC := sys`, so LK's module system also searches `sys/`: the project files live in `sys/project/`, and the kernel-side module is `sys/lib/lkuser` (referenced as module `lib/lkuser` in LK's namespace — note repo-root `lib/` is a *different*, user-space directory). Output: `build-qemu-$(ARCH)-usertest/lk.elf`.
3. **User space** — the top-level makefile itself compiles apps and libs with its own simple rule system (not LK's): it `find`s every `apps/*/app.mk` and `lib/*/lib.mk`, which set `APP_SRCS`/`LIB_SRCS` etc. and include the shared `make/app.mk` / `make/lib.mk`. Those bind each app's `APP_CFLAGS`/`APP_INCLUDES`/... (and each lib's `LIB_*`) to its own objects, since the recipes run after the variables are cleared. Output goes to `build-$(ARCH)/`; the generated `.d` files are included, so header changes rebuild their users. Apps link with `ld`: the objects, then `--start-group` `APP_LIBS` (`lib/lku/lku.a`), newlib's `libc.a`/`libm.a` `--end-group`, then libgcc; crt0 comes out of `lku.a`, pulled in by the `_start` entry symbol. Per-arch flags, linker scripts, and newlib multilib paths are in `arch/<arch>/arch.mk`.

To add a user app: create `apps/<name>/app.mk` modeled on `apps/boottest/app.mk` (newlib) or `apps/calltest/app.mk` (no libc; `apps/include/lktest.h` has the string, output and `CHECK` helpers) — it is discovered automatically. Kernel-side code follows normal LK module conventions (`rules.mk`, `MODULE_DEPS`, ...). `lk/scripts/check-module-deps.py` does not cover `sys/lib/lkuser`: with the default `--root` it never reads the module's sources, and with `--root .` it names LK's modules `lk/lib/...` and reports every one as missing. Check a new `#include` against `rules.mk` by hand.

## The Kernel Side (`sys/lib/lkuser/`)

Kernel objects share `object` (`object.h`): an `lk::ref_counted` count, a kind and the number of handles naming the object, no virtual functions. Every object is made by `object::make<T>` and stays on a registry list (what `lkuser objs` walks) until its last release, which calls `object::destroy`; that unregisters it and deletes the concrete type by kind. Derived types are reached with `static_cast` after a kind check (each has a `kKind`), never `containerof`. The last handle to an object going runs its kind's last-handle action: an endpoint closes.

- `proc.cpp`: a process owns an LK user aspace, its handle table, its threads and its exit status. `proc::create` connects it to every registered kernel service and builds the startup block from argv and those connections. `start` maps and zeroes a 64KB main stack, copies the block to its top and starts the main thread. `exit` closes every handle and prints `lkuser: process <pid> (<name>) exited with code N` (the gate counts the `exited with code 0` suffix, so it stays last on the line). A reaper thread (started from an `LK_INIT_HOOK`) joins the threads of exited processes and frees their address spaces.
- `thread.cpp`: a user thread embeds its LK `thread_t` and the `pending_call` of its call in progress, sets `TLS_ENTRY_LKUSER` to itself and enters user space with `arch_enter_uspace`. It must outlive the LK thread, so it is dropped only after the reaper joins it.
- `handle.cpp`: the per-process handle table (`LKUSER_HANDLES`, 64 slots; handle h is slot h - 1, so 0 is never a handle, and the lowest free slot goes first), `handle_close` and the attenuating `handle_dup`. A full table refuses with `ERR_NO_RESOURCES` rather than growing. Rights are checked before kind: a call on an endpoint handle is `ERR_ACCESS_DENIED`, and a kind mismatch with the rights present is `ERR_BAD_HANDLE`.
- `endpoint.cpp`: endpoints, connections, `connect`, and `call` on a kernel endpoint, whose handler runs on the caller's thread in the caller's aspace (see Messages and services). A user endpoint can be created and connected to, but nothing receives on one until `recv` exists, so a call to it is `ERR_NOT_SUPPORTED`; once its last handle closes, `ERR_CHANNEL_CLOSED`.
- `msg.cpp`: `msg_buffer`, one side of a call as the caller's iovecs (at most `LK_IOV_MAX`, checked against the aspace when loaded), read or written at any offset through `copy.cpp`.
- `copy.cpp`: every access to user memory. A kernel-mode fault on a user address is fatal, so pages are looked up with `arch_mmu_query` under `vmm_aspace_lock` and copied through the physmap, returning `ERR_FAULT` for anything outside the aspace, unmapped, or lacking USER plus READ/WRITE. It works on any aspace, active or not. Syscall handlers receive user pointers as `user_ptr<T>`, which has no dereference. The pmm hands out pages as their last user left them, so every mapping given to user space (image pages, the main stack, `vm.ALLOC`) is cleared with `zero_user_pages` first.
- `svc/`: the kernel-resident services, one file each (`console`, `process`, `time`, `vm`, and the `echo` test service), registered at boot by `LKUSER_SERVICE`. `LKUSER_SERVICES` in `rules.mk` picks the files compiled in; `sys/project/qemu-riscv-usertest.mk` adds `echo`.
- `user.cpp`: the ELF loader (borrows the target aspace with `vmm_set_active_aspace` while `elf_load` writes the segments) and the `lkuser` console command.
- `syscalls.cpp`: the riscv syscall entry, the typed dispatcher, the rows not served elsewhere, and the user-fault hook: a strong `riscv_user_exception` that prints the fault, enables interrupts and ends the process with `ERR_FAULT`. Every process has one thread, so ending the faulting thread is all it takes.

Allocation failure is an error the caller sees, never a panic: every `lk::make_ref`, `object::make`, nothrow `new` and `malloc` is checked and unwinds with `ERR_NO_MEMORY`. Plain `new` (LK's panics) and asserts on allocation results are not used.

Invariants that are easy to break:
- No reference is dropped with a handle table's mutex, a spinlock or `THREAD_LOCK` held. The last release takes the registry mutex and frees memory, and the console holds the registry mutex while it takes a table's. `handle_table` moves a removed reference out and drops it after unlocking.
- A process's handle table is emptied by `proc::exit`, not by its destructor, so a handle naming the process itself cannot keep it alive.

## Syscall Mechanism (the key cross-file flow)

The raw syscall table is a single X-macro list: `sys/lib/lkuser/include/sys/_syscalls.h`, 21 rows of `LK_SYSCALL_DEF(num, ret, name, args...)`. `exit`, `thread_exit`, `handle_close`, `handle_dup`, `call`, `msg_read`, `msg_write`, `endpoint_create`, `connect` and `debug_write` have handlers (`msg_read`/`msg_write` return `ERR_NOT_FOUND` for every token, since tokens come only from `recv`); every other row returns `ERR_NOT_SUPPORTED`, as does a number past the table. Shared definitions live beside it:
- `sys/lk_abi.h`: handle and token types, the handle rights, the startup block and its tags, and the width and argument-width rules.
- `sys/lk_msg.h`: the message header, the impulse, and the argument structures of the IPC rows.
- `sys/svc/*.h`: one protocol per service.
- `sys/_errors.h`: LK's status codes as `LK_ERR_DEF(name, value)` rows. `sys/lk_err.h` expands them to enumerators for user space, and the kernel `static_assert`s each row against `<lk/err.h>` in `syscalls.cpp`, so a code that drifts from LK fails the build. The kernel never includes `lk_err.h`, whose names collide with LK's macros.

These headers compile on both sides, so they use only `<stdint.h>`/`<stddef.h>` types.

Trap ABI (riscv): number in `a7`, arguments in `a0`-`a5`, the result in `a0` as a single status (`NO_ERROR` or a negative `ERR_*`, never an errno). Arguments are register width by preference: handles, rights and counts 32-bit, lengths `size_t`, tokens and cookies `uintptr_t`. An inherently 64-bit value may take a register pair on 32-bit, but neither the stubs nor the dispatcher map one yet (the dispatcher asserts that each argument fits one register), so the first such row has to add that mapping; times travel in the argument structures. Interrupts are enabled while a handler runs, and the thread is marked as inside a syscall.

- **Kernel side**: `syscalls.h` declares one `sys_<name>` per row from the row itself, with each pointer argument turned into a `user_ptr<T>`; a handler defined with another signature fails to link. `syscalls.cpp` converts the registers to the declared argument types (integers keep their low bits) and dispatches by number.
- **User side**: `lib/lku/syscalls.c` expands each row into a naked `lk_<name>` stub (`li a7, n; ecall; ret`); `lib/lku/include/lk/ipc.h` declares them.

To add a syscall: add a row with the next number, then define its `sys_<name>` handler in the kernel (replacing the `ERR_NOT_SUPPORTED` stub if the row exists). Both sides recompile from the same header, so numbers and signatures can never skew.

## Messages and Services

A request and a reply each start with `struct lk_msg_hdr` (`op`, payload `len`, `nhandles`, and `status` in the reply) and are laid out by the caller as iovecs in `struct lk_call_args`. `call` returns the transport's status, and the service's verdict is the reply header's `status`, so a client can tell "the call failed" from "the service said no".

Before any service sees a request, the transport checks it and answers `ERR_INVALID_ARGS` for each of these:
- a header that doesn't fit, or a reply buffer that can't take one;
- more than `LK_IOV_MAX` iovecs on either side;
- an op at or above `LK_OP_KERNEL_BASE`;
- a header `len` or `nhandles` that disagrees with the call.

Other transport answers: `ERR_FAULT` for an iovec outside the aspace, `ERR_NOT_SUPPORTED` for carried handles (not yet), and `ERR_BAD_HANDLE` or `ERR_ACCESS_DENIED` for the connection. Kernel endpoints ignore `timeout_ns`: their handlers run to completion.

A kernel service is a `kernel_handler_t` taking a `kcall`, registered with `LKUSER_SERVICE(name, tag, handler)` in its own `svc/<name>.cpp`:
- `args<T>()` reads a fixed-size request (`ERR_INVALID_ARGS` unless the payload is exactly a `T`), and `reply(t)` writes a fixed-size reply (`ERR_NOT_ENOUGH_BUFFER` unless it fits).
- `read`/`write` copy the payload at any offset, clipped at the end, for streams of any size.
- The handler returns the service status. The kernel then writes the reply header, with `len` being how far the handler wrote.
- A fault on the caller's buffers fails the whole call with `ERR_FAULT`, whatever the handler returned.

Every process the console starts is born with one connection per compiled-in service, under the service's startup tag, with `CALL IMPULSE WAIT DUP TRANSFER`. To add a service:
1. Write its protocol in `sys/svc/<name>.h`. Payloads are wire structs, so they get size and offset asserts.
2. Pick a tag: `sys/lk_abi.h` for a real service, from `LK_TAG_TEST_BASE` up for a test one.
3. Add the handler file and its name to `LKUSER_SERVICES`.

## User Space (`lib/lku`)

- `crt0_riscv.S`: `_start` sets `gp` and calls `_start_c` with `sp` (the startup block) in `a0`.
- `msg.c`: `lk_msg_call` (one call with a contiguous payload and reply, header filled in and stripped off) and the service clients declared in `<lk/services.h>` (`lk_console_write`, `lk_vm_alloc`, `lk_time_sleep`, ...). Each returns the transport's status if the call failed, else the service's.
- `newlib.c`: `_start_c` for newlib programs (argc/argv from the startup block, libc init, `exit(main(argc, argv))`) and newlib's system calls. Only `_exit` reaches the kernel; the others fail with `ENOSYS` (`_sbrk` with `ENOMEM`), so newlib programs such as `boottest` and `hello` build but cannot print or allocate, and are not in the gate.
- A program without libc (`apps/rawtest`, `apps/calltest`) defines its own `_start_c`, so `newlib.o` never links in.

The startup block (`struct lk_proc_args` in `sys/lk_abi.h`): `nargs` and `nhandles`, then the argument offsets, then (tag, handle) pairs, then the strings; offsets count from the start of the block. `lk_proc_args_find(args, tag)` looks a handle up by its tag.

## Packaging and Running

Every app/lib registers itself in `FS_LIST` (`<build file>:<fs path>` pairs; apps land stripped at `bin/<name>`, extra data files can be added — see `apps/hello/app.mk`). `make fs` assembles these into `build-$(ARCH)/root_dir/` and then a 64MB FAT image `build-$(ARCH)/root.fat`, which `make test` attaches to QEMU as a virtio-blk device.

Inside the booted LK console, user binaries are driven with the `lkuser` command:

```
lkuser load /bin/rawtest a b c   # create a process: load the ELF, argv = the path and what follows
lkuser run [&]                   # start it (& = don't wait for it to exit)
lkuser root <path>               # set the fs root prefix for paths given to load
lkuser objs [-f]                 # every kernel object: kind, refs, handles, what it is
lkuser handles <pid> [-f]        # a process's handles and their rights
```

`-f` takes no locks, for when something is wedged holding one. After every process has exited, `lkuser objs` should list only the kernel service endpoints.

If adding a new architecture: add `arch/<arch>/arch.mk` (toolchain, flags, newlib multilib paths), a crt0 in `lib/lku/`, its trap stub in `lib/lku/syscalls.c`, the kernel's syscall entry and user-fault hook for that arch in `sys/lib/lkuser/syscalls.cpp`, a project in `sys/project/`, and a QEMU invocation in the top-level `test` target (and `scripts/qemu-test.sh` for the gate).
