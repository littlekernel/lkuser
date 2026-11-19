#!/usr/bin/env bash
#
# Copyright (c) 2026 Travis Geiselbrecht
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT
#
# Non-interactive boot test: boots the kernel with a scratch copy of the image attached, if one
# is given, which the kernel mounts read-only at /init, runs each app through lk.autorun and
# powers off. Passes only when QEMU exits on its own, the kernel never panicked, the image was
# not written, and every app exited with the code it was expected to, printing "<APP>: PASS"
# (the name upper-cased) if that code is 0. The console log and the scratch image are kept for
# inspection.
#
# Each app is name[:arg...][&][=code]: /init/bin/<name> runs with the arguments after it, so
# "rawtest:a:b:c" runs "lkuser load /init/bin/rawtest a b c", and is expected to exit with code
# (default 0), as "lkuser: process <pid> (<name>) exited with code <code>" reports it. Lines for
# processes the apps start themselves, under other names, are not counted. With "&" the app runs
# in the background ("lkuser start") while the ones after it run, and "lkuser wait" waits for
# every such app before QEMU powers off, so a server must end on its own.
#
# Each -b name[=code] is a process the kernel starts at boot (LKUSER_BOOT), checked the same way
# but not loaded; "lkuser wait" waits for it too, so it must end on its own, after the apps
# (a server is told to quit by the last app).
#
# With -i the boot processes run the apps and there is no autorun: the apps go to init on the
# kernel command line (lkuser.init, each without its =code), and lkuser.halt has the kernel
# power off once the boot processes are gone, printing the objects left. The run then also
# fails unless that dump lists only kernel endpoints and closed user endpoints.
#
# usage: qemu-test.sh -a <arch> -k <lk.elf> [-f <image>] -l <log> [-t <secs>] [-s <smp>] [-i]
#                     [-b name[=code]]... app...

set -u

arch=
kernel=
fsimage=
log=
timeout_secs=60
smp=1
boots=()
by_init=0

usage() {
    echo "usage: $0 -a <arch> -k <lk.elf> [-f <image>] -l <log> [-t <secs>] [-s <smp>] [-i] [-b name[=code]]... name[:arg...][&][=code]..." >&2
    exit 2
}

while getopts "a:k:f:l:t:s:b:i" opt; do
    case $opt in
        a) arch=$OPTARG ;;
        k) kernel=$OPTARG ;;
        f) fsimage=$OPTARG ;;
        l) log=$OPTARG ;;
        t) timeout_secs=$OPTARG ;;
        s) smp=$OPTARG ;;
        b) boots+=("$OPTARG") ;;
        i) by_init=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
apps=("$@")
specs=()
names=()
codes=()
backgrounds=()
any_background=0
for app in "${apps[@]}"; do
    spec=${app%%=*}
    code=0
    if [[ $app == *=* ]]; then
        code=${app#*=}
    fi
    if ! [[ $code =~ ^-?[0-9]+$ ]]; then
        echo "$0: '$app': the expected exit code must be a number" >&2
        exit 2
    fi
    background=0
    if [[ $spec == *'&' ]]; then
        spec=${spec%'&'}
        background=1
        any_background=1
    fi
    specs+=("$spec")
    names+=("${spec%%:*}")
    codes+=("$code")
    backgrounds+=("$background")
done
# boot processes are checked as apps are, after them in the lists
for boot in "${boots[@]}"; do
    name=${boot%%=*}
    code=0
    if [[ $boot == *=* ]]; then
        code=${boot#*=}
    fi
    if ! [[ $code =~ ^-?[0-9]+$ ]] || ! [[ $name =~ ^[A-Za-z0-9_.-]+$ ]]; then
        echo "$0: '-b $boot': expected name[=code]" >&2
        exit 2
    fi
    names+=("$name")
    codes+=("$code")
done

if [ -z "$arch" ] || [ -z "$kernel" ] || [ -z "$log" ] ||
    [ $((${#apps[@]} + ${#boots[@]})) -eq 0 ]; then
    usage
fi

case $arch in
    riscv)
        qemu=(qemu-system-riscv64 -machine virt -cpu rv64 -bios default)
        ;;
    riscv32)
        qemu=(qemu-system-riscv32 -machine virt -cpu rv32 -bios default)
        ;;
    arm64)
        qemu=(qemu-system-aarch64 -machine virt -cpu cortex-a76)
        ;;
    *)
        echo "$0: no automated test for ARCH=$arch (riscv, riscv32 and arm64 only)" >&2
        exit 2
        ;;
esac

for f in "$kernel" ${fsimage:+"$fsimage"}; do
    if [ ! -f "$f" ]; then
        echo "$0: $f does not exist" >&2
        exit 2
    fi
done

# '+' is a space to lib/cmdline; a literal space would end the argument
if [ $by_init = 1 ]; then
    cmdline="lkuser.halt"
    init=
    for i in "${!specs[@]}"; do
        init+="${init:++}${specs[$i]}"
        if [ "${backgrounds[$i]}" = 1 ]; then
            init+="&"
        fi
    done
    if [ -n "$init" ]; then
        cmdline+=" lkuser.init=$init"
    fi
else
    autorun=
    for i in "${!specs[@]}"; do
        IFS=: read -r -a parts <<<"${specs[$i]}"
        load="lkuser+load+/init/bin/${parts[0]}"
        for arg in "${parts[@]:1}"; do
            load+="+$arg"
        done
        if [ "${backgrounds[$i]}" = 1 ]; then
            autorun+="$load;lkuser+start;"
        else
            autorun+="$load;lkuser+run;"
        fi
    done
    if [ $any_background = 1 ] || [ ${#boots[@]} -gt 0 ]; then
        autorun+="lkuser+wait;"
    fi
    autorun+="poweroff"
    if [ ${#autorun} -gt 511 ]; then
        echo "$0: lk.autorun script is ${#autorun} bytes, over the 511 byte limit" >&2
        exit 2
    fi
    cmdline="lk.autorun=$autorun"
fi

mkdir -p "$(dirname "$log")"
drive=()
if [ -n "$fsimage" ]; then
    scratch_fs="${log%.log}.fat"
    cp -- "$fsimage" "$scratch_fs" || exit 2
    image_sum=$(sha256sum <"$scratch_fs")
    drive=(-drive if=none,file="$scratch_fs",id=blk,format=raw -device virtio-blk-device,drive=blk)
fi
echo "qemu-test: arch $arch, smp $smp, timeout ${timeout_secs}s, apps: ${apps[*]}${boots:+, boot: ${boots[*]}}$([ $by_init = 1 ] && echo ", by init")"

# no NIC: nothing here uses one, and arm64 virt's default virtio-net-pci needs an iPXE ROM
# (efi-virtio.rom) that a minimal QEMU install lacks
timeout --foreground "$timeout_secs" "${qemu[@]}" -m 512 -smp "$smp" -nographic -nic none \
    -kernel "$kernel" "${drive[@]}" -append "$cmdline" </dev/null 2>&1 | tee "$log"
qemu_status=${PIPESTATUS[0]}

# the guest console uses CRLF line endings
clean_log=$(tr -d '\r' <"$log")

fail=0
if [ "$qemu_status" -eq 124 ]; then
    echo "qemu-test: FAIL: timed out after ${timeout_secs}s"
    fail=1
elif [ "$qemu_status" -ne 0 ]; then
    echo "qemu-test: FAIL: qemu exited with status $qemu_status"
    fail=1
fi

if grep -qE 'panic \(caller|ASSERT FAILED' <<<"$clean_log"; then
    echo "qemu-test: FAIL: kernel panic"
    fail=1
fi

# what the kernel found left at the halt: the service endpoints, and those of user servers
# that have gone
if [ $by_init = 1 ]; then
    dump=$(sed -n '/^lkuser: halt: objects$/,/^lkuser: halt: end$/p' <<<"$clean_log")
    if ! grep -qx 'lkuser: halt: end' <<<"$dump"; then
        echo "qemu-test: FAIL: no halt-time object dump"
        fail=1
    else
        left=$(grep -vxE 'lkuser: halt: (objects|end)|endpoint +0x[0-9a-f]+ refs [0-9]+ handles 0 (kernel '"'"'[a-z]+'"'"'|user .* closed)' <<<"$dump")
        if [ -n "$left" ]; then
            echo "qemu-test: FAIL: objects left at the halt:"
            sed 's/^/qemu-test:   /' <<<"$left"
            fail=1
        fi
    fi
fi

# the image is mounted read-only
if [ -n "$fsimage" ] && [ "$(sha256sum <"$scratch_fs")" != "$image_sum" ]; then
    echo "qemu-test: FAIL: the image was written"
    fail=1
fi

for i in "${!names[@]}"; do
    token="${names[$i]^^}: PASS"
    if [ "${codes[$i]}" = 0 ] && ! grep -qxF "$token" <<<"$clean_log"; then
        echo "qemu-test: FAIL: no '$token'"
        fail=1
    fi
done

# every exit of a process named like an app, each with the code an entry expects
declare -A expected
for i in "${!names[@]}"; do
    key="${names[$i]} ${codes[$i]}"
    expected[$key]=$((${expected[$key]:-0} + 1))
done
for key in "${!expected[@]}"; do
    name=${key% *}
    code=${key#* }
    got=$(grep -cE "\($name\) exited with code $code\$" <<<"$clean_log")
    if [ "$got" -ne "${expected[$key]}" ]; then
        echo "qemu-test: FAIL: $got of ${expected[$key]} $name processes exited with code $code"
        fail=1
    fi
done
declare -A runs
for name in "${names[@]}"; do
    runs[$name]=$((${runs[$name]:-0} + 1))
done
for name in "${!runs[@]}"; do
    got=$(grep -cE "\($name\) exited with code -?[0-9]+\$" <<<"$clean_log")
    if [ "$got" -ne "${runs[$name]}" ]; then
        echo "qemu-test: FAIL: $got exits of $name processes, ${runs[$name]} expected"
        fail=1
    fi
done

if [ $fail -ne 0 ]; then
    echo "qemu-test: FAIL (log: $log)"
    exit 1
fi
echo "qemu-test: PASS (${names[*]})"
