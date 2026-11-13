/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_GR_SMC_H_
#define TENSTORRENT_GR_SMC_H_

#include <stdint.h>
#include <zephyr/toolchain.h>

FUNC_NORETURN void gr_smc_jump_to(uintptr_t entry_point);

/* Chiplet type, from SMC_WRAP RESET_UNIT_CHIP_ID chip_id[1:0]. Fixed in RTL per chiplet type. */
enum gr_smc_chip_type {
	GR_SMC_CHIP_KERAUNOS = 0,
	GR_SMC_CHIP_QUASAR = 1,
	GR_SMC_CHIP_ATHENA = 2,
	GR_SMC_CHIP_MIMIR = 3,
};

struct gr_smc_chipid {
	/* Which kind of chiplet this SMC is on. */
	enum gr_smc_chip_type type;
	/* Instance of that kind, from the 4-bit CHIP_ID strap: Mimir0 = 0, Mimir1 = 1, ... */
	uint8_t instance;
};

/**
 * @brief Identify the chiplet this SMC is running on.
 *
 * The type comes from RESET_UNIT_CHIP_ID, which carries only the 2-bit chiplet type, so every
 * Mimir reads 3. The instance comes from the CHIP_ID[3:0] straps latched at reset into
 * RESET_UNIT_STRAPS_LO/HI. Both are read from hardware; no host input is used.
 */
struct gr_smc_chipid gr_smc_get_chipid(void);

#endif
