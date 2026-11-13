/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_MISC_TT_CHIPLET_REMOTE_BOOT_H_
#define ZEPHYR_INCLUDE_DRIVERS_MISC_TT_CHIPLET_REMOTE_BOOT_H_

#include <zephyr/device.h>

/*
 * Ready-ping channel schema. Each remote is a mailbox channel pair p, and its
 * "up" ping is written on that pair's outbound channel. The primary listens on
 * the matching inbound channel, which is the odd one of the pair.
 *
 *   Mimir M<x>   -> pair x
 *   Quasar Q<y>  -> pair 16 + y
 *
 * The mailbox has 32 pairs, so this leaves room for 16 Mimirs and 16 Quasars.
 */
#define TT_CHIPLET_READY_PAIR_MIMIR(x)  (x)
#define TT_CHIPLET_READY_PAIR_QUASAR(y) (16 + (y))

/* Remote side: outbound channel the remote writes its ready ping on */
#define TT_CHIPLET_READY_OUT(pair) (2 * (pair))
/* Primary side: inbound channel the primary listens on for that remote */
#define TT_CHIPLET_READY_IN(pair)  (2 * (pair) + 1)

/**
 * @brief Copy each remote chiplet's MIS image into place and signal it to jump to it.
 *
 * Reads the BUN3 TOC, and for every remote chiplet takes the entry at its mis-toc-index,
 * copies it to its mis-spa-base, then rings its mis-mbox. Remotes must already have pinged
 * ready (see TT_CHIPLET_READY_OUT()); that is handled during driver init.
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
