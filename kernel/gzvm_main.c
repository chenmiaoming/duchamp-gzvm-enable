// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#include <linux/device.h>
#include <linux/file.h>
#include <linux/kdev_t.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "gzvm_drv_v9_1.h"

struct gzvm_device {
	struct miscdevice misc;
	struct kref refcount;
	u32 quirks;
};

/* Defaults for platform devices that do not supply firmware ABI data. */
static bool legacy_esr = true;
module_param(legacy_esr, bool, 0444);
MODULE_PARM_DESC(legacy_esr, "Accept old exception ABI ESR_EL2 in reserved[6]");

static bool legacy_reserved;
module_param(legacy_reserved, bool, 0444);
MODULE_PARM_DESC(legacy_reserved,
		 "r0compat1: tolerate old six-word padding on validated translation faults");

static bool legacy_spi_intid;
module_param(legacy_spi_intid, bool, 0444);
MODULE_PARM_DESC(legacy_spi_intid, "Use architectural SPI INTIDs for legacy IRQ_LINE");

static bool legacy_vtimer;
module_param(legacy_vtimer, bool, 0444);
MODULE_PARM_DESC(legacy_vtimer, "Migrate the virtual timer using the legacy firmware ABI");

static u32 gzvm_default_quirks(void)
{
	return (legacy_esr ? GZVM_QUIRK_LEGACY_ESR : 0) |
	       (legacy_reserved ? GZVM_QUIRK_LEGACY_RESERVED : 0) |
	       (legacy_spi_intid ? GZVM_QUIRK_LEGACY_SPI_INTID : 0) |
	       (legacy_vtimer ? GZVM_QUIRK_LEGACY_VTIMER : 0);
}

/**
 * gzvm_err_to_errno() - Convert geniezone return value to standard errno
 *
 * @err: Return value from geniezone function return
 *
 * Return: Standard errno
 */
int gzvm_err_to_errno(unsigned long err)
{
	int gz_err = (int)err;

	switch (gz_err) {
	case 0:
		return 0;
	case ERR_NO_MEMORY:
		return -ENOMEM;
	case ERR_INVALID_ARGS:
		return -EINVAL;
	case ERR_NOT_SUPPORTED:
		return -EOPNOTSUPP;
	case ERR_NOT_IMPLEMENTED:
		return -EOPNOTSUPP;
	case ERR_FAULT:
		return -EFAULT;
	default:
		break;
	}

	return -EINVAL;
}

/**
 * gzvm_dev_ioctl_check_extension() - Check if given capability is support
 *				      or not
 *
 * @gzvm: Pointer to struct gzvm
 * @args: Pointer in u64 from userspace
 *
 * Return:
 * * 0			- Supported, no error
 * * -EOPNOTSUPP	- Unsupported
 * * -EFAULT		- Failed to get data from userspace
 */
long gzvm_dev_ioctl_check_extension(struct gzvm *gzvm, unsigned long args)
{
	__u64 cap;
	void __user *argp = (void __user *)args;

	if (copy_from_user(&cap, argp, sizeof(uint64_t)))
		return -EFAULT;
	/* v9.1 supports the ordinary demand-paged VM path only. */
	if (cap == GZVM_CAP_PROTECTED_VM)
		return -EOPNOTSUPP;
	return gzvm_arch_check_extension(gzvm, cap, argp);
}

static void gzvm_dev_free(struct kref *refcount)
{
	struct gzvm_device *dev = container_of(refcount, struct gzvm_device, refcount);

	kfree(dev);
}

static int gzvm_dev_open(struct inode *inode, struct file *filp)
{
	struct miscdevice *misc = filp->private_data;
	struct gzvm_device *dev = container_of(misc, struct gzvm_device, misc);

	kref_get(&dev->refcount);
	filp->private_data = dev;
	return 0;
}

static int gzvm_dev_release(struct inode *inode, struct file *filp)
{
	struct gzvm_device *dev = filp->private_data;

	kref_put(&dev->refcount, gzvm_dev_free);
	return 0;
}

static long gzvm_dev_ioctl(struct file *filp, unsigned int cmd,
			   unsigned long user_args)
{
	struct gzvm_device *dev = filp->private_data;
	long ret;

	switch (cmd) {
	case GZVM_CREATE_VM:
		ret = gzvm_dev_ioctl_create_vm(user_args, dev->quirks);
		return ret;
	case GZVM_CHECK_EXTENSION:
		if (!user_args)
			return -EINVAL;
		ret = gzvm_dev_ioctl_check_extension(NULL, user_args);
		return ret;
	default:
		break;
	}

	return -ENOTTY;
}

static const struct file_operations gzvm_chardev_ops = {
	.owner = THIS_MODULE,
	.open = gzvm_dev_open,
	.release = gzvm_dev_release,
	.unlocked_ioctl = gzvm_dev_ioctl,
	.llseek		= noop_llseek,
};

static int gzvm_drv_probe(struct platform_device *pdev)
{
	const struct gzvm_platform_data *pdata = dev_get_platdata(&pdev->dev);
	struct gzvm_device *dev;
	int ret;

	if (pdata && (pdata->version != GZVM_PLATFORM_DATA_V1 ||
		      (pdata->quirks & ~GZVM_SUPPORTED_QUIRKS))) {
		dev_err(&pdev->dev, "Unsupported platform data version %u or quirks %#x\n",
			pdata->version, pdata->quirks);
		return -EINVAL;
	}

	if (gzvm_arch_probe() != 0) {
		dev_err(&pdev->dev, "Not found available conduit\n");
		return -ENODEV;
	}

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;
	dev->quirks = pdata ? pdata->quirks : gzvm_default_quirks();
	dev->misc.minor = MISC_DYNAMIC_MINOR;
	dev->misc.name = KBUILD_MODNAME;
	dev->misc.fops = &gzvm_chardev_ops;
	kref_init(&dev->refcount);

	ret = misc_register(&dev->misc);
	if (ret) {
		kfree(dev);
		return ret;
	}

	platform_set_drvdata(pdev, dev);
	return 0;
}

static int gzvm_drv_remove(struct platform_device *pdev)
{
	struct gzvm_device *dev = platform_get_drvdata(pdev);

	/* Each open device file retains its configuration until release. */
	misc_deregister(&dev->misc);
	kref_put(&dev->refcount, gzvm_dev_free);
	return 0;
}

static const struct of_device_id gzvm_of_match[] = {
	{ .compatible = "mediatek,geniezone-hyp" },
	{/* sentinel */},
};

static struct platform_driver gzvm_driver = {
	.probe = gzvm_drv_probe,
	.remove = gzvm_drv_remove,
	.driver = {
		.name = KBUILD_MODNAME,
		.owner = THIS_MODULE,
		.of_match_table = gzvm_of_match,
	},
};

static int __init gzvm_init(void)
{
	int ret;

	/* irqfd shutdown may outlive platform-device unbinding. */
	ret = gzvm_drv_irqfd_init();
	if (ret)
		return ret;
	ret = platform_driver_register(&gzvm_driver);
	if (ret)
		gzvm_drv_irqfd_exit();
	else
		pr_info("gzvm_v9_1: host driver loaded; legacy SMCCC call IDs retained\n");
	return ret;
}

static void __exit gzvm_exit(void)
{
	platform_driver_unregister(&gzvm_driver);
	/* The module cannot unload while any device/VM/vCPU file is open. */
	gzvm_drv_irqfd_exit();
}

module_init(gzvm_init);
module_exit(gzvm_exit);

MODULE_DEVICE_TABLE(of, gzvm_of_match);
MODULE_AUTHOR("MediaTek");
MODULE_DESCRIPTION("GenieZone interface for VMM");
MODULE_LICENSE("GPL");
MODULE_VERSION("9.1");
