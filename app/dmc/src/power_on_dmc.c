/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/* Debug monitor for the POWER_ON-dmc input: logs every edge and exposes a shell command. */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

#if DT_NODE_EXISTS(DT_NODELABEL(power_on_dmc))

LOG_MODULE_REGISTER(power_on_dmc, LOG_LEVEL_INF);

static const struct gpio_dt_spec power_on = GPIO_DT_SPEC_GET(DT_NODELABEL(power_on_dmc), gpios);
static struct gpio_callback power_on_cb;
static atomic_t fall_count;
static atomic_t rise_count;
static int64_t last_fall_ms = -1;

static void power_on_changed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (gpio_pin_get_dt(&power_on)) {
		atomic_inc(&rise_count);
		LOG_WRN("POWER_ON-dmc rose at %lld ms", k_uptime_get());
	} else {
		atomic_inc(&fall_count);
		last_fall_ms = k_uptime_get();
		LOG_WRN("POWER_ON-dmc fell at %lld ms", last_fall_ms);
	}
}

static int power_on_dmc_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&power_on)) {
		LOG_ERR("POWER_ON-dmc GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&power_on, GPIO_INPUT);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_pin_configure_dt", ret);
		return ret;
	}

	gpio_init_callback(&power_on_cb, power_on_changed, BIT(power_on.pin));
	ret = gpio_add_callback_dt(&power_on, &power_on_cb);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_add_callback_dt", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&power_on, GPIO_INT_EDGE_BOTH);
	if (ret != 0) {
		LOG_ERR("%s() failed: %d", "gpio_pin_interrupt_configure_dt", ret);
		return ret;
	}

	LOG_INF("POWER_ON-dmc monitor armed, initial level %d", gpio_pin_get_dt(&power_on));

	return 0;
}

SYS_INIT(power_on_dmc_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#if defined(CONFIG_SHELL)
static int cmd_power_on(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "POWER_ON-dmc=%d falls=%ld rises=%ld last_fall_ms=%lld uptime_ms=%lld",
		    gpio_pin_get_dt(&power_on), (long)atomic_get(&fall_count),
		    (long)atomic_get(&rise_count), last_fall_ms, k_uptime_get());

	return 0;
}

SHELL_CMD_REGISTER(power_on, NULL, "Show POWER_ON-dmc level and edge counts", cmd_power_on);
#endif /* CONFIG_SHELL */

#endif /* DT_NODE_EXISTS(DT_NODELABEL(power_on_dmc)) */
