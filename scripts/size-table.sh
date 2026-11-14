#!/usr/bin/env bash
#
# Copyright (c) 2026 Travis Geiselbrecht
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT
#
# The size of the executive (sys/lib/lkuser) per source file, at LK_DEBUG=0 and at the build's
# usual LK_DEBUG=2, with the budgets the plan sets: objects, handles, endpoints and wait within
# 12KB of code on riscv64, and each kernel service within 3KB. Reported, not gated: the script
# fails only if a build does. code is every .text section, rodata every .rodata/.srodata one.
# The LK_DEBUG=0 kernel builds into build-<project>-debug0, so the gate's build is left alone.
# LK_MAKEARGS in the environment goes to both of LK's makes: the direct projects need the
# makefile's, which name the image they embed, so run those through `make size-table`.
#
# usage: size-table.sh [project]   (default qemu-riscv-usertest)
#        make size-table [ARCH=...] [PROFILE=...]

set -eu

project=${1:-qemu-riscv-usertest}
case $project in
    qemu-arm64-*) prefix=aarch64-elf- ;;
    *) prefix=riscv64-elf- ;;
esac
size=${TOOLCHAIN_PREFIX:-$prefix}size
read -r -a lkargs <<< "${LK_MAKEARGS:-}"
case $project in
    *-direct)
        if [ ${#lkargs[@]} -eq 0 ]; then
            echo "size-table.sh: $project embeds the boot image; use make size-table PROFILE=direct" >&2
            exit 1
        fi
        ;;
esac
# under make size-table, make's own jobserver; run by hand, one job per cpu
jobs=-j$(nproc)
case ${MAKEFLAGS:-} in
    *jobserver*) jobs= ;;
esac

PROJECT=$project make -f makefile.lk $jobs "${lkargs[@]}" >/dev/null
PROJECT=$project LK_DEBUG=0 BUILDDIR_SUFFIX=-debug0 make -f makefile.lk $jobs "${lkargs[@]}" >/dev/null

# "<code> <rodata>" of one object file
sizes() {
    "$size" -A "$1" | awk '
        $1 ~ /^\.text/ { code += $2 }
        $1 ~ /^\.(s)?rodata/ { ro += $2 }
        END { printf "%d %d\n", code, ro }'
}

dir0=build-$project-debug0/sys/lib/lkuser
dir2=build-$project/sys/lib/lkuser

printf '%-22s %14s %14s\n' "$project" "LK_DEBUG=0" "LK_DEBUG=2"
printf '%-22s %7s %6s %7s %6s\n' "file" "code" "ro" "code" "ro"
declare -A code0 code2
total0=0
total2=0
for o in $(cd "$dir2" && find . -name '*.cpp.o' | sed 's|^\./||' | sort); do
    read -r c0 r0 < <(sizes "$dir0/$o")
    read -r c2 r2 < <(sizes "$dir2/$o")
    name=${o%.cpp.o}
    code0[$name]=$c0
    code2[$name]=$c2
    total0=$((total0 + c0))
    total2=$((total2 + c2))
    printf '%-22s %7d %6d %7d %6d\n' "$name" "$c0" "$r0" "$c2" "$r2"
done
printf '%-22s %7d %6s %7d\n' "total" "$total0" "" "$total2"

echo
core0=0
core2=0
for f in object handle endpoint wait; do
    core0=$((core0 + ${code0[$f]:-0}))
    core2=$((core2 + ${code2[$f]:-0}))
done
verdict() {
    if [ "$1" -le "$2" ]; then echo "within"; else echo "OVER"; fi
}
printf 'objects+handles+endpoints+wait: %d at LK_DEBUG=0, %d at LK_DEBUG=2; budget 12288: %s\n' \
    "$core0" "$core2" "$(verdict "$core0" 12288)"
for name in "${!code0[@]}"; do
    case $name in
        svc/svc) ;;
        svc/*)
            printf 'service %-12s %5d at LK_DEBUG=0, %5d at LK_DEBUG=2; budget 3072: %s\n' "${name#svc/}" \
                "${code0[$name]}" "${code2[$name]}" "$(verdict "${code0[$name]}" 3072)"
            ;;
    esac
done | sort
