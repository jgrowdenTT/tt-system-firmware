/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

#include <tenstorrent/gr_smc.h>

/* SMC_WRAP reset unit registers, as in lib/tenstorrent/gr_smc/grendel_utils.c. */
#define RESET_UNIT_CHIP_ID_REG  0xC0002008
#define RESET_UNIT_STRAPS_LO    0xC0002090
#define RESET_UNIT_STRAPS_HI    0xC0002094

/* Timing hooks from lib/tenstorrent/gr_smc/grendel_utils.c. They are not in gr_smc.h, so
 * declare them here with the same prototypes that file uses.
 */
uint64_t read_cycle(void);
void wait_loop_itr(uint32_t iterations);

/* Iteration counts for the timing checks. */
#define SHORT_SPIN 10U
#define LONG_SPIN  100000U

ZTEST_SUITE(gr_smc, NULL, NULL, NULL, NULL, NULL);

/* The cycle counter must move forward across a spin. */
ZTEST(gr_smc, test_read_cycle_advances)
{
	uint64_t before = read_cycle();

	wait_loop_itr(LONG_SPIN);

	uint64_t after = read_cycle();

	zassert_true(after > before, "cycle counter did not advance: %llu -> %llu",
		     (unsigned long long)before, (unsigned long long)after);
}

/* Spinning more iterations must take proportionally more cycles. The 10x margin
 * allows for interrupts and cache effects while still catching a loop that the
 * compiler removed or a counter that does not count.
 */
ZTEST(gr_smc, test_wait_loop_scales_with_iterations)
{
	uint64_t t0 = read_cycle();

	wait_loop_itr(SHORT_SPIN);

	uint64_t t1 = read_cycle();

	wait_loop_itr(LONG_SPIN);

	uint64_t t2 = read_cycle();

	uint64_t short_cycles = t1 - t0;
	uint64_t long_cycles = t2 - t1;

	zassert_true(long_cycles > 10U * short_cycles,
		     "long spin (%llu cycles) not clearly longer than short spin (%llu)",
		     (unsigned long long)long_cycles, (unsigned long long)short_cycles);
}

/* Zero iterations must return immediately and not wrap to 2^32 iterations. */
ZTEST(gr_smc, test_wait_loop_zero_returns_quickly)
{
	uint64_t t0 = read_cycle();

	wait_loop_itr(0U);

	uint64_t t1 = read_cycle();

	zassert_true(t1 - t0 < 1000U, "zero-iteration spin took %llu cycles",
		     (unsigned long long)(t1 - t0));
}

/* The type field must equal chip_id[1:0] of the raw RESET_UNIT_CHIP_ID register. */
ZTEST(gr_smc, test_chipid_type_matches_raw_register)
{
	uint32_t raw = sys_read32(RESET_UNIT_CHIP_ID_REG);
	struct gr_smc_chipid id = gr_smc_get_chipid();

	zassert_equal((uint32_t)id.type, raw & 0x3U,
		      "type %u does not match raw chip_id[1:0] %u", (unsigned int)id.type,
		      (unsigned int)(raw & 0x3U));
}

/* The instance must be rebuilt from the four CHIP_ID straps, at the positions in the
 * header: CHIP_ID_3 = STRAPS_LO[11], CHIP_ID_2 = STRAPS_LO[12], CHIP_ID_1 = STRAPS_HI[23],
 * CHIP_ID_0 = STRAPS_HI[25]. Instance bit 3 is CHIP_ID_3 and bit 0 is CHIP_ID_0.
 */
ZTEST(gr_smc, test_chipid_instance_matches_straps)
{
	uint32_t lo = sys_read32(RESET_UNIT_STRAPS_LO);
	uint32_t hi = sys_read32(RESET_UNIT_STRAPS_HI);
	struct gr_smc_chipid id = gr_smc_get_chipid();

	uint32_t expected = (((lo >> 11) & 1U) << 3) | (((lo >> 12) & 1U) << 2) |
			    (((hi >> 23) & 1U) << 1) | ((hi >> 25) & 1U);

	zassert_equal((uint32_t)id.instance, expected,
		      "instance %u does not match straps (expected %u)", (unsigned int)id.instance,
		      (unsigned int)expected);
	zassert_true(id.instance <= 15U, "instance %u exceeds the 4-bit strap field",
		     (unsigned int)id.instance);
}

/* Identity is read from hardware and should not change between calls. */
ZTEST(gr_smc, test_chipid_is_stable)
{
	struct gr_smc_chipid a = gr_smc_get_chipid();
	struct gr_smc_chipid b = gr_smc_get_chipid();

	zassert_equal((uint32_t)a.type, (uint32_t)b.type, "type changed between reads");
	zassert_equal((uint32_t)a.instance, (uint32_t)b.instance, "instance changed between reads");
}
