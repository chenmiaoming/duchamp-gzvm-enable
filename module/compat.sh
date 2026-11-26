stock_gzvm=/system_dlkm/lib/modules/gzvm.ko

file_sha() {
    local result
    result=$(sha256sum "$1") || return
    printf '%s\n' "${result%% *}"
}

check_target() {
    if [ "$(/system/bin/getprop ro.product.device)" != "$TARGET_DEVICE" ] ||
       [ "$(/system/bin/getprop ro.product.vendor.device)" != "$TARGET_DEVICE" ] ||
       [ "$(/system/bin/getprop ro.build.version.incremental)" != "$TARGET_FIRMWARE" ] ||
       [ "$(uname -r)" != "$TARGET_RELEASE" ]; then
        echo "Unsupported device, firmware or kernel release"
        return 1
    fi
    if [ "$(file_sha "$stock_gzvm")" != "$STOCK_FILE_SHA" ]; then
        echo "Factory gzvm.ko does not match the supported firmware"
        return 1
    fi
}

matches_loaded() {
    local note="/sys/module/$1/notes/.note.gnu.build-id"
    [ -r "$note" ] && [ "$(file_sha "$note")" = "$2" ]
}

check_payload() {
    [ "$(file_sha "$module_dir/lib/gzvm.ko")" = "$DRIVER_FILE_SHA" ] &&
        [ "$(file_sha "$module_dir/lib/gzvm-duchamp.ko")" = "$ENABLER_FILE_SHA" ]
}
