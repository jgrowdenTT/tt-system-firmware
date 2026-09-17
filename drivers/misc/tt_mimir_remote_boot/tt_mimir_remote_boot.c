/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT tenstorrent_mimir_remote_boot

#include <errno.h>
#include <inttypes.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_d2d.h>
#include <zephyr/drivers/misc/tt_smc_remoteproc.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(tt_mimir_remote_boot, CONFIG_TT_MIMIR_REMOTE_BOOT_LOG_LEVEL);

BUILD_ASSERT(CONFIG_TT_MIMIR_REMOTE_BOOT_INIT_PRIO > CONFIG_TT_SMC_REMOTEPROC_INIT_PRIO,
	     "TT_MIMIR_REMOTE_BOOT_INIT_PRIO must be higher than TT_SMC_REMOTEPROC_INIT_PRIO");

struct tt_mimir_remote_boot_config {
	const struct device *const *remoteprocs;
	size_t num_remoteprocs;
	const struct device *const *d2ds;
	size_t num_d2ds;
	uint32_t fw_toc_index;
	uint32_t d2d_fw_toc_index;
	uint32_t gddr_params_toc_index;
};

struct tt_mimir_remote_boot_data {
};

static int tt_mimir_remote_boot_init(const struct device *dev)
{
	const struct tt_mimir_remote_boot_config *cfg = dev->config;

	int ret;

	/* A) Locate the Mimir image in the BUN2 staging area */
	const struct fw_bundle_manifest *manifest =
		(const struct fw_bundle_manifest *)TT_BUN2_STAGING_AREA_ADDR;
	const struct fw_bundle_toc *toc = (const struct fw_bundle_toc *)(TT_BUN2_STAGING_AREA_ADDR +
									 manifest->payload_offset);

	if (MAX(cfg->fw_toc_index, cfg->d2d_fw_toc_index) >= toc->image_count) {
		LOG_ERR("TOC index %u/%u out of range (bundle has %" PRIu64 " entries)",
			cfg->fw_toc_index, cfg->d2d_fw_toc_index, toc->image_count);
		return -ENOENT;
	}

	const struct fw_bundle_toc_entry *entry = &toc->entries[cfg->fw_toc_index];
	const struct fw_bundle_toc_entry *d2d_entry = &toc->entries[cfg->d2d_fw_toc_index];

	/* B) OCCP-load and start the Mimir image on each remote processor */
	LOG_INF("Loading Mimir FW from TOC[%u]: load_addr=0x%" PRIx64 " size=%" PRIu64,
		cfg->fw_toc_index, entry->load_addr, entry->length);

	for (size_t i = 0; i < cfg->num_remoteprocs; i++) {
		if (!device_is_ready(cfg->remoteprocs[i])) {
			LOG_ERR("remoteproc[%zu] is not ready", i);
			return -ENODEV;
		}
		/* D2D FW copy via OCCP */
		LOG_INF("OCCP load - D2D FW for remote proc [%zu]", i);
		ret = tt_smc_remoteproc_load(cfg->remoteprocs[i], TT_D2D_OCCP_STAGE_ADDR,
					     (uint8_t *)(TT_BUN2_STAGING_AREA_ADDR +
							 manifest->payload_offset +
							 d2d_entry->offset),
					     (size_t)d2d_entry->length);

		if (ret != 0) {
			LOG_ERR("Failed to OCCP-load D2D FW on remoteproc[%zu]: %d", i, ret);
			return ret;
		}

		/* OCCP load firmware bin */
		LOG_INF("OCCP load - SMC-BL1 FW for remote proc [%zu]", i);
		ret = tt_smc_remoteproc_load(cfg->remoteprocs[i], entry->load_addr,
					     (uint8_t *)(TT_BUN2_STAGING_AREA_ADDR +
							 manifest->payload_offset + entry->offset),
					     (size_t)entry->length);
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-load Mimir FW on remoteproc[%zu]: %d", i, ret);
			return ret;
		}
	}

	for (size_t i = 0; i < cfg->num_remoteprocs; i++) {
		/* Remote execute the M-SMC_BL1 */
		ret = tt_smc_remoteproc_boot(cfg->remoteprocs[i], entry->load_addr);
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-boot Mimir FW on remoteproc[%zu]: %d", i, ret);
			return ret;
		}
	}

	/* Load every paired local D2D tile before starting any of them. */
	for (size_t i = 0; i < cfg->num_d2ds; i++) {
		const struct device *d2d = cfg->d2ds[i];

		if (i >= cfg->num_remoteprocs || !device_is_ready(d2d)) {
			LOG_ERR("D2D[%zu] is not ready or has no remoteproc pair", i);
			return -ENODEV;
		}

		ret = tt_d2d_reset_release(d2d);
		if (ret != 0) {
			LOG_ERR("Failed to release D2D[%zu] reset: %d", i, ret);
			return ret;
		}

		ret = tt_d2d_load_fw(d2d,
				     (const uint8_t *)(TT_BUN2_STAGING_AREA_ADDR +
						       manifest->payload_offset +
						       d2d_entry->offset),
				     (size_t)d2d_entry->length);
		if (ret != 0) {
			LOG_ERR("Failed to load D2D[%zu] firmware: %d", i, ret);
			return ret;
		}
	}

	for (size_t i = 0; i < cfg->num_d2ds; i++) {
		ret = tt_d2d_start(cfg->d2ds[i]);
		if (ret != 0) {
			LOG_ERR("Failed to start D2D[%zu]: %d", i, ret);
			return ret;
		}
	}

	return 0;
}

/* TODO - New SYS_INIT step to copy GDDR params over to mimir once D2D is trained */

#define TT_MIMIR_REMOTE_BOOT_REMOTEPROC_GET(node_id, prop, idx)                                    \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define TT_MIMIR_REMOTE_BOOT_D2D_GET(node_id, prop, idx)                                           \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define TT_MIMIR_REMOTE_BOOT_D2D_CONFIG(inst)                                                      \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, d2d),                                              \
		(static const struct device *const tt_mimir_remote_boot_d2ds_##inst[] = {          \
			DT_INST_FOREACH_PROP_ELEM(inst, d2d, TT_MIMIR_REMOTE_BOOT_D2D_GET)         \
		};),                                                                               \
		())

#define TT_MIMIR_REMOTE_BOOT_DEFINE(inst)                                                          \
	static const struct device *const tt_mimir_remote_boot_remoteprocs_##inst[] = {            \
		DT_INST_FOREACH_PROP_ELEM(inst, smc_remoteproc,                                    \
					  TT_MIMIR_REMOTE_BOOT_REMOTEPROC_GET)};                   \
	TT_MIMIR_REMOTE_BOOT_D2D_CONFIG(inst)                                                      \
	static const struct tt_mimir_remote_boot_config tt_mimir_remote_boot_config_##inst = {     \
		.remoteprocs = tt_mimir_remote_boot_remoteprocs_##inst,                            \
		.num_remoteprocs = ARRAY_SIZE(tt_mimir_remote_boot_remoteprocs_##inst),            \
		.d2ds = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, d2d),                              \
					(tt_mimir_remote_boot_d2ds_##inst),                        \
					(NULL)),                                \
			 .num_d2ds = COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, d2d),                 \
					(ARRAY_SIZE(tt_mimir_remote_boot_d2ds_##inst)),            \
					(0)),              \
				  .fw_toc_index = DT_INST_PROP(inst, fw_toc_index),                \
				  .d2d_fw_toc_index = DT_INST_PROP(inst, d2d_fw_toc_index),        \
				  .gddr_params_toc_index =                                         \
					  DT_INST_PROP(inst, gddr_params_toc_index),               \
	};                                                                                         \
	static struct tt_mimir_remote_boot_data tt_mimir_remote_boot_data_##inst;                  \
	DEVICE_DT_INST_DEFINE(inst, tt_mimir_remote_boot_init, NULL,                               \
			      &tt_mimir_remote_boot_data_##inst,                                   \
			      &tt_mimir_remote_boot_config_##inst, POST_KERNEL,                    \
			      CONFIG_TT_MIMIR_REMOTE_BOOT_INIT_PRIO, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_MIMIR_REMOTE_BOOT_DEFINE)
