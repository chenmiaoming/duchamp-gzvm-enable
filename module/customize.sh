module_dir=$MODPATH
. "$module_dir/target.conf" || abort "Missing target configuration"
. "$module_dir/build.conf" || abort "Missing build metadata"
. "$module_dir/compat.sh" || abort "Missing compatibility checks"

check_target || abort "This package supports only the listed duchamp build"
check_payload || abort "Kernel module checksum verification failed"

set_perm "$module_dir/boot-completed.sh" 0 0 0755
set_perm "$module_dir/lib/gzvm.ko" 0 0 0644
set_perm "$module_dir/lib/gzvm-duchamp.ko" 0 0 0644
