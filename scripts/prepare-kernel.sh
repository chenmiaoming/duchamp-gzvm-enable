#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: sh scripts/prepare-kernel.sh KERNEL_SOURCE NEW_KERNEL_OUTPUT" >&2
    exit 2
fi

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
source=$(CDPATH='' cd -- "$1" && pwd)
revision=$(cat "$root/build-inputs/commit")
[ "$(git -C "$source" rev-parse HEAD)" = "$revision" ] ||
    { echo "Wrong kernel source revision" >&2; exit 1; }

mkdir -p "$2"
output=$(CDPATH='' cd -- "$2" && pwd)
[ "$source" != "$output" ] && [ ! -e "$output/.config" ] ||
    { echo "Use a separate, fresh kernel output directory" >&2; exit 1; }

cp "$root/build-inputs/gki.config" "$output/.config"
awk '$4 ~ /^EXPORT_SYMBOL/ { print $2 }' "$root/build-inputs/Module.symvers" |
    LC_ALL=C sort -u > "$output/abi_symbollist.raw"
"$source/scripts/config" --file "$output/.config" \
    --set-str UNUSED_KSYMS_WHITELIST "$output/abi_symbollist.raw"
make -C "$source" O="$output" ARCH=arm64 LLVM="${LLVM:-1}" LOCALVERSION= \
    -j"${JOBS:-$(nproc)}" modules_prepare
grep -qx 'CONFIG_DEBUG_INFO_BTF_MODULES=y' "$output/include/config/auto.conf" ||
    { echo "The target module ABI requires CONFIG_DEBUG_INFO_BTF_MODULES=y. Check pahole." >&2; exit 1; }
cp "$root/build-inputs/Module.symvers" "$output/Module.symvers"
