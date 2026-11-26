// SPDX-License-Identifier: GPL-2.0-only
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include "gzvm_platform.h"

static struct platform_device *gzvm_device;
static bool duchamp_verified;
module_param(duchamp_verified, bool, 0400);
MODULE_PARM_DESC(duchamp_verified, "Require firmware and device checks by the duchamp loader");

static const struct gzvm_platform_data duchamp_profile = {
	.version = GZVM_PLATFORM_DATA_V1,
	.quirks = GZVM_QUIRK_LEGACY_ESR | GZVM_QUIRK_LEGACY_RESERVED |
		  GZVM_QUIRK_LEGACY_SPI_INTID | GZVM_QUIRK_LEGACY_VTIMER,
};

static int __init gzvm_duchamp_init(void)
{
	const struct platform_device_info info = {
		.name = "gzvm",
		.id = PLATFORM_DEVID_NONE,
		.data = &duchamp_profile,
		.size_data = sizeof(duchamp_profile),
	};
	struct device_node *node;
	const char *status;
	bool disabled = false;

	if (!duchamp_verified || !of_machine_is_compatible("mediatek,MT6897"))
		return -ENODEV;

	node = of_find_node_by_path("/hypervisor");
	if (!node)
		return -ENODEV;
	if (of_device_is_compatible(node, "mediatek,geniezone-hyp") &&
	    !of_property_read_string(node, "status", &status))
		disabled = !strcmp(status, "disabled");
	of_node_put(node);
	if (!disabled)
		return -ENODEV;

	gzvm_device = platform_device_register_full(&info);
	if (IS_ERR(gzvm_device))
		return PTR_ERR(gzvm_device);

	return 0;
}

static void __exit gzvm_duchamp_exit(void)
{
	platform_device_unregister(gzvm_device);
}

module_init(gzvm_duchamp_init);
module_exit(gzvm_duchamp_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Register the disabled GenieZone platform device on duchamp");
MODULE_SOFTDEP("pre: gzvm");
