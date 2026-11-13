/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mis_launch.h"
#include <tenstorrent/gr_smc.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

LOG_MODULE_REGISTER(mis_launch_m, CONFIG_LOG_DEFAULT_LEVEL);

/* TODO: Better placement so we can re-place with one change instead of multiple. */
#define MIS_ENTRY_POINT (0xC0067000)

/* Keraunos signals on M's local mbox0, inbound channel 1: first to proceed, then for M-SMC-MIS */
#define MIS_MBOX_CHANNEL 1

static void mis_mbox_rx_cb(const struct device *dev, mbox_channel_id_t channel_id, void *user_data,
			   struct mbox_msg *msg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(msg);
	k_sem_give((struct k_sem *)user_data);
}

void launch_mis(void)
{
	const struct device *mbox = DEVICE_DT_GET(DT_NODELABEL(mbox0));
	struct k_sem ready_sem;
	int ret;

	if (!device_is_ready(mbox)) {
		LOG_ERR("mbox0 device not ready");
		return;
	}

	k_sem_init(&ready_sem, 0, 2);

	ret = mbox_register_callback(mbox, MIS_MBOX_CHANNEL, mis_mbox_rx_cb, &ready_sem);
	if (ret != 0) {
		LOG_ERR("Failed to register mbox callback: %d", ret);
		return;
	}

	ret = mbox_set_enabled(mbox, MIS_MBOX_CHANNEL, true);
	if (ret != 0) {
		LOG_ERR("Failed to enable mbox channel %d: %d", MIS_MBOX_CHANNEL, ret);
		(void)mbox_set_enabled(mbox, MIS_MBOX_CHANNEL, false);
		return;
	}

	LOG_INF("Waiting for Keraunos proceed signal on mbox channel %d", MIS_MBOX_CHANNEL);
	k_sem_take(&ready_sem, K_FOREVER);
	LOG_INF("Keraunos signalled proceed");

	LOG_INF("Waiting for M-SMC-MIS ready signal on mbox channel %d", MIS_MBOX_CHANNEL);
	k_sem_take(&ready_sem, K_FOREVER);
	LOG_INF("M-SMC-MIS signalled ready");

	/* Jump to M-SMC-MIS. Does not return; M-SMC-MIS owns the core from here on. */
	gr_smc_jump_to(MIS_ENTRY_POINT);
}
