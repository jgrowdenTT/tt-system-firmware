/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT tenstorrent_mimir_remote_boot

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_d2d.h>
#include <zephyr/drivers/misc/tt_grendel_i3c.h>
#include <zephyr/drivers/misc/tt_mimir_remote_boot.h>
#include <zephyr/drivers/misc/tt_smc_remoteproc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(tt_mimir_remote_boot, CONFIG_TT_MIMIR_REMOTE_BOOT_LOG_LEVEL);

BUILD_ASSERT(CONFIG_TT_MIMIR_REMOTE_BOOT_INIT_PRIO > CONFIG_TT_SMC_REMOTEPROC_INIT_PRIO,
	     "TT_MIMIR_REMOTE_BOOT_INIT_PRIO must be higher than TT_SMC_REMOTEPROC_INIT_PRIO");

/* The remote die has to boot its own firmware before it can answer at all. */
#define TT_MIMIR_REMOTE_READY_TIMEOUT_MS 5000

/*
 * A single command on the remote die, once its target is answering. D2D_INIT
 * copies the whole D2D image, which takes seconds under emulation, so this is
 * sized for that rather than for silicon.
 */
#define TT_MIMIR_REMOTE_CMD_TIMEOUT_MS 30000

/* Writing the mis-mbox outbound channel 0 prompts Mimir to jump to its MIS image */
#define TT_MIMIR_MIS_MBOX_CHANNEL 0

/* Base of the core-local address window that image load addresses are relative to */
#define TT_MIMIR_SMC_LOCAL_BASE 0xC0000000ULL

struct tt_mimir_local_d2d_link {
	const struct device *local_d2d;
	const struct device *remote_i3c;
	uint8_t remote_instance;
};

struct tt_mimir_remote_d2d_link {
	const struct device *remote_a_i3c;
	uint8_t remote_a_instance;
	const struct device *remote_b_i3c;
	uint8_t remote_b_instance;
};

struct tt_mimir_remote_boot_config {
	const struct device *const *remoteprocs;
	size_t num_remoteprocs;
	const struct tt_mimir_local_d2d_link *local_links;
	size_t num_local_links;
	const struct tt_mimir_remote_d2d_link *remote_links;
	size_t num_remote_links;
	const struct device *const *mis_mboxes;
	size_t num_mis_mboxes;
	/* {hi, lo} 32-bit cell pairs, one per mis_mboxes entry */
	const uint32_t *mis_spa_bases;
	size_t num_mis_spa_bases;
	/* Pairs: [2n]/[2n+1] are the two remote ends of remote-only D2D link n. */
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
		/* D2D FW length header, so Mimir knows how much of the staged image is real */
		uint32_t d2d_fw_len_le = sys_cpu_to_le32((uint32_t)d2d_entry->length);

		LOG_INF("OCCP load - D2D FW for remote proc [%zu]", i);
		ret = tt_smc_remoteproc_load(cfg->remoteprocs[i], TT_D2D_OCCP_STAGE_ADDR,
					     (uint8_t *)&d2d_fw_len_le, sizeof(d2d_fw_len_le));
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-load D2D FW length header on remoteproc[%zu]: %d",
				i, ret);
			return ret;
		}

		ret = tt_smc_remoteproc_load(cfg->remoteprocs[i],
					     TT_D2D_OCCP_STAGE_ADDR + TT_D2D_FW_HEADER_SIZE,
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

	for (size_t i = 0; i < cfg->num_local_links; i++) {
		/* The remote target only appears once its die has booted far enough to run. */
		ret = tt_grendel_i3c_wait_ready(cfg->local_links[i].remote_i3c,
						K_MSEC(TT_MIMIR_REMOTE_READY_TIMEOUT_MS));
		if (ret != 0) {
			return ret;
		}
	}

	for (size_t i = 0; i < cfg->num_remote_links; i++) {
		ret = tt_grendel_i3c_wait_ready(cfg->remote_links[i].remote_a_i3c,
						K_MSEC(TT_MIMIR_REMOTE_READY_TIMEOUT_MS));
		if (ret != 0) {
			return ret;
		}
		ret = tt_grendel_i3c_wait_ready(cfg->remote_links[i].remote_b_i3c,
						K_MSEC(TT_MIMIR_REMOTE_READY_TIMEOUT_MS));
		if (ret != 0) {
			return ret;
		}
	}

	/*
	 * Both ends of each link have to be loaded before either is started,
	 * and then started close together, or the link does not train.
	 */
	for (size_t i = 0; i < cfg->num_local_links; i++) {
		const struct tt_mimir_local_d2d_link *link = &cfg->local_links[i];

		if (!device_is_ready(link->local_d2d)) {
			LOG_ERR("D2D[%zu] is not ready", i);
			return -ENODEV;
		}

		ret = tt_d2d_reset_release(link->local_d2d);
		if (ret != 0) {
			LOG_ERR("Failed to release D2D[%zu] reset: %d", i, ret);
			return ret;
		}

		ret = tt_d2d_load_fw(link->local_d2d,
				     (const uint8_t *)(TT_BUN2_STAGING_AREA_ADDR +
						       manifest->payload_offset +
						       d2d_entry->offset),
				     (size_t)d2d_entry->length);
		if (ret != 0) {
			LOG_ERR("Failed to load D2D[%zu] firmware: %d", i, ret);
			return ret;
		}

		ret = tt_grendel_i3c_d2d_init(link->remote_i3c, link->remote_instance,
						      K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("Remote D2D[%zu] init failed: %d", i, ret);
			return ret;
		}
	}

	/* Links with no local tile: both ends are commanded over I3C. */
	for (size_t i = 0; i < cfg->num_remote_links; i++) {
		ret = tt_grendel_i3c_d2d_init(cfg->remote_links[i].remote_a_i3c,
					      cfg->remote_links[i].remote_a_instance,
					      K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("Remote D2D link[%zu] end A init failed: %d", i, ret);
			return ret;
		}

		ret = tt_grendel_i3c_d2d_init(cfg->remote_links[i].remote_b_i3c,
					      cfg->remote_links[i].remote_b_instance,
					      K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("Remote D2D link[%zu] end B init failed: %d", i, ret);
			return ret;
		}
	}

	if (cfg->num_local_links == 0) {
		LOG_WRN("No Grendel I3C controller; starting the local side only");
	}

	for (size_t i = 0; i < cfg->num_local_links; i++) {
		const struct tt_mimir_local_d2d_link *link = &cfg->local_links[i];

		ret = tt_grendel_i3c_d2d_start(link->remote_i3c, link->remote_instance,
						       K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("Remote D2D[%zu] start failed: %d", i, ret);
			return ret;
		}

		ret = tt_d2d_start(link->local_d2d);
		if (ret != 0) {
			LOG_ERR("Failed to start D2D[%zu]: %d", i, ret);
			return ret;
		}

		ret = tt_d2d_wait_link(link->local_d2d, K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("D2D[%zu] failed to train: %d", i, ret);
			return ret;
		}
	}

	for (size_t i = 0; i < cfg->num_remote_links; i++) {
		ret = tt_grendel_i3c_d2d_start(cfg->remote_links[i].remote_a_i3c,
					       cfg->remote_links[i].remote_a_instance,
					       K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("Remote D2D link[%zu] end A start failed: %d", i, ret);
			return ret;
		}

		ret = tt_grendel_i3c_d2d_start(cfg->remote_links[i].remote_b_i3c,
					       cfg->remote_links[i].remote_b_instance,
					       K_MSEC(TT_MIMIR_REMOTE_CMD_TIMEOUT_MS));
		if (ret != 0) {
			LOG_ERR("Remote D2D link[%zu] end B start failed: %d", i, ret);
			return ret;
		}
	}

	/* Tell each Mimir to proceed with its boot */
	uint64_t payload = 0;
	struct mbox_msg msg = {
		.data = &payload,
		.size = sizeof(payload),
	};

	for (size_t i = 0; i < cfg->num_mis_mboxes; i++) {
		ret = mbox_send(cfg->mis_mboxes[i], TT_MIMIR_MIS_MBOX_CHANNEL, &msg);
		if (ret != 0) {
			LOG_ERR("Failed to signal Mimir %zu to proceed: %d", i, ret);
			return ret;
		}
	}

	return 0;
}

int tt_mimir_remote_boot_launch_mis(const struct device *dev,
				    const struct fw_bundle_toc_entry *entry, const void *image)
{
	const struct tt_mimir_remote_boot_config *cfg = dev->config;
	uint64_t payload = 0;
	struct mbox_msg msg = {
		.data = &payload,
		.size = sizeof(payload),
	};
	int ret = 0;

	if (cfg->num_mis_mboxes == 0) {
		return -ENODEV;
	}

	if (cfg->num_mis_spa_bases != 2 * cfg->num_mis_mboxes) {
		LOG_ERR("mis-spa-bases must have one {hi, lo} pair per mis-mbox");
		return -EINVAL;
	}

	for (size_t i = 0; i < cfg->num_mis_mboxes; i++) {
		uint64_t spa_base = ((uint64_t)cfg->mis_spa_bases[2 * i] << 32) |
				    cfg->mis_spa_bases[2 * i + 1];
		uint64_t dst = spa_base + (entry->load_addr - TT_MIMIR_SMC_LOCAL_BASE);

		/* TODO - Use DM */
		memcpy((void *)(uintptr_t)dst, image, entry->length);

		int err = mbox_send(cfg->mis_mboxes[i], TT_MIMIR_MIS_MBOX_CHANNEL, &msg);

		if (err != 0) {
			LOG_ERR("Failed to launch MIS on Mimir %zu: %d", i, err);
			ret = err;
		}
	}

	return ret;
}

/* TODO - New SYS_INIT step to copy GDDR params over to mimir once D2D is trained */

#define TT_MIMIR_REMOTE_BOOT_REMOTEPROC_GET(node_id, prop, idx)                                    \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define TT_MIMIR_REMOTE_BOOT_MIS_MBOX_GET(node_id, prop, idx)                                      \
	DEVICE_DT_GET(DT_PHANDLE_BY_IDX(node_id, prop, idx)),

#define TT_MIMIR_REMOTE_BOOT_MIS_MBOX_CONFIG(inst)                                                 \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, mis_mbox),                                         \
		(static const struct device *const tt_mimir_remote_boot_mis_mboxes_##inst[] = {    \
			DT_INST_FOREACH_PROP_ELEM(inst, mis_mbox,                                  \
						  TT_MIMIR_REMOTE_BOOT_MIS_MBOX_GET)               \
		};),                                                                               \
		())

#define TT_MIMIR_REMOTE_BOOT_MIS_SPA_BASES_CONFIG(inst)                                            \
	COND_CODE_1(DT_INST_NODE_HAS_PROP(inst, mis_spa_bases),                                    \
		(static const uint32_t tt_mimir_remote_boot_mis_spa_bases_##inst[] =               \
			 DT_INST_PROP(inst, mis_spa_bases);),                                      \
		())

#define TT_MIMIR_LOCAL_D2D_LINK_GET(node_id) {                                                   \
	.local_d2d = DEVICE_DT_GET(DT_PHANDLE(node_id, local_d2d)),                               \
	.remote_i3c = DEVICE_DT_GET(DT_PHANDLE(node_id, remote_i3c)),                             \
	.remote_instance = DT_PROP(node_id, remote_instance),                                     \
},

#define TT_MIMIR_REMOTE_D2D_LINK_GET(node_id) {                                                  \
	.remote_a_i3c = DEVICE_DT_GET(DT_PHANDLE(node_id, remote_a_i3c)),                         \
	.remote_a_instance = DT_PROP(node_id, remote_a_instance),                                  \
	.remote_b_i3c = DEVICE_DT_GET(DT_PHANDLE(node_id, remote_b_i3c)),                         \
	.remote_b_instance = DT_PROP(node_id, remote_b_instance),                                 \
},

#define TT_MIMIR_REMOTE_BOOT_DEFINE(inst)                                                          \
	static const struct device *const tt_mimir_remote_boot_remoteprocs_##inst[] = {            \
		DT_INST_FOREACH_PROP_ELEM(inst, smc_remoteproc,                                    \
					  TT_MIMIR_REMOTE_BOOT_REMOTEPROC_GET)};                   \
	static const struct tt_mimir_local_d2d_link tt_mimir_local_links_##inst[] = {              \
		DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(local_d2d_links),                       \
					     TT_MIMIR_LOCAL_D2D_LINK_GET) };                     \
	static const struct tt_mimir_remote_d2d_link tt_mimir_remote_links_##inst[] = {             \
		DT_FOREACH_CHILD_STATUS_OKAY(DT_NODELABEL(remote_d2d_links),                     \
					     TT_MIMIR_REMOTE_D2D_LINK_GET) };                    \
	TT_MIMIR_REMOTE_BOOT_MIS_MBOX_CONFIG(inst)                                                 \
	TT_MIMIR_REMOTE_BOOT_MIS_SPA_BASES_CONFIG(inst)                                            \
	static const struct tt_mimir_remote_boot_config tt_mimir_remote_boot_config_##inst = {     \
		.remoteprocs = tt_mimir_remote_boot_remoteprocs_##inst,                            \
		.num_remoteprocs = ARRAY_SIZE(tt_mimir_remote_boot_remoteprocs_##inst),            \
		.local_links = tt_mimir_local_links_##inst,                                            \
		.num_local_links = ARRAY_SIZE(tt_mimir_local_links_##inst),                            \
		.remote_links = tt_mimir_remote_links_##inst,                                          \
		.num_remote_links = ARRAY_SIZE(tt_mimir_remote_links_##inst),                          \
			  .mis_mboxes = COND_CODE_1(                                       \
				DT_INST_NODE_HAS_PROP(inst, mis_mbox),                     \
				(tt_mimir_remote_boot_mis_mboxes_##inst),                  \
				(NULL)),                                                   \
			  .num_mis_mboxes = COND_CODE_1(                                   \
				DT_INST_NODE_HAS_PROP(inst, mis_mbox),                     \
				(ARRAY_SIZE(tt_mimir_remote_boot_mis_mboxes_##inst)),      \
				(0)),                                                      \
			  .mis_spa_bases = COND_CODE_1(                                    \
				DT_INST_NODE_HAS_PROP(inst, mis_spa_bases),                \
				(tt_mimir_remote_boot_mis_spa_bases_##inst),               \
				(NULL)),                                                   \
			  .num_mis_spa_bases = COND_CODE_1(                                \
				DT_INST_NODE_HAS_PROP(inst, mis_spa_bases),                \
				(ARRAY_SIZE(tt_mimir_remote_boot_mis_spa_bases_##inst)),   \
				(0)),                                                      \
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
