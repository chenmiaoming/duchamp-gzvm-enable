#!/system/bin/sh

module_dir=${0%/*}
umask 077
exec >"$module_dir/activation.log" 2>&1

. "$module_dir/target.conf" || exit 1
. "$module_dir/build.conf" || exit 1
. "$module_dir/compat.sh" || exit 1
check_target || exit 1
check_payload || { echo "Kernel module checksum verification failed"; exit 1; }

lock=/dev/.duchamp-gzvm-activation
mkdir "$lock" || { echo "Activation already in progress"; exit 1; }
trap 'rmdir "$lock"' EXIT

is_ready() {
    [ -c /dev/gzvm ] &&
        [ -L /sys/bus/platform/drivers/gzvm/gzvm ] &&
        matches_loaded gzvm "$DRIVER_NOTE_SHA" &&
        matches_loaded gzvm_duchamp "$ENABLER_NOTE_SHA"
}

if is_ready; then
    echo "GenieZone is already enabled"
    exit 0
fi

if [ -e /dev/gzvm ] || [ -L /sys/bus/platform/drivers/gzvm/gzvm ] ||
   [ -d /sys/module/gzvm_test ] || [ -d /sys/module/gzvm_duchamp ]; then
    echo "A device or enabler is already present. Leaving it in place"
    exit 1
fi

removed_factory=0
added_driver=0
added_enabler=0

rollback() {
    if [ "$added_driver" = 1 ] &&
       { [ "$(cat /sys/module/gzvm/parameters/active_vms)" != 0 ] ||
         [ "$(cat /sys/module/gzvm/parameters/quarantined_vms)" != 0 ]; }; then
        return 1
    fi
    if [ "$added_enabler" = 1 ]; then
        /system/bin/rmmod gzvm_duchamp || return 1
    fi
    if [ "$added_driver" = 1 ]; then
        /system/bin/rmmod gzvm || return 1
    fi
    if [ "$removed_factory" = 1 ]; then
        /system/bin/insmod "$stock_gzvm" || return 1
    fi
}

if [ -d /sys/module/gzvm ]; then
    if ! matches_loaded gzvm "$STOCK_NOTE_SHA" ||
       [ "$(cat /sys/module/gzvm/refcnt)" != 0 ]; then
        echo "The loaded gzvm is in use or differs from the factory module"
        exit 1
    fi
    /system/bin/rmmod gzvm || { echo "Factory gzvm could not be detached"; exit 1; }
    removed_factory=1
fi

if /system/bin/insmod "$module_dir/lib/gzvm.ko"; then
    added_driver=1
    if /system/bin/insmod "$module_dir/lib/gzvm-duchamp.ko" duchamp_verified=1; then
        added_enabler=1
        for attempt in 1 2 3 4 5; do
            if is_ready; then
                echo "GenieZone enabled on $TARGET_DEVICE"
                exit 0
            fi
            sleep 1
        done
    fi
fi

echo "Activation failed"
if rollback; then
    echo "Restored previous module state"
else
    echo "Could not roll back. See kernel logs for the module state"
fi
exit 1
