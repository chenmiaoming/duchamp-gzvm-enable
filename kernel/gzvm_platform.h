/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __GZVM_PLATFORM_H__
#define __GZVM_PLATFORM_H__

#include <linux/bits.h>
#include <linux/types.h>

#define GZVM_PLATFORM_DATA_V1	1

#define GZVM_QUIRK_LEGACY_ESR		BIT(0)
#define GZVM_QUIRK_LEGACY_RESERVED	BIT(1)
#define GZVM_QUIRK_LEGACY_SPI_INTID	BIT(2)
#define GZVM_QUIRK_LEGACY_VTIMER		BIT(3)

#define GZVM_SUPPORTED_QUIRKS		(GZVM_QUIRK_LEGACY_ESR | \
					 GZVM_QUIRK_LEGACY_RESERVED | \
					 GZVM_QUIRK_LEGACY_SPI_INTID | \
					 GZVM_QUIRK_LEGACY_VTIMER)

/* Optional platform data for a GenieZone platform device. */
struct gzvm_platform_data {
	u32 version;
	u32 quirks;
};

#endif /* __GZVM_PLATFORM_H__ */
