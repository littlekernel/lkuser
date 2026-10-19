#!/usr/bin/env bash
#
# Copyright (c) 2026 Travis Geiselbrecht
#
# Use of this source code is governed by a MIT-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/MIT
#
# Non-interactive boot test: boots the kernel with a scratch copy of the FAT image attached
# (the tests write to it, so every run starts from the image as built), runs each app through
# lk.autorun and powers off. Passes only when QEMU exits on its own, the kernel never panicked,
# and every app exited with the code it was expected to, printing "<APP>: PASS" (the name
# upper-cased) if that code is 0. The console log and the scratch image are kept for
# inspection.
#
# Each app is name[:arg...][&][=code]: /bin/<name> runs with the arguments after it, so
# "rawtest:a:b:c" runs "lkuser load /bin/rawtest a b c", and is expected to exit with code
# (default 0), as "lkuser: process <pid> (<name>) exited with code <code>" reports it. Lines for
# processes the apps start themselves, under other names, are not counted. With "&" the app runs
# in the background ("lkuser start") while the ones after it run, and "lkuser wait" waits for
# every such app before the image is unmounted, so a server must end on its own.
#
# usage: qemu-test.sh -a <arch> -k <lk.elf> -f <root.fat> -l <log> [-t <secs>] [-s <smp>] app...

set -u

arch=
kernel=
fsimage=
log=
timeout_secs=60
smp=1

usage() {
    echo "usage: $0 -a <arch> -k <lk.elf> -f <root.fat> -l <log> [-t <secs>] [-s <smp>] name[:arg...][&][=code]..." >&2
    exit 2
}

while getopts "a:k:f:l:t:s:" opt; do
    case $opt in
        a) arch=$OPTARG ;;
        k) kernel=$OPTARG ;;
        f) fsimage=$OPTARG ;;
        l) log=$OPTARG ;;
        t) timeout_secs=$OPTARG ;;
        s) smp=$OPTARG ;;
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

if [ -z "$arch" ] || [ -z "$kernel" ] || [ -z "$fsimage" ] || [ -z "$log" ] || [ ${#apps[@]} -eq 0 ]; then
    usage
fi

case $arch in
    riscv)
        qemu=(qemu-system-riscv64 -machine virt -cpu rv64 -bios default)
        ;;
    *)
        echo "$0: no automated test for ARCH=$arch (riscv only)" >&2
        exit 2
        ;;
esac

for f in "$kernel" "$fsimage"; do
    if [ ! -f "$f" ]; then
        echo "$0: $f does not exist" >&2
        exit 2
    fi
done

# '+' is a space to lib/cmdline; a literal space would end the argument
autorun="mount+/fs+fat+virtio0;lkuser+root+/fs;"
for i in "${!specs[@]}"; do
    IFS=: read -r -a parts <<<"${specs[$i]}"
    load="lkuser+load+/bin/${parts[0]}"
    for arg in "${parts[@]:1}"; do
        load+="+$arg"
    done
    if [ "${backgrounds[$i]}" = 1 ]; then
        autorun+="$load;lkuser+start;"
    else
        autorun+="$load;lkuser+run;"
    fi
done
if [ $any_background = 1 ]; then
    autorun+="lkuser+wait;"
fi
# unmounted so the scratch image is left clean for mtools and fsck.fat
autorun+="unmount+/fs;poweroff"
if [ ${#autorun} -gt 511 ]; then
    echo "$0: lk.autorun script is ${#autorun} bytes, over the 511 byte limit" >&2
    exit 2
fi

mkdir -p "$(dirname "$log")"
scratch_fs="${log%.log}.fat"
cp -- "$fsimage" "$scratch_fs" || exit 2
echo "qemu-test: arch $arch, smp $smp, timeout ${timeout_secs}s, apps: ${apps[*]}"

timeout --foreground "$timeout_secs" "${qemu[@]}" -m 512 -smp "$smp" -nographic \
    -kernel "$kernel" \
    -drive if=none,file="$scratch_fs",id=blk,format=raw -device virtio-blk-device,drive=blk \
    -append "lk.autorun=$autorun" </dev/null 2>&1 | tee "$log"
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
