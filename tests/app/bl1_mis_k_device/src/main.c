/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * On-device tests for the BL1 K launcher. See tests_bl1_mis_k_device.dox for the design and the
 * test plan.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/drivers/misc/tt_chiplet_remote_boot.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

#include "mis_launch.h"

DEFINE_FFF_GLOBALS;

DECLARE_FAKE_VOID_FUNC(gr_smc_jump_to, uintptr_t);
DEFINE_FAKE_VOID_FUNC(gr_smc_jump_to, uintptr_t);
DECLARE_FAKE_VALUE_FUNC(int, __wrap_tt_chiplet_remote_boot_launch_mis, const struct device *);
DEFINE_FAKE_VALUE_FUNC(int, __wrap_tt_chiplet_remote_boot_launch_mis, const struct device *);

#define HOST_BOOT_STATE_ADDR   ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(host_boot_state)))
#define BUNDLE_VALIDATION_ADDR ((uintptr_t)DT_REG_ADDR(DT_NODELABEL(bundle_validation)))
#define RESET_VECTOR0_ADDR     0xC0010000UL

#define HOST_STAGED   2U
#define HOST_VALIDATED 2U
#define MIS_ENTRY     0xC0067000ULL

static uint64_t saved_reset_vector;

static K_SEM_DEFINE(host_go, 0, 1);
static K_SEM_DEFINE(host_done, 0, 1);

/* Plays the host for one launch: stage, then validate once the launcher asks. */
static void host_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_sem_take(&host_go, K_FOREVER);

		/* Let the launcher clear validation and announce WAIT first. */
		k_sleep(K_MSEC(5));
		sys_write32(HOST_STAGED, HOST_BOOT_STATE_ADDR);

		/* Validate once the launcher has written the READY bit. The delay makes the launcher
		 * poll the validation register at least once, so that wait loop is exercised.
		 */
		while (!(sys_read32(BUNDLE_VALIDATION_ADDR) & BIT(0))) {
			k_sleep(K_MSEC(1));
		}
		k_sleep(K_MSEC(5));
		sys_write32(HOST_VALIDATED, BUNDLE_VALIDATION_ADDR);

		k_sem_give(&host_done);
	}
}

/* Cooperative, so it preempts the test thread when its sleep ends, while the launcher spins. */
K_THREAD_DEFINE(host_tid, 2048, host_fn, NULL, NULL, NULL, K_PRIO_COOP(2), 0, 0);

/* Lay out a BUN3 bundle in the real staging area. TOC entry 0 has the given type. */
static void stage_bundle(uint64_t image_count, uint64_t type0)
{
	struct fw_bundle_manifest *manifest = (struct fw_bundle_manifest *)TT_BUN3_STAGING_AREA_ADDR;
	struct fw_bundle_toc *toc = NULL;

	manifest->payload_offset = sizeof(*manifest);
	toc = (struct fw_bundle_toc *)(TT_BUN3_STAGING_AREA_ADDR + manifest->payload_offset);
	toc->image_count = image_count;
	toc->entries[0].type = type0;
	toc->entries[0].entry_point = MIS_ENTRY;
}

/* Runs the launcher with the host thread playing the other side. */
static void run_launch(void)
{
	k_sem_give(&host_go);
	launch_mis();
	zassert_equal(k_sem_take(&host_done, K_MSEC(2000)), 0, "host thread did not finish");
}

static void mis_k_device_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(gr_smc_jump_to);
	RESET_FAKE(__wrap_tt_chiplet_remote_boot_launch_mis);
	FFF_RESET_HISTORY();


	saved_reset_vector = sys_read64(RESET_VECTOR0_ADDR);
	memset((void *)TT_BUN3_STAGING_AREA_ADDR, 0, sizeof(struct fw_bundle_manifest) + 256);
	sys_write32(0, HOST_BOOT_STATE_ADDR);
	sys_write32(0, BUNDLE_VALIDATION_ADDR);
}

static void mis_k_device_after(void *fixture)
{
	ARG_UNUSED(fixture);

	/* The launcher writes the reset vector before it jumps. Put back what was there. */
	sys_write64(saved_reset_vector, RESET_VECTOR0_ADDR);
}

ZTEST_SUITE(mis_launch_k_device, NULL, NULL, mis_k_device_before, mis_k_device_after, NULL);

/**
 * @brief An empty bundle returns from the launcher without a chiplet launch or a jump.
 *
 * The launcher checks the TOC after validation. With no entries it logs the error and returns,
 * so the reset vector is not written and the jump is never reached.
 *
 * @ingroup tests_bl1_mis_k_device
 */
ZTEST(mis_launch_k_device, test_empty_bundle_returns_without_jump)
{
	stage_bundle(0, FW_BUNDLE_IMG_TYPE_SMC_MIS);

	run_launch();

	zassert_equal(__wrap_tt_chiplet_remote_boot_launch_mis_fake.call_count, 0);
	zassert_equal(gr_smc_jump_to_fake.call_count, 0);
}

/**
 * @brief A bundle whose TOC index 0 is not an SMC MIS image returns without a jump.
 *
 * An SMC BL1 image at index 0 is the wrong type for the K launcher, so it returns early, as for
 * an empty bundle.
 *
 * @ingroup tests_bl1_mis_k_device
 */
ZTEST(mis_launch_k_device, test_wrong_type_returns_without_jump)
{
	stage_bundle(1, FW_BUNDLE_IMG_TYPE_SMC_BL1);

	run_launch();

	zassert_equal(__wrap_tt_chiplet_remote_boot_launch_mis_fake.call_count, 0);
	zassert_equal(gr_smc_jump_to_fake.call_count, 0);
}

/**
 * @brief A valid MIS bundle sets the reset vector and jumps to MIS, with no chiplet launch.
 *
 * The test overlay has no chiplet_boot node, so the launcher skips the chiplet launch. The reset
 * vector holds the MIS entry point while the jump is made, and the jump is called once with that
 * entry point. The test restores the reset vector afterwards.
 *
 * @ingroup tests_bl1_mis_k_device
 */
ZTEST(mis_launch_k_device, test_valid_mis_launches_chiplets_and_jumps)
{
	stage_bundle(1, FW_BUNDLE_IMG_TYPE_SMC_MIS);
	__wrap_tt_chiplet_remote_boot_launch_mis_fake.return_val = 0;

	run_launch();

	/* No chiplet_boot node in the test overlay, so the launcher skips the chiplet launch. */
	zassert_equal(__wrap_tt_chiplet_remote_boot_launch_mis_fake.call_count, 0);
	zassert_equal(gr_smc_jump_to_fake.call_count, 1);
	zassert_equal(gr_smc_jump_to_fake.arg0_val, (uintptr_t)MIS_ENTRY);
	zassert_equal(sys_read64(RESET_VECTOR0_ADDR), MIS_ENTRY, "reset vector not set");
}
