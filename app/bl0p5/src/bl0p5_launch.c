/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include "bl0p5_launch.h"

LOG_MODULE_REGISTER(bl0p5_launch, CONFIG_TT_APP_LOG_LEVEL);

/* Host <-> SMC boot handshake states, written/polled via the host-boot-state scratch register */
#define HOST_BOOT_STATE_WAIT_FOR_BUNDLE (1U)
#define HOST_BOOT_STATE_BUNDLE_STAGED   (2U)
#define HOST_BOOT_STATE_BUNDLE_CONSUMED (3U)

/* Bundle validation handshake bits, in the bundle-validation scratch register */
#define BUNDLE_READY_FOR_VALIDATION_BIT BIT(0)
#define BUNDLE_VALIDATED_BIT            BIT(1)

/* SMC_CPU_SMC_CPU_CTRL_RESET_VECTOR_0 local address */
#define SMC_RESET_VECTOR0_ADDR 0xC0010000UL

int bl0p5_launch(const struct bl0p5_regs *regs)
{
	/* A) Tell the host we are ready to receive a bundle */
	sys_write32(HOST_BOOT_STATE_WAIT_FOR_BUNDLE, regs->host_boot_state);

	/* B) Wait for the host to stage the bundle in the staging area. Only the low nibble of the
	 * register carries the state, so the upper bits are ignored.
	 */
	LOG_INF("Waiting for BUN2 staging");
	for (uint32_t polls = 0;
	     (sys_read32(regs->host_boot_state) & 0xF) != HOST_BOOT_STATE_BUNDLE_STAGED;
	     polls++) {
		if (polls >= regs->max_polls) {
			LOG_ERR("Timed out waiting for BUN2 staging");
			return -ETIMEDOUT;
		}
		regs->pause();
	}

	/* Close host access to the staging area; only the SMC touches it from here on */

	/* D) Request validation of the staged bundle */
	sys_write32(BUNDLE_READY_FOR_VALIDATION_BIT, regs->bundle_validation);

	/* E) Wait for validation to complete. The ready bit alone does not count. */
	LOG_INF("Waiting for BUN2 validation");
	for (uint32_t polls = 0; !(sys_read32(regs->bundle_validation) & BUNDLE_VALIDATED_BIT);
	     polls++) {
		if (polls >= regs->max_polls) {
			LOG_ERR("Timed out waiting for BUN2 validation");
			return -ETIMEDOUT;
		}
		regs->pause();
	}

	const struct fw_bundle_manifest *manifest =
		(const struct fw_bundle_manifest *)regs->staging_area;
	const struct fw_bundle_toc *toc =
		(const struct fw_bundle_toc *)(regs->staging_area + manifest->payload_offset);

	/* Bundles always place the K-SMC BL1 image at TOC index 0 */
	const struct fw_bundle_toc_entry *bl1_entry =
		(toc->image_count > 0) ? &toc->entries[0] : NULL;

	if (bl1_entry == NULL || bl1_entry->type != FW_BUNDLE_IMG_TYPE_SMC_BL1) {
		LOG_ERR("No SMC BL1 image found at TOC index 0 in staged bundle");
		return -ENOENT;
	}

	sys_write32(HOST_BOOT_STATE_BUNDLE_CONSUMED, regs->host_boot_state);

	/* F) Copy SMC BL1 from the staged bundle to its execute location */
	memcpy((void *)bl1_entry->load_addr,
	       (const void *)(regs->staging_area + manifest->payload_offset + bl1_entry->offset),
	       bl1_entry->length);

	sys_write64(bl1_entry->entry_point, regs->reset_vector);

	/* G) Jump to SMC BL1. Does not return; BL1 owns the core from here on. */
	regs->jump((uintptr_t)bl1_entry->entry_point);

	return 0;
}
