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
#include <zephyr/drivers/misc/tt_chiplet_remote_boot.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

LOG_MODULE_REGISTER(mis_launch_m, CONFIG_LOG_DEFAULT_LEVEL);

#ifdef CONFIG_SOC_TT_MIMIR_SMC
#define SOC_CHAR "M"
#endif
#ifdef CONFIG_SOC_TT_QUASAR_SMC
#define SOC_CHAR "Q"
#endif
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

/*
 * This remote's ready-ping pair, from the schema in tt_chiplet_remote_boot.h. The instance
 * comes from the CHIP_ID straps: RESET_UNIT_CHIP_ID carries only the chiplet type, so it
 * cannot tell Mimirs (or Quasars) apart.
 */
static uint32_t ready_pair(void)
{
	struct gr_smc_chipid id = gr_smc_get_chipid();

#ifdef CONFIG_SOC_TT_QUASAR_SMC
	return TT_CHIPLET_READY_PAIR_QUASAR(id.instance);
#else
	return TT_CHIPLET_READY_PAIR_MIMIR(id.instance);
#endif
}

/* Ping Keraunos that this chiplet is up in MIS boot, on its own outbound channel */
static int ping_keraunos_ready(uint32_t pair)
{
	const struct device *k_mbox = DEVICE_DT_GET(DT_NODELABEL(k_mbox));
	uint64_t payload = 0;
	struct mbox_msg msg = {
		.data = &payload,
		.size = sizeof(payload),
	};

	return mbox_send(k_mbox, TT_CHIPLET_READY_OUT(pair), &msg);
}

void launch_mis(void)
{
	const struct device *mbox = DEVICE_DT_GET(DT_NODELABEL(mbox0));
	struct k_sem go_sem;
	uint32_t pair = ready_pair();
	int ret;

	if (!device_is_ready(mbox)) {
		LOG_ERR("mbox0 device not ready");
		return;
	}

	/* Listen for Keraunos' go before pinging, so the go can't be missed */
	k_sem_init(&go_sem, 0, 1);

	ret = mbox_register_callback(mbox, MIS_MBOX_CHANNEL, mis_mbox_rx_cb, &go_sem);
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

	/* A) D2D is brought up from the staged FW and reported trained */
	LOG_INF("D2D trained (simulated)");

	/* C) Tell Keraunos we are in MIS boot and waiting for MIS FW */
	ret = ping_keraunos_ready(pair);
	if (ret != 0) {
		LOG_ERR("Failed to ping Keraunos ready on pair %u: %d", pair, ret);
		return;
	}
	LOG_INF(SOC_CHAR "-SMC waiting for MIS FW, pinged Keraunos on channel %u",
		TT_CHIPLET_READY_OUT(pair));

	/* Keraunos sends one go once it has copied MIS into place */
	LOG_INF("Waiting for Keraunos MIS go on mbox channel %d", MIS_MBOX_CHANNEL);
	k_sem_take(&go_sem, K_FOREVER);
	LOG_INF(SOC_CHAR "-SMC-MIS launch signalled");

	/* Jump to SMC-MIS. Does not return; SMC-MIS owns the core from here on. */
	gr_smc_jump_to(MIS_ENTRY_POINT);
}
