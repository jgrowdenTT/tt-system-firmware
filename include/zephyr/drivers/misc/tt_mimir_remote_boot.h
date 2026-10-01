/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_MIMIR_REMOTE_BOOT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_MIMIR_REMOTE_BOOT_H_

#include <zephyr/device.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>

/**
 * @brief Copy a MIS image to every Mimir and signal each to jump to it.
 *
 * @param dev Mimir remote boot device
 * @param entry TOC entry of the Mimir MIS image (load address and length)
 * @param image Source address of the image data in the staged bundle
 *
 * @retval 0 on success
 * @retval -ENODEV if the device has no mis-mbox
 * @retval negative errno of the last mailbox failure
 */
int tt_mimir_remote_boot_launch_mis(const struct device *dev,
				    const struct fw_bundle_toc_entry *entry, const void *image);

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_MIMIR_REMOTE_BOOT_H_ */
