/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mis_launch.h"
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_mimir_remote_boot.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <errno.h>

LOG_MODULE_REGISTER(mis_launch_k, CONFIG_LOG_DEFAULT_LEVEL);

#define HOST_BOOT_STATE_ADDR   ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(host_boot_state)))
#define BUNDLE_VALIDATION_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bundle_validation)))
#define SMC_RESET_VECTOR0_ADDR 0xC0010000UL

/* Host <-> SMC boot handshake states, written/polled via the host-boot-state scratch register */
#define HOST_BOOT_STATE_WAIT_FOR_BUNDLE (1U)
#define HOST_BOOT_STATE_BUNDLE_STAGED   (2U)

/* Bundle validation handshake bits, in the bundle-validation scratch register */
#define BUNDLE_READY_FOR_VALIDATION_BIT BIT(0)
#define BUNDLE_VALIDATED_BIT            BIT(1)

void launch_mis(void)
{
	/* Pre-emptively Clear bundle validation */
	sys_write32(0, BUNDLE_VALIDATION_ADDR);

	/* A) Tell the host we are ready to receive a bundle */
	sys_write32(HOST_BOOT_STATE_WAIT_FOR_BUNDLE, HOST_BOOT_STATE_ADDR);

	/* B) Wait for the host to stage the bundle in the staging area */
	LOG_INF("Waiting for BUN3 staging");
	while ((sys_read32(HOST_BOOT_STATE_ADDR) & 0xf) != HOST_BOOT_STATE_BUNDLE_STAGED) {
		k_busy_wait(100);
	}

	/* C) TODO: Close host access to the staging area*/

	/* D) Request validation of the staged bundle */
	sys_write32(BUNDLE_READY_FOR_VALIDATION_BIT, BUNDLE_VALIDATION_ADDR);

	/* E) Wait for validation to complete */
	LOG_INF("Waiting for BUN3 validation");
	while (!(sys_read32(BUNDLE_VALIDATION_ADDR) & BUNDLE_VALIDATED_BIT)) {
		k_busy_wait(100);
	}

	const struct fw_bundle_manifest *manifest =
		(const struct fw_bundle_manifest *)TT_BUN3_STAGING_AREA_ADDR;
	const struct fw_bundle_toc *toc = (const struct fw_bundle_toc *)(TT_BUN3_STAGING_AREA_ADDR +
									 manifest->payload_offset);

	/* Bundles place the M-SMC MIS image at TOC index 1 */
	const struct fw_bundle_toc_entry *m_mis_entry =
		(toc->image_count > 1) ? &toc->entries[1] : NULL;

	/* Bundles place the K-SMC MIS image at TOC index 0 */
	const struct fw_bundle_toc_entry *k_mis_entry =
		(toc->image_count > 0) ? &toc->entries[0] : NULL;

	if (m_mis_entry == NULL || m_mis_entry->type != FW_BUNDLE_IMG_TYPE_SMC_MIS) {
		LOG_ERR("No SMC MIS image found at TOC index 1");
		return;
	}

	if (k_mis_entry == NULL || k_mis_entry->type != FW_BUNDLE_IMG_TYPE_SMC_MIS) {
		LOG_ERR("No SMC MIS image found at TOC index 0");
		return;
	}

	/* Prod each Mimir to jump to its MIS image after copying it over */
	int ret = tt_mimir_remote_boot_launch_mis(
		DEVICE_DT_GET(DT_NODELABEL(mimir_boot)), m_mis_entry,
		(const void *)(TT_BUN3_STAGING_AREA_ADDR + manifest->payload_offset +
			       m_mis_entry->offset));

	if (ret != 0) {
		LOG_ERR("Failed to launch MIS on Mimir: %d", ret);
	}
	/* We don't have to copy the image. BUN3 is placed so that we can immediately jump to the
	 * right location
	 */
	__asm__ volatile("fence\nfence.i" ::: "memory");

	LOG_INF("Jumping to K-SMC-MIS at load_addr 0x%llx", k_mis_entry->load_addr);

	/* G) Jump to SMC MIS. Does not return; MIS owns the core from here on. */
	/* mret gives MIS a clean entry: M-mode, MIE=0, no BL1 mtvec leaking through */
	sys_write64(k_mis_entry->entry_point, SMC_RESET_VECTOR0_ADDR);
	__asm__ volatile("csrw mepc, %0\n"
			 "li   t0, 0x1800\n" /* MPP=M-mode, MIE=0, MPIE=0 */
			 "csrw mstatus, t0\n"
			 "mret\n"
			 :
			 : "r"((uintptr_t)k_mis_entry->entry_point)
			 : "t0");

	while (1) {
		k_msleep(100);
	}
}
