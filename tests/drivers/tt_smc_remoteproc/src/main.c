/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i3c.h>
#include <zephyr/ztest.h>

#include <zephyr/drivers/misc/tt_smc_remoteproc.h>

#include <soc.h>

#define SMC_SRAM_OCCP_BASE 0xc0066400 /* Start of SMC RAM scratch region */

const struct device *smc_dev = DEVICE_DT_GET(DT_ALIAS(smc_remoteproc));

uint8_t remote_smc_bin[] = {
#include CONFIG_REMOTE_SMC_BINARY_HEADER
};
const unsigned int remote_smc_bin_len = sizeof(remote_smc_bin);

/**
 * @brief Load and boot the secondary SMC image on the remote chiplet.
 * @details
 * Loads the embedded secondary BL1 image into the remote SMC's SRAM scratch region
 * through the remoteproc driver, then boots it. Both steps must succeed. The overall
 * result comes from the secondary image: once it reaches main() it prints.
 * The line it prints much match the console harness to pass the test.
 * @ingroup tests_remoteproc
 */
ZTEST(tt_smc_remoteproc, test_boot)
{
	int ret;

	TC_PRINT("Primary BL1 is running!\n");

	zassert_true(device_is_ready(smc_dev), "Remote SMC device not ready");

	ret = tt_smc_remoteproc_load(smc_dev, SMC_SRAM_OCCP_BASE, remote_smc_bin,
				     remote_smc_bin_len);
	zassert_equal(ret, 0, "Failed to load remote SMC image: %d", ret);

	ret = tt_smc_remoteproc_boot(smc_dev, SMC_SRAM_OCCP_BASE);
	zassert_equal(ret, 0, "Failed to boot remote SMC: %d", ret);

	/*
	 * We don't return from the testsuite here. The remote SMC
	 * will also boot a ZTEST, and that test passing is what actually
	 * indicates success.
	 */
	while (1) {
		k_sleep(K_SECONDS(1));
		TC_PRINT("Primary BL1 is alive!\n");
	}
}

ZTEST_SUITE(tt_smc_remoteproc, NULL, NULL, NULL, NULL, NULL);
