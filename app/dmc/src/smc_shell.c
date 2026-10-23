/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Shell commands that run on the DMC and reach the SMC (ARC) over JTAG/AXI.
 * Useful for debugging when the SMC is not answering on I2C/SMBus.
 */

#include <stdlib.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/jtag.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include <tenstorrent/bh_chip.h>

#include "status_reg.h"

#if defined(CONFIG_SHELL) && BH_CHIP_COUNT > 0

#define SCRATCH_REG_COUNT     8
#define SCRATCH_RAM_REG_COUNT 64
#define RD_MAX_WORDS          256
#define ARC_PC_CORE_0_ADDR    0x80030C00

static const struct {
	uint8_t idx;
	const char *name;
} scratch_ram_names[] = {
	{0, "FW_VERSION"},
	{2, "BOOT_STATUS0"},
	{3, "BOOT_STATUS1"},
	{4, "ERROR_STATUS0"},
	{5, "ERROR_STATUS1"},
	{6, "INTERFACE_TABLE_BASE"},
	{8, "MSG_Q_STATUS"},
	{9, "MSG_Q_ERR_FLAGS"},
	{10, "SPI_BUFFER_INFO"},
	{12, "TELEMETRY_DATA"},
	{13, "TELEMETRY_TABLE"},
	{14, "PCIE_INIT_CPL_TIME"},
	{15, "CMFW_START_TIME"},
	{16, "ARC_START_TIME"},
	{17, "PERST_TO_DMFW_INIT_DONE"},
	{18, "PING_DMFW_DURATION"},
	{19, "I2C0_TARGET_DEBUG_STATE"},
};

static const char *scratch_ram_name(unsigned int idx)
{
	for (size_t i = 0; i < ARRAY_SIZE(scratch_ram_names); i++) {
		if (scratch_ram_names[i].idx == idx) {
			return scratch_ram_names[i].name;
		}
	}
	return "";
}

/* Reads `count` consecutive 32-bit words starting at `addr` in a single JTAG session. */
static int smc_read_words(const struct shell *sh, uint32_t addr, uint32_t *out, size_t count)
{
	const struct device *jtag = BH_CHIPS[BH_CHIP_PRIMARY_INDEX].config.jtag;
	int ret = jtag_setup(jtag);

	if (ret != 0) {
		shell_error(sh, "jtag_setup failed: %d", ret);
		return ret;
	}

	jtag_reset(jtag);
	for (size_t i = 0; i < count; i++) {
		ret = jtag_axi_read32(jtag, addr + 4 * i, &out[i]);
		if (ret != 0) {
			shell_error(sh, "AXI read of 0x%08zx failed: %d", (size_t)addr + 4 * i,
				    ret);
			break;
		}
	}
	jtag_teardown(jtag);

	return ret;
}

static int cmd_smc_scratch(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t regs[SCRATCH_REG_COUNT];
	uint32_t ram[SCRATCH_RAM_REG_COUNT];
	bool all = argc > 1 && strcmp(argv[1], "all") == 0;
	int ret;

	ret = smc_read_words(sh, RESET_UNIT_SCRATCH_BASE_ADDR, regs, ARRAY_SIZE(regs));
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "SCRATCH[0-7] @ 0x%08x", RESET_UNIT_SCRATCH_BASE_ADDR);
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		shell_print(sh, "  [%zu] 0x%08x%s", i, regs[i],
			    i == 0 ? "  (POST_CODE)" : i == 1 ? "  (CABLE_POWER_LIMIT)" : "");
	}

	ret = smc_read_words(sh, RESET_UNIT_SCRATCH_RAM_BASE_ADDR, ram, ARRAY_SIZE(ram));
	if (ret != 0) {
		return ret;
	}

	shell_print(sh, "SCRATCH_RAM[0-63] @ 0x%08x", RESET_UNIT_SCRATCH_RAM_BASE_ADDR);
	for (size_t i = 0; i < ARRAY_SIZE(ram); i++) {
		const char *name = scratch_ram_name(i);

		if (all || ram[i] != 0 || name[0] != '\0') {
			shell_print(sh, "  [%2zu] 0x%08x  %s", i, ram[i], name);
		}
	}

	uint32_t pc;

	if (smc_read_words(sh, ARC_PC_CORE_0_ADDR, &pc, 1) == 0) {
		shell_print(sh, "ARC PC core0 @ 0x%08x: 0x%08x", ARC_PC_CORE_0_ADDR, pc);
	}

	return 0;
}

static int cmd_smc_rd(const struct shell *sh, size_t argc, char **argv)
{
	static uint32_t buf[RD_MAX_WORDS];
	uint32_t addr = strtoul(argv[1], NULL, 0);
	size_t count = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;

	if (count == 0 || count > RD_MAX_WORDS || (addr & 3) != 0) {
		shell_error(sh, "addr must be 4-byte aligned, count 1..%d", RD_MAX_WORDS);
		return -EINVAL;
	}

	int ret = smc_read_words(sh, addr, buf, count);

	if (ret != 0) {
		return ret;
	}

	for (size_t i = 0; i < count; i++) {
		shell_print(sh, "0x%08zx: 0x%08x", (size_t)addr + 4 * i, buf[i]);
	}

	return 0;
}

static int cmd_smc_pgood(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (size_t i = 0; i < BH_CHIP_COUNT; i++) {
		const struct bh_chip *chip = &BH_CHIPS[i];

		shell_print(sh, "chip%zu: pgood=%d severe_fault=%d fall_pending=%d rise_pending=%d "
			    "last_trip_ms=%lld uptime_ms=%lld",
			    i, gpio_pin_get_dt(&chip->config.pgood),
			    chip->data.pgood_severe_fault, chip->data.pgood_fall_triggered,
			    chip->data.pgood_rise_triggered, chip->data.pgood_last_trip_ms,
			    k_uptime_get());
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_smc,
	SHELL_CMD(pgood, NULL, "Show DMC pgood state and severe-fault latch", cmd_smc_pgood),
	SHELL_CMD_ARG(scratch, NULL,
		      "Read SMC scratch registers over JTAG\n"
		      "usage: smc scratch [all]   (all also shows zero SCRATCH_RAM entries)",
		      cmd_smc_scratch, 1, 1),
	SHELL_CMD_ARG(rd, NULL,
		      "Read SMC AXI address over JTAG\n"
		      "usage: smc rd <addr> [count]",
		      cmd_smc_rd, 2, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(smc, &sub_smc, "SMC access over JTAG", NULL);

#endif /* CONFIG_SHELL && BH_CHIP_COUNT > 0 */
