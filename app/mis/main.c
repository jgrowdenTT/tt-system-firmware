/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>

#include <tenstorrent/gr_smc.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#if IS_ENABLED(CONFIG_SHELL) && IS_ENABLED(CONFIG_SHELL_PROMPT_CHANGE) &&                          \
	IS_ENABLED(CONFIG_SHELL_BACKEND_SERIAL)
#include <zephyr/shell/shell.h>
#include <zephyr/shell/shell_uart.h>
#define TT_MIS_SHELL_PROMPT_ENABLED 1
#endif

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

static const char *const chip_type_names[] = {
	[GR_SMC_CHIP_KERAUNOS] = "keraunos",
	[GR_SMC_CHIP_QUASAR] = "quasar",
	[GR_SMC_CHIP_ATHENA] = "athena",
	[GR_SMC_CHIP_MIMIR] = "mimir",
};

static const char *chip_type_name(enum gr_smc_chip_type type)
{
	if ((size_t)type >= ARRAY_SIZE(chip_type_names)) {
		return "unknown";
	}
	return chip_type_names[type];
}

#ifdef TT_MIS_SHELL_PROMPT_ENABLED
/* Prompt becomes e.g. "mimir1:~$ ". Takes effect at the next prompt, not the current one. */
static void set_shell_prompt(const struct gr_smc_chipid *id)
{
	char prompt[CONFIG_SHELL_PROMPT_BUFF_SIZE];
	const struct shell *sh = shell_backend_uart_get_ptr();
	int len = snprintf(prompt, sizeof(prompt), "%s%u:~$ ", chip_type_name(id->type),
			   id->instance);

	if (sh == NULL || len < 0 || (size_t)len >= sizeof(prompt)) {
		LOG_WRN("Shell prompt not set");
		return;
	}

	int ret = shell_prompt_change(sh, prompt);

	if (ret != 0) {
		LOG_WRN("Failed to set shell prompt: %d", ret);
	}
}
#endif

int main(void)
{
	LOG_INF("Mission FW Main Loop entry - " CONFIG_BOARD_TARGET);

	struct gr_smc_chipid id = gr_smc_get_chipid();

	LOG_INF("Chip %s instance %u", chip_type_name(id.type), id.instance);

#ifdef TT_MIS_SHELL_PROMPT_ENABLED
	set_shell_prompt(&id);
#endif

	while (1) {
		k_msleep(100);
	}

	return 0;
}
