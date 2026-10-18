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

Architecture is selected with `ARCH` (default `riscv` in the top-level `makefile`); e.g. `ARCH=arm make -j $(nproc)`. Never assume host architecture — always check/propagate `ARCH` on build and test targets. Toolchain prefixes come from `arch/<arch>/arch.mk`: `riscv64-elf-` for riscv, `arm-eabi-` for arm (overridable via `TOOLCHAIN_PREFIX`, and CI fetches prebuilt ones from newos.org/toolchains). Building the FAT image requires mtools (`mformat`/`mcopy`); `make test` requires `qemu-system-riscv64` or `qemu-system-arm`.

`make test` is an interactive QEMU boot. `make test-auto` is the gate (`scripts/qemu-test.sh`, riscv only): an `lk.autorun` script mounts the image, runs each app in `TEST_APPS` with `lkuser load`/`run`, and powers off, with stdin closed. It passes only if:
- QEMU powered off by itself within `TEST_TIMEOUT` seconds (default 60; a run takes well under a second),
- no kernel panic or assert was logged,
- every app printed the exact line `<APP>: PASS` (name upper-cased) and its process logged `exited with code 0`.

The console log is kept in `build-$(ARCH)/test-auto.log`. `TEST_APPS` defaults to `boottest` (static data, float, malloc, sbrk, exit; reads no input); `TEST_SMP` sets `-smp`.

Local make overrides can go in an untracked `local.mk` (auto-included).

## Architecture: Two Build Systems, One Makefile

The top-level `makefile` drives three mostly independent builds:

1. **newlib** — configured/built once per arch into `build-newlib-$(ARCH)` and installed into `install-newlib-$(ARCH)` (stamp-file gated; only rebuilt after `make spotless` or `make clean-newlib`). Configured with `--disable-newlib-supplied-syscalls`; syscalls come from this repo's `lib/lku`.
2. **LK kernel** — via `makefile.lk` (symlink to `lk/makefile`) with `PROJECT=qemu-$(ARCH)-usertest`. `lk_inc.mk` sets `LKINC := sys`, so LK's module system also searches `sys/`: the project files live in `sys/project/`, and the kernel-side module is `sys/lib/lkuser` (referenced as module `lib/lkuser` in LK's namespace — note repo-root `lib/` is a *different*, user-space directory). Output: `build-qemu-$(ARCH)-usertest/lk.elf`.
3. **User space** — the top-level makefile itself compiles apps and libs with its own simple rule system (not LK's): it `find`s every `apps/*/app.mk` and `lib/*/lib.mk`, which set `APP_SRCS`/`LIB_SRCS` etc. and include the shared `make/app.mk` / `make/lib.mk`. Output goes to `build-$(ARCH)/`. Apps link against `lib/lku` (crt0 + syscall stubs), newlib's `libc.a`/`libm.a`, and libgcc. Per-arch flags, linker scripts, and newlib multilib paths are in `arch/<arch>/arch.mk`.

To add a user app: create `apps/<name>/app.mk` modeled on `apps/hello/app.mk` — it is discovered automatically. Kernel-side code follows normal LK module conventions (`rules.mk`, `MODULE_DEPS`, ...).

## Syscall Mechanism (the key cross-file flow)

The syscall API is a single X-macro list: `sys/lib/lkuser/include/sys/_syscalls.h`, rows of `LK_SYSCALL_DEF(num, ret, name, args...)`. It is expanded in two places:

- **Kernel side**: `sys/lib/lkuser/syscalls.cpp` implements the handlers; `sys/lib/lkuser/include/sys/lkuser_syscalls.h` builds the syscall table.
- **User side**: `lib/lku/liblk.c` expands each row into a trap stub (`svc` on arm, `ecall` on riscv, syscall number in r12/t0) and layers newlib's `_read`/`_write`/`_sbrk`/... retargeting on top.

To add a syscall: add a row to `_syscalls.h` with the next number, implement it in `syscalls.cpp`, and wire any libc-facing wrapper in `liblk.c`. Both sides recompile from the same header, so numbers can never skew.

Kernel-side runtime (`sys/lib/lkuser/`): `user.cpp` loads ELF binaries from the filesystem via LK's `lib/elf`/`lib/fs`/`lib/bio`, `proc.cpp`/`thread.cpp` manage processes, address spaces, and a reaper thread (started from an `LK_INIT_HOOK`).

## Packaging and Running

Every app/lib registers itself in `FS_LIST` (`<build file>:<fs path>` pairs; apps land stripped at `bin/<name>`, extra data files can be added — see `apps/hello/app.mk`). `make fs` assembles these into `build-$(ARCH)/root_dir/` and then a 64MB FAT image `build-$(ARCH)/root.fat`, which `make test` attaches to QEMU as a virtio-blk device.

Inside the booted LK console, user binaries are driven with the `lkuser` command:

```
lkuser load /bin/hello    # load an ELF from the mounted image
lkuser run [&]            # start it (& = don't wait)
lkuser root <path>        # set the fs root prefix for user-space file access
```

If adding a new architecture: add `arch/<arch>/arch.mk` (toolchain, flags, newlib multilib paths), a crt0 in `lib/lku/`, a syscall stub variant in `liblk.c`, a project in `sys/project/`, and a QEMU invocation in the top-level `test` target.
