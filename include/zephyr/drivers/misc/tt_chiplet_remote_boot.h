/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_CHIPLET_REMOTE_BOOT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_CHIPLET_REMOTE_BOOT_H_

#include <zephyr/device.h>

/**
 * @brief Copy each remote chiplet's MIS image into place and signal it to jump to it.
 *
 * Reads the BUN3 TOC, and for every remote chiplet takes the entry at its mis-toc-index,
 * copies it to its mis-spa-base, then rings its mis-mbox.
 *
 * @param dev Chiplet remote boot device
 *
 * @retval 0 on success
 * @retval -ENOENT if a mis-toc-index is outside the BUN3 TOC
 * @retval -EINVAL if a mis-toc-index entry is not a MIS image
 * @retval negative errno of the last mailbox failure
 */
int tt_chiplet_remote_boot_launch_mis(const struct device *dev);

#endif /* ZEPHYR_INCLUDE_DRIVERS_MISC_TT_CHIPLET_REMOTE_BOOT_H_ */
