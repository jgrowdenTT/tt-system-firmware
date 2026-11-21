/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/tt_smc_remoteproc.h>
#include <zephyr/fff.h>
#include <zephyr/ztest.h>

#include <tenstorrent/occp.h>

#define SMC_SRAM_OCCP_BASE 0xc0066400

/*
 * The driver's calls to occp_write_data() and occp_execute_image() are linked with --wrap, so they
 * reach these FFF fakes. The real OCCP functions are linked but never called, and this suite
 * cannot touch the remote SMC.
 */
DEFINE_FFF_GLOBALS;

DECLARE_FAKE_VALUE_FUNC(int, __wrap_occp_write_data, const struct occp_backend *, uint64_t,
			const uint8_t *, size_t);
DEFINE_FAKE_VALUE_FUNC(int, __wrap_occp_write_data, const struct occp_backend *, uint64_t,
		       const uint8_t *, size_t);
DECLARE_FAKE_VALUE_FUNC(int, __wrap_occp_execute_image, const struct occp_backend *, uint64_t,
			uint8_t);
DEFINE_FAKE_VALUE_FUNC(int, __wrap_occp_execute_image, const struct occp_backend *, uint64_t,
		       uint8_t);

static void before(void *f)
{
	ARG_UNUSED(f);
	RESET_FAKE(__wrap_occp_write_data);
	RESET_FAKE(__wrap_occp_execute_image);
}

ZTEST_SUITE(tt_smc_remoteproc_error, NULL, NULL, before, NULL, NULL);

/**
 * @brief A failed image write makes tt_smc_remoteproc_load() return the error.
 * @details Forces the wrapped occp_write_data() to return -EIO. Checks that the driver makes
 * exactly one write, to the requested address, with the requested length, and passes the error
 * through.
 * @ingroup tests_remoteproc
 */
ZTEST(tt_smc_remoteproc_error, test_load_write_failure)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(smc_remoteproc));
	static const uint8_t image[16] = {0};
	int ret;

	__wrap_occp_write_data_fake.return_val = -EIO;
	ret = tt_smc_remoteproc_load(dev, SMC_SRAM_OCCP_BASE, image, sizeof(image));

	zassert_equal(ret, -EIO, "load returned %d on a failed write", ret);
	zassert_equal(__wrap_occp_write_data_fake.call_count, 1, "write calls: %d",
		      __wrap_occp_write_data_fake.call_count);
	zassert_equal(__wrap_occp_write_data_fake.arg1_val, SMC_SRAM_OCCP_BASE);
	zassert_equal(__wrap_occp_write_data_fake.arg3_val, sizeof(image));
}

/**
 * @brief A failed execute makes tt_smc_remoteproc_boot() return the error.
 * @details Forces the wrapped occp_execute_image() to return -EIO. Checks that the driver
 * executes exactly once, at the requested address on CPU 0, and passes the error through.
 * @ingroup tests_remoteproc
 */
ZTEST(tt_smc_remoteproc_error, test_boot_execute_failure)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(smc_remoteproc));
	int ret;

	__wrap_occp_execute_image_fake.return_val = -EIO;
	ret = tt_smc_remoteproc_boot(dev, SMC_SRAM_OCCP_BASE);

	zassert_equal(ret, -EIO, "boot returned %d on a failed execute", ret);
	zassert_equal(__wrap_occp_execute_image_fake.call_count, 1, "execute calls: %d",
		      __wrap_occp_execute_image_fake.call_count);
	zassert_equal(__wrap_occp_execute_image_fake.arg1_val, SMC_SRAM_OCCP_BASE);
	zassert_equal(__wrap_occp_execute_image_fake.arg2_val, 0);
}
