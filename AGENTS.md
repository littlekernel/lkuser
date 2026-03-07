# LKUser Development Notes

`lkuser` is an overlay repository on top of:
- LK (`lk/` submodule) for kernel and base build system
- external libc (`newlib/` submodule, currently)

The repository adds user-space integration and extension code, especially `lib/lkuser`, while reusing LK's modular build flow.

## What This Repo Does

- Extends LK with extra functionality in `lib/lkuser`
- Builds both kernel-side and user-space artifacts
- Coordinates cross-repo build steps through top-level make targets

## Build Model

The top-level `makefile` orchestrates sub-makes and generated output trees.

Architecture is controlled by `ARCH` (default: `riscv` in the top-level `makefile`).
Agent guidance: do not assume host architecture; always check/propagate `ARCH` when invoking build or test targets.

Typical flow:
1. build libc/toolchain pieces (`newlib/` and related output dirs)
2. build LK kernel/app modules (via `lk/` and LK make system)
3. produce combined artifacts in `build-*` directories

Primary entrypoint is the top-level `makefile` in this repository.

## Key Paths

- `lk/`: LK kernel source and build system (git submodule)
- `newlib/`: libc source (git submodule)
- `lib/lkuser/`: overlay library added by this repo
- `apps/`, `sys/`: user-space and integration code
- `build-*` directories: generated outputs (do not treat as source)

## Editing Guidance

- Prefer editing overlay code in this repo first (`lib/lkuser`, `apps`, `sys`).
- Treat `lk/` and `newlib/` as upstream submodules unless intentional submodule changes are required.
- Avoid committing generated files from `build-*`, `install-*`, or tool output unless explicitly needed.
- Preserve compatibility with LK module conventions (`rules.mk`, `MODULE_DEPS`, etc.) when adding new code.

## Quick Commands

Run from repository root:

```bash
make
make clean
make spotless
make test
```

Use specific top-level targets as defined in `makefile` for libc/toolchain/kernel variants.

- `make spotless`: removes all local build outputs, including the newlib build/install trees.
- `make test`: builds LK + user-space artifacts, generates a FAT filesystem image, and starts QEMU.

For `make test`, QEMU launch is architecture-specific in the top-level `makefile`:
- `ARCH=arm` -> `qemu-system-arm`
- `ARCH=riscv` -> `qemu-system-riscv64`

If adding a new architecture, update top-level `test` handling and any architecture-specific toolchain/newlib settings under `arch/<arch>/`.
