/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/kernel.h>
#include <tenstorrent/gr_smc.h>

#include "bl0p5_launch.h"

#define HOST_BOOT_STATE_ADDR   ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(host_boot_state)))
#define BUNDLE_VALIDATION_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bundle_validation)))

/* SMC_CPU_SMC_CPU_CTRL_RESET_VECTOR_0 local address */
#define SMC_RESET_VECTOR0_ADDR 0xC0010000UL

/* Each host wait polls every 100 us, so this allows about 30 seconds per wait. */
#define BUNDLE_WAIT_MAX_POLLS (300000U)

static void bl0p5_pause(void)
{
	k_busy_wait(100);
}

static const struct bl0p5_regs board_regs = {
	.host_boot_state = HOST_BOOT_STATE_ADDR,
	.bundle_validation = BUNDLE_VALIDATION_ADDR,
	.staging_area = TT_BUN2_STAGING_AREA_ADDR,
	.reset_vector = SMC_RESET_VECTOR0_ADDR,
	.max_polls = BUNDLE_WAIT_MAX_POLLS,
	.pause = bl0p5_pause,
	.jump = gr_smc_jump_to,
};

int main(void)
{
	bl0p5_launch(&board_regs);

	/* Unreachable unless BL1 returns, which means the handoff failed and the core's
	 * state (stack, vector table, PMP, ...) can no longer be trusted.
	 */
	k_panic();
	return 0;
}
