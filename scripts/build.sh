#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "Usage: sh scripts/build.sh KERNEL_SOURCE KERNEL_OUTPUT" >&2
    exit 2
fi

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
kernel_source=$(CDPATH='' cd -- "$1" && pwd)
kernel_output=$(CDPATH='' cd -- "$2" && pwd)
. "$root/module/target.conf"

release=$(sed -n 's/^#define UTS_RELEASE "\(.*\)"$/\1/p' \
    "$kernel_output/include/generated/utsrelease.h")
[ "$release" = "$TARGET_RELEASE" ] || { echo "Wrong prepared kernel release" >&2; exit 1; }
symvers_sha=$(sha256sum "$kernel_output/Module.symvers")
[ "${symvers_sha%% *}" = "$TARGET_SYMVERS_SHA" ] ||
    { echo "Wrong Module.symvers" >&2; exit 1; }

check_btf_config() {
    grep -qx 'CONFIG_DEBUG_INFO_BTF_MODULES=y' "$kernel_output/include/config/auto.conf" ||
        { echo "The target module ABI requires CONFIG_DEBUG_INFO_BTF_MODULES=y. Prepare the kernel with pahole." >&2; exit 1; }
}

pahole --version >/dev/null || { echo "pahole is required to preserve the target module ABI" >&2; exit 1; }
check_btf_config
make -C "$kernel_source" O="$kernel_output" M="$root/kernel" \
    ARCH=arm64 LLVM="${LLVM:-1}" LOCALVERSION= -j"${JOBS:-$(nproc)}" modules
check_btf_config
