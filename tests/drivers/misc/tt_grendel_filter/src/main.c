/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/misc/tt_grendel_filter.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <firewall.h>
#include <platform.h>

#include "regs.h"

DEFINE_FFF_GLOBALS;

/*
 * filter_arm() and filter_disable() are linked with --wrap. When forced_err is not GRENDEL_ERR_OK
 * the wrapper returns it without calling the real firewall code, so the driver's error mapping can
 * be exercised with codes the real firewall never returns.
 */
static grendel_err_t forced_err;

grendel_err_t __real_filter_arm(filter_t filter, uint32_t filter_index, uint32_t hsio_tile,
				uint64_t start_addr, uint64_t end_addr, filter_fields_t fields);
grendel_err_t __real_filter_disable(filter_t filter, uint32_t filter_index, uint32_t hsio_tile);

grendel_err_t __wrap_filter_arm(filter_t filter, uint32_t filter_index, uint32_t hsio_tile,
				uint64_t start_addr, uint64_t end_addr, filter_fields_t fields)
{
	if (forced_err != GRENDEL_ERR_OK) {
		return forced_err;
	}
	return __real_filter_arm(filter, filter_index, hsio_tile, start_addr, end_addr, fields);
}

grendel_err_t __wrap_filter_disable(filter_t filter, uint32_t filter_index, uint32_t hsio_tile)
{
	if (forced_err != GRENDEL_ERR_OK) {
		return forced_err;
	}
	return __real_filter_disable(filter, filter_index, hsio_tile);
}

DEFINE_FAKE_VOID_FUNC(write16_reg, uint64_t, uint16_t);
DEFINE_FAKE_VALUE_FUNC(uint16_t, read16_reg, uint64_t);
DEFINE_FAKE_VOID_FUNC(write32_reg, uint64_t, uint32_t);
DEFINE_FAKE_VALUE_FUNC(uint32_t, read32_reg, uint64_t);
DEFINE_FAKE_VOID_FUNC(write64_reg, uint64_t, uint64_t);
DEFINE_FAKE_VALUE_FUNC(uint64_t, read64_reg, uint64_t);

#define FILTER_STRIDE        FIREWALL_FILTER_SIZE
#define FILTER_CONFIG_OFFSET FIREWALL_FILTER_CONFIG_OFFSET
#define FILTER_START_OFFSET  FIREWALL_START_ADDR_OFFSET
#define FILTER_END_OFFSET    FIREWALL_END_ADDR_OFFSET
#define INBOUND_BASE         SMC_CPU_SMC_INBOUND_FILTER_CTRL_0_REG_MAP_BASE_ADDR

#define FILTER_CONFIG_READ_EN     BIT64(0)
#define FILTER_CONFIG_WRITE_EN    BIT64(1)
#define FILTER_CONFIG_ADDR_MODE   BIT64(4)
#define FILTER_CONFIG_ALLOW_NS    BIT64(8)
#define FILTER_CONFIG_ALLOW_BURST BIT64(24)

static uint64_t entry_reg_addr(uint32_t entry, size_t offset)
{
	return INBOUND_BASE + (entry * FILTER_STRIDE) + offset;
}

static void assert_write64(uint64_t addr, uint64_t value)
{
	for (unsigned int i = write64_reg_fake.call_count; i > 0; i--) {
		unsigned int index = i - 1;

		if (write64_reg_fake.arg0_history[index] == addr) {
			zassert_equal(write64_reg_fake.arg1_history[index], value);
			return;
		}
	}

	zassert_true(false, "Expected write to 0x%llx", addr);
}

static void before(void *f)
{
	ARG_UNUSED(f);
	forced_err = GRENDEL_ERR_OK;
	RESET_FAKE(write16_reg);
	RESET_FAKE(read16_reg);
	RESET_FAKE(write32_reg);
	RESET_FAKE(read32_reg);
	RESET_FAKE(write64_reg);
	RESET_FAKE(read64_reg);
}

ZTEST(tt_grendel_filter, test_enable_programs_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
		.nonsecure = 1,
		.burst = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x08018000, 0x08037fff, flags);

	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 3);
	assert_write64(entry_reg_addr(0, FILTER_START_OFFSET), 0x08018000);
	assert_write64(entry_reg_addr(0, FILTER_END_OFFSET), 0x08037fff);
	assert_write64(entry_reg_addr(0, FILTER_CONFIG_OFFSET),
		       FILTER_CONFIG_READ_EN | FILTER_CONFIG_WRITE_EN | FILTER_CONFIG_ADDR_MODE |
			       FILTER_CONFIG_ALLOW_NS | FILTER_CONFIG_ALLOW_BURST);
}

ZTEST(tt_grendel_filter, test_enable_read_only)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x1000, 0x1fff, flags);

	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 3);
	assert_write64(entry_reg_addr(0, FILTER_CONFIG_OFFSET),
		       FILTER_CONFIG_READ_EN | FILTER_CONFIG_ADDR_MODE);
}

ZTEST(tt_grendel_filter, test_enable_equal_range)
{
	struct tt_grendel_filter_flags flags = {
		.write = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x2000, 0x2000, flags);

	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 3);
	assert_write64(entry_reg_addr(0, FILTER_START_OFFSET), 0x2000);
	assert_write64(entry_reg_addr(0, FILTER_END_OFFSET), 0x2000);
}

ZTEST(tt_grendel_filter, test_enable_does_not_touch_other_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
	};
	int ret = tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x0, 0xff, flags);

	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 3);
	for (unsigned int i = 0; i < write64_reg_fake.call_count; i++) {
		zassert_true(write64_reg_fake.arg0_history[i] >= INBOUND_BASE &&
			     write64_reg_fake.arg0_history[i] < INBOUND_BASE + FILTER_STRIDE);
	}
}

ZTEST(tt_grendel_filter, test_disable_clears_entry)
{
	struct tt_grendel_filter_flags flags = {
		.read = 1,
		.write = 1,
		.burst = 1,
	};
	int ret;

	ret = tt_grendel_filter_enable(SMC_INBOUND, 1, 0, 0x10, 0x1f, flags);
	zassert_equal(ret, 0);

	ret = tt_grendel_filter_disable(SMC_INBOUND, 1, 0);
	zassert_equal(ret, 0);
	zassert_equal(write64_reg_fake.call_count, 6);
	assert_write64(entry_reg_addr(1, FILTER_CONFIG_OFFSET), 0);
	assert_write64(entry_reg_addr(1, FILTER_START_OFFSET), 0);
	assert_write64(entry_reg_addr(1, FILTER_END_OFFSET), 0);
}

ZTEST(tt_grendel_filter, test_enable_rejects_bad_args)
{
	struct tt_grendel_filter_flags rw = {
		.read = 1,
		.write = 1,
	};
	struct tt_grendel_filter_flags none = {0};

	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 16, 0, 0, 1, rw), -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0x20, 0x10, rw), -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, BIT64(56), BIT64(56), rw),
		      -EINVAL);
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0, 1, none), -EINVAL);
	zassert_equal(write64_reg_fake.call_count, 0);
}

ZTEST(tt_grendel_filter, test_disable_rejects_bad_entry)
{
	zassert_equal(tt_grendel_filter_disable(SMC_INBOUND, 16, 0), -EINVAL);
	zassert_equal(write64_reg_fake.call_count, 0);
}

/**
 * @brief Enabling an entry for a filter type this platform does not have fails with -EINVAL.
 * @details MIMIR_ITN_MEM_TILE0 is a valid bank index but is not built for the Keraunos SMC, so
 * the real firewall code returns GRENDEL_ERR_FIREWALL_INVALID_PARAM. The driver must map that to
 * -EINVAL and must not program any registers.
 * @ingroup tests_tt_grendel_filter
 */
ZTEST(tt_grendel_filter, test_enable_unsupported_filter_type)
{
	struct tt_grendel_filter_flags rw = {
		.read = 1,
		.write = 1,
	};

	zassert_equal(tt_grendel_filter_enable((uint32_t)MIMIR_ITN_MEM_TILE0, 0, 0, 0, 1, rw),
		      -EINVAL);
	zassert_equal(write64_reg_fake.call_count, 0);
}

/**
 * @brief Disabling an entry for a filter type this platform does not have fails with -EINVAL.
 * @details Same unsupported filter type as the enable test. The firewall returns
 * GRENDEL_ERR_FIREWALL_INVALID_PARAM, which the driver must map to -EINVAL without writing
 * any registers.
 * @ingroup tests_tt_grendel_filter
 */
ZTEST(tt_grendel_filter, test_disable_unsupported_filter_type)
{
	zassert_equal(tt_grendel_filter_disable((uint32_t)MIMIR_ITN_MEM_TILE0, 0, 0), -EINVAL);
	zassert_equal(write64_reg_fake.call_count, 0);
}

/**
 * @brief An unexpected firewall error on enable is reported as -EIO.
 * @details Forces filter_arm() to return GRENDEL_ERR_SMC_TIMEOUT, a code the driver does not map
 * explicitly. The driver must log it and return -EIO, without programming any registers.
 * @ingroup tests_tt_grendel_filter
 */
ZTEST(tt_grendel_filter, test_enable_unmapped_error_is_eio)
{
	struct tt_grendel_filter_flags rw = {
		.read = 1,
		.write = 1,
	};

	forced_err = GRENDEL_ERR_SMC_TIMEOUT;
	zassert_equal(tt_grendel_filter_enable(SMC_INBOUND, 0, 0, 0, 1, rw), -EIO);
	zassert_equal(write64_reg_fake.call_count, 0);
}

/**
 * @brief An unexpected firewall error on disable is reported as -EIO.
 * @details Forces filter_disable() to return GRENDEL_ERR_SMC_TIMEOUT, which the driver does not
 * map explicitly. The driver must return -EIO, without programming any registers.
 * @ingroup tests_tt_grendel_filter
 */
ZTEST(tt_grendel_filter, test_disable_unmapped_error_is_eio)
{
	forced_err = GRENDEL_ERR_SMC_TIMEOUT;
	zassert_equal(tt_grendel_filter_disable(SMC_INBOUND, 0, 0), -EIO);
	zassert_equal(write64_reg_fake.call_count, 0);
}

ZTEST_SUITE(tt_grendel_filter, NULL, NULL, before, NULL, NULL);
