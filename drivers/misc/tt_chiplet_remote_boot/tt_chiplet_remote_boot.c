/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define DT_DRV_COMPAT tenstorrent_chiplet_remote_boot

#include <errno.h>
#include <inttypes.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_d2d.h>
#include <zephyr/drivers/misc/tt_chiplet_remote_boot.h>
#include <zephyr/drivers/misc/tt_smc_remoteproc.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(tt_chiplet_remote_boot, CONFIG_TT_CHIPLET_REMOTE_BOOT_LOG_LEVEL);

BUILD_ASSERT(CONFIG_TT_CHIPLET_REMOTE_BOOT_INIT_PRIO > CONFIG_TT_SMC_REMOTEPROC_INIT_PRIO,
	     "TT_CHIPLET_REMOTE_BOOT_INIT_PRIO must be higher than TT_SMC_REMOTEPROC_INIT_PRIO");

/*
 * A single command on the remote die, once its target is answering. D2D_INIT
 * copies the whole D2D image, which takes seconds under emulation, so this is
 * sized for that rather than for silicon.
 */
#define TT_MIMIR_REMOTE_CMD_TIMEOUT_MS 30000

/* Writing the mis-mbox outbound channel 0 prompts a chiplet to jump to its MIS image */
#define TT_CHIPLET_MIS_MBOX_CHANNEL 0

/* Base of the core-local address window that image load addresses are relative to */
#define TT_MIMIR_SMC_LOCAL_BASE 0xC0000000ULL

/* Order must match the chiplet-type enum in the binding */
enum tt_chiplet_type {
	TT_CHIPLET_TYPE_MIMIR,
	TT_CHIPLET_TYPE_QUASAR,
};

static const char *const tt_chiplet_type_names[] = {
	[TT_CHIPLET_TYPE_MIMIR] = "mimir",
	[TT_CHIPLET_TYPE_QUASAR] = "quasar",
};

/* One remote chiplet, from its child node */
struct tt_chiplet_remote {
	const struct device *remoteproc;
	const struct device *mis_mbox;
	/* SPA base of the chiplet's core-local window */
	uint64_t mis_spa_base;
	/* BUN2 entry holding this chiplet's BL1 */
	uint32_t bl1_toc_index;
	/* BUN3 entry holding this chiplet's MIS */
	uint32_t mis_toc_index;
	enum tt_chiplet_type chiplet_type;
};

struct tt_chiplet_remote_boot_config {
	const struct tt_chiplet_remote *remotes;
	size_t num_remotes;
	/* Shared by all remotes */
	uint32_t d2d_fw_toc_index;
	uint32_t gddr_params_toc_index;
};

struct tt_chiplet_remote_boot_data {
};

static int tt_chiplet_remote_boot_init(const struct device *dev)
{
	const struct tt_chiplet_remote_boot_config *cfg = dev->config;

	int ret;

	/* A) Locate the chiplet images in the BUN2 staging area */
	const struct fw_bundle_manifest *manifest =
		(const struct fw_bundle_manifest *)TT_BUN2_STAGING_AREA_ADDR;
	const struct fw_bundle_toc *toc = (const struct fw_bundle_toc *)(TT_BUN2_STAGING_AREA_ADDR +
									 manifest->payload_offset);
	const uintptr_t payload_base = TT_BUN2_STAGING_AREA_ADDR + manifest->payload_offset;

	if (cfg->d2d_fw_toc_index >= toc->image_count) {
		LOG_ERR("D2D FW TOC index %u out of range (bundle has %" PRIu64 " entries)",
			cfg->d2d_fw_toc_index, toc->image_count);
		return -ENOENT;
	}

	const struct fw_bundle_toc_entry *d2d_entry = &toc->entries[cfg->d2d_fw_toc_index];

	for (size_t i = 0; i < cfg->num_remotes; i++) {
		const struct tt_chiplet_remote *r = &cfg->remotes[i];

		if (r->bl1_toc_index >= toc->image_count) {
			LOG_ERR("remote[%zu] BL1 TOC index %u out of range (bundle has %" PRIu64
				" entries)",
				i, r->bl1_toc_index, toc->image_count);
			return -ENOENT;
		}
		if (!device_is_ready(r->remoteproc)) {
			LOG_ERR("remoteproc[%zu] is not ready", i);
			return -ENODEV;
		}
		LOG_INF("Remote[%zu] %s: BL1 from TOC[%u]", i,
			tt_chiplet_type_names[r->chiplet_type], r->bl1_toc_index);
	}

	/* B) OCCP-load each remote's BL1 and the shared D2D FW, then start them */
	for (size_t i = 0; i < cfg->num_remotes; i++) {
		const struct tt_chiplet_remote *r = &cfg->remotes[i];
		const struct fw_bundle_toc_entry *bl1 = &toc->entries[r->bl1_toc_index];

		/* D2D FW length header, so the chiplet knows how much of the staged image is real */
		uint32_t d2d_fw_len_le = sys_cpu_to_le32((uint32_t)d2d_entry->length);

		LOG_INF("OCCP load - D2D FW for remote [%zu]", i);
		ret = tt_smc_remoteproc_load(r->remoteproc, TT_D2D_OCCP_STAGE_ADDR,
					     (uint8_t *)&d2d_fw_len_le, sizeof(d2d_fw_len_le));
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-load D2D FW length header on remoteproc[%zu]: %d",
				i, ret);
			return ret;
		}

		ret = tt_smc_remoteproc_load(r->remoteproc,
					     TT_D2D_OCCP_STAGE_ADDR + TT_D2D_FW_HEADER_SIZE,
					     (uint8_t *)(payload_base + d2d_entry->offset),
					     (size_t)d2d_entry->length);
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-load D2D FW on remoteproc[%zu]: %d", i, ret);
			return ret;
		}

		LOG_INF("OCCP load - BL1 for remote [%zu]", i);
		ret = tt_smc_remoteproc_load(r->remoteproc, bl1->load_addr,
					     (uint8_t *)(payload_base + bl1->offset),
					     (size_t)bl1->length);
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-load BL1 on remoteproc[%zu]: %d", i, ret);
			return ret;
		}
	}

	for (size_t i = 0; i < cfg->num_remotes; i++) {
		const struct tt_chiplet_remote *r = &cfg->remotes[i];

		/* Remote execute its BL1 */
		ret = tt_smc_remoteproc_boot(r->remoteproc, toc->entries[r->bl1_toc_index].load_addr);
		if (ret != 0) {
			LOG_ERR("Failed to OCCP-boot BL1 on remoteproc[%zu]: %d", i, ret);
			return ret;
		}
	}

	/* Tell each chiplet to proceed with its boot. Each chiplet brings its own D2D out of reset. */
	uint64_t payload = 0;
	struct mbox_msg msg = {
		.data = &payload,
		.size = sizeof(payload),
	};

	for (size_t i = 0; i < cfg->num_remotes; i++) {
		ret = mbox_send(cfg->remotes[i].mis_mbox, TT_CHIPLET_MIS_MBOX_CHANNEL, &msg);
		if (ret != 0) {
			LOG_ERR("Failed to signal remote %zu to proceed: %d", i, ret);
			return ret;
		}
	}

	return 0;
}

int tt_chiplet_remote_boot_launch_mis(const struct device *dev)
{
	const struct tt_chiplet_remote_boot_config *cfg = dev->config;

	/* The MIS bundle is staged in the BUN3 area, which shares the BUN2 address */
	const struct fw_bundle_manifest *manifest =
		(const struct fw_bundle_manifest *)TT_BUN3_STAGING_AREA_ADDR;
	const struct fw_bundle_toc *toc = (const struct fw_bundle_toc *)(TT_BUN3_STAGING_AREA_ADDR +
									 manifest->payload_offset);
	const uintptr_t payload_base = TT_BUN3_STAGING_AREA_ADDR + manifest->payload_offset;

	uint64_t payload = 0;
	struct mbox_msg msg = {
		.data = &payload,
		.size = sizeof(payload),
	};
	int ret = 0;

	for (size_t i = 0; i < cfg->num_remotes; i++) {
		const struct tt_chiplet_remote *r = &cfg->remotes[i];

		if (r->mis_toc_index >= toc->image_count) {
			LOG_ERR("remote[%zu] MIS TOC index %u out of range (bundle has %" PRIu64
				" entries)",
				i, r->mis_toc_index, toc->image_count);
			ret = -ENOENT;
			continue;
		}

		const struct fw_bundle_toc_entry *mis = &toc->entries[r->mis_toc_index];

		if (mis->type != FW_BUNDLE_IMG_TYPE_SMC_MIS) {
			LOG_ERR("remote[%zu] TOC[%u] is not a MIS image", i, r->mis_toc_index);
			ret = -EINVAL;
			continue;
		}

		uint64_t dst = r->mis_spa_base + (mis->load_addr - TT_MIMIR_SMC_LOCAL_BASE);

		/* TODO - Use DM */
		memcpy((void *)(uintptr_t)dst, (const void *)(payload_base + mis->offset),
		       mis->length);

		int err = mbox_send(r->mis_mbox, TT_CHIPLET_MIS_MBOX_CHANNEL, &msg);

		if (err != 0) {
			LOG_ERR("Failed to launch MIS on remote[%zu]: %d", i, err);
			ret = err;
		}
	}

	return ret;
}

/* TODO - New SYS_INIT step to copy GDDR params over to the chiplets once D2D is trained */

/*
 * A child's own TOC index wins. Otherwise the parent's default for the child's chiplet type
 * applies: m-*-toc-idx for "mimir", q-*-toc-idx for "quasar". TT_CHIPLET_REMOTE_ASSERT rejects
 * a child that has neither.
 */
#define TT_PASTE_(a, b) a##b
#define TT_PASTE(a, b) TT_PASTE_(a, b)

#define TT_CHIPLET_DEFAULT_BL1_MIMIR(node_id) DT_PROP_OR(DT_PARENT(node_id), m_bl1_toc_idx, 0)
#define TT_CHIPLET_DEFAULT_BL1_QUASAR(node_id) DT_PROP_OR(DT_PARENT(node_id), q_bl1_toc_idx, 0)
#define TT_CHIPLET_DEFAULT_MIS_MIMIR(node_id) DT_PROP_OR(DT_PARENT(node_id), m_mis_toc_idx, 0)
#define TT_CHIPLET_DEFAULT_MIS_QUASAR(node_id) DT_PROP_OR(DT_PARENT(node_id), q_mis_toc_idx, 0)

#define TT_CHIPLET_HAS_DEFAULT_BL1_MIMIR(node_id)                                                  \
	DT_NODE_HAS_PROP(DT_PARENT(node_id), m_bl1_toc_idx)
#define TT_CHIPLET_HAS_DEFAULT_BL1_QUASAR(node_id)                                                 \
	DT_NODE_HAS_PROP(DT_PARENT(node_id), q_bl1_toc_idx)
#define TT_CHIPLET_HAS_DEFAULT_MIS_MIMIR(node_id)                                                  \
	DT_NODE_HAS_PROP(DT_PARENT(node_id), m_mis_toc_idx)
#define TT_CHIPLET_HAS_DEFAULT_MIS_QUASAR(node_id)                                                 \
	DT_NODE_HAS_PROP(DT_PARENT(node_id), q_mis_toc_idx)

#define TT_CHIPLET_BL1_TOC_INDEX(node_id)                                                          \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, bl1_toc_index),                                      \
		    (DT_PROP(node_id, bl1_toc_index)),                                             \
		    (TT_PASTE(TT_CHIPLET_DEFAULT_BL1_,                                             \
			      DT_STRING_UPPER_TOKEN(node_id, chiplet_type))(node_id)))

#define TT_CHIPLET_MIS_TOC_INDEX(node_id)                                                          \
	COND_CODE_1(DT_NODE_HAS_PROP(node_id, mis_toc_index),                                      \
		    (DT_PROP(node_id, mis_toc_index)),                                             \
		    (TT_PASTE(TT_CHIPLET_DEFAULT_MIS_,                                             \
			      DT_STRING_UPPER_TOKEN(node_id, chiplet_type))(node_id)))

#define TT_CHIPLET_REMOTE_ASSERT(node_id)                                                          \
	BUILD_ASSERT(DT_NODE_HAS_PROP(node_id, bl1_toc_index) ||                                   \
			     TT_PASTE(TT_CHIPLET_HAS_DEFAULT_BL1_,                                 \
				      DT_STRING_UPPER_TOKEN(node_id, chiplet_type))(node_id),     \
		     "bl1-toc-index: set it on the node, or the parent's default for its type");   \
	BUILD_ASSERT(DT_NODE_HAS_PROP(node_id, mis_toc_index) ||                                   \
			     TT_PASTE(TT_CHIPLET_HAS_DEFAULT_MIS_,                                 \
				      DT_STRING_UPPER_TOKEN(node_id, chiplet_type))(node_id),     \
		     "mis-toc-index: set it on the node, or the parent's default for its type");   \
	

#define TT_CHIPLET_REMOTE_GET(node_id)                                                             \
	{                                                                                          \
		.remoteproc = DEVICE_DT_GET(DT_PHANDLE(node_id, smc_remoteproc)),                  \
		.mis_mbox = DEVICE_DT_GET(DT_PHANDLE(node_id, mis_mbox)),                          \
		.mis_spa_base = ((uint64_t)DT_PROP_BY_IDX(node_id, mis_spa_base, 0) << 32) |       \
				DT_PROP_BY_IDX(node_id, mis_spa_base, 1),                          \
		.bl1_toc_index = TT_CHIPLET_BL1_TOC_INDEX(node_id),                                \
		.mis_toc_index = TT_CHIPLET_MIS_TOC_INDEX(node_id),                                \
		.chiplet_type = DT_ENUM_IDX(node_id, chiplet_type),                                \
	},

#define TT_CHIPLET_REMOTE_BOOT_DEFINE(inst)                                                        \
	DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), TT_CHIPLET_REMOTE_ASSERT)                  \
	static const struct tt_chiplet_remote tt_chiplet_remote_remotes_##inst[] = {               \
		DT_FOREACH_CHILD_STATUS_OKAY(DT_DRV_INST(inst), TT_CHIPLET_REMOTE_GET)};           \
	static const struct tt_chiplet_remote_boot_config tt_chiplet_remote_boot_config_##inst = {  \
		.remotes = tt_chiplet_remote_remotes_##inst,                                       \
		.num_remotes = ARRAY_SIZE(tt_chiplet_remote_remotes_##inst),                       \
		.d2d_fw_toc_index = DT_INST_PROP(inst, d2d_fw_toc_index),                          \
		.gddr_params_toc_index = DT_INST_PROP(inst, gddr_params_toc_index),                \
	};                                                                                         \
	static struct tt_chiplet_remote_boot_data tt_chiplet_remote_boot_data_##inst;              \
	DEVICE_DT_INST_DEFINE(inst, tt_chiplet_remote_boot_init, NULL,                             \
			      &tt_chiplet_remote_boot_data_##inst,                                 \
			      &tt_chiplet_remote_boot_config_##inst, POST_KERNEL,                  \
			      CONFIG_TT_CHIPLET_REMOTE_BOOT_INIT_PRIO, NULL);

DT_INST_FOREACH_STATUS_OKAY(TT_CHIPLET_REMOTE_BOOT_DEFINE)
