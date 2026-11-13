/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr port of the Sival SDK's common/src/utils.c: the timing hooks
 * (read_cycle, wait_loop_itr) the prebuilt driver archives (gddr, eth)
 * reference but do not define.
 */

#include <stdint.h>

#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>

#include <tenstorrent/gr_smc.h>

LOG_MODULE_REGISTER(gr_smc, CONFIG_LOG_DEFAULT_LEVEL);

/* SMC_WRAP reset unit registers; the same addresses on every Grendel chiplet SMC. */
#define GR_SMC_RESET_UNIT_CHIP_ID_REG_ADDR   0xC0002008
#define GR_SMC_RESET_UNIT_STRAPS_LO_REG_ADDR 0xC0002090 /* straps from GPIOs 0-31 */
#define GR_SMC_RESET_UNIT_STRAPS_HI_REG_ADDR 0xC0002094 /* straps from GPIOs 32-63 */
#define GR_SMC_RESET_UNIT_CHIP_ID_MASK       0x3

/*
 * CHIP_ID[3:0] strap positions, as in tt_smc_master_chiplet_config_pkg.sv and the SMC ROM:
 * CHIP_ID_3 = strap 11 and CHIP_ID_2 = strap 12 in STRAPS_LO; CHIP_ID_1 = strap 55 and
 * CHIP_ID_0 = strap 57, which are bits 23 and 25 of STRAPS_HI.
 */
#define GR_SMC_STRAP_CHIP_ID_3_LO_BIT 11
#define GR_SMC_STRAP_CHIP_ID_2_LO_BIT 12
#define GR_SMC_STRAP_CHIP_ID_1_HI_BIT 23
#define GR_SMC_STRAP_CHIP_ID_0_HI_BIT 25

uint64_t read_cycle(void);
void wait_loop_itr(uint32_t iterations);

/*
 * 64-bit cycle counter, via Zephyr's kernel cycle counter (portable across the
 * SMC; the SDK only uses this for timing/heartbeat logging).
 */
uint64_t read_cycle(void)
{
#ifdef CONFIG_TIMER_HAS_64BIT_CYCLE_COUNTER
	return k_cycle_get_64();
#else
	return (uint64_t)k_cycle_get_32();
#endif
}

/*
 * Spin for a fixed iteration count. Explicit volatile loop with a compiler
 * barrier so it is not optimized away; matches utils.h's "spin N iterations"
 * contract.
 */
void wait_loop_itr(uint32_t iterations)
{
	for (volatile uint32_t i = 0; i < iterations; i++) {
		__asm__ volatile("" ::: "memory");
	}
}

struct gr_smc_chipid gr_smc_get_chipid(void)
{
	uint32_t chip_id = sys_read32(GR_SMC_RESET_UNIT_CHIP_ID_REG_ADDR);
	uint32_t straps_lo = sys_read32(GR_SMC_RESET_UNIT_STRAPS_LO_REG_ADDR);
	uint32_t straps_hi = sys_read32(GR_SMC_RESET_UNIT_STRAPS_HI_REG_ADDR);
	uint8_t instance = 0;

	instance |= ((straps_lo >> GR_SMC_STRAP_CHIP_ID_3_LO_BIT) & 1) << 3;
	instance |= ((straps_lo >> GR_SMC_STRAP_CHIP_ID_2_LO_BIT) & 1) << 2;
	instance |= ((straps_hi >> GR_SMC_STRAP_CHIP_ID_1_HI_BIT) & 1) << 1;
	instance |= (straps_hi >> GR_SMC_STRAP_CHIP_ID_0_HI_BIT) & 1;

	return (struct gr_smc_chipid){
		.type = (enum gr_smc_chip_type)(chip_id & GR_SMC_RESET_UNIT_CHIP_ID_MASK),
		.instance = instance,
	};
}

FUNC_NORETURN void gr_smc_jump_to(uintptr_t entry_point)
{
	LOG_INF("Jumping to %p", (void *)entry_point);

	(void)irq_lock();

	__asm__ volatile(
		/* Order completed memory writes before the handoff. */
		"fence\n"
		/* Make the next image visible to instruction fetch. */
		"fence.i\n"
		/* Disable all machine interrupt sources. */
		"csrw mie, zero\n"
		/* Clear pending interrupt bits that are software-writable. */
		"csrw mip, zero\n"
		/* Order the cleanup before entering the next image. */
		"fence\n"
		/* Set the next entry address as the mret destination. */
		"csrw mepc, %0\n"
		/* Select M-mode with MIE and MPIE cleared. */
		"li   t0, 0x1800\n"
		/* Apply the clean machine-mode interrupt state. */
		"csrw mstatus, t0\n"
		/* Enter the next image and restore MIE from the cleared MPIE. */
		"mret\n"
		:
		: "r"(entry_point)
		: "t0", "memory");
	CODE_UNREACHABLE;
}
