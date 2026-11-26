/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <soc.h>
#include <tenstorrent/uart_tt_virt.h>

/* The console VUART is the device under test; configure is validated before any state changes. */
#define UART_DEV DEVICE_DT_GET(DT_CHOSEN(zephyr_console))

static const struct uart_config valid_cfg = {
	.baudrate = 115200,
	.parity = UART_CFG_PARITY_NONE,
	.stop_bits = UART_CFG_STOP_BITS_1,
	.data_bits = UART_CFG_DATA_BITS_8,
	.flow_ctrl = UART_CFG_FLOW_CTRL_RTS_CTS,
};

ZTEST_SUITE(uart_tt_virt, NULL, NULL, NULL, NULL, NULL);

/**
 * @brief A valid configuration with RTS/CTS flow control is accepted.
 * @details Positive control for the flow-control rejection test: the same call with a
 * valid value must succeed, so the rejection below is specific to the bad value.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_configure_valid_flow_ctrl)
{
	int err = uart_configure(UART_DEV, &valid_cfg);

	zassert_equal(err, 0, "valid configuration rejected: %d", err);
}

/**
 * @brief A flow-control value outside the enum range is rejected.
 * @details Sets flow_ctrl to 7, which is outside UART_CFG_FLOW_CTRL_NONE..RTS_CTS, and expects
 * uart_configure() to return -EINVAL. The driver validates flow control before storing the
 * configuration, so the stored settings are unchanged.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_configure_invalid_flow_ctrl)
{
	struct uart_config cfg = valid_cfg;
	int err;

	cfg.flow_ctrl = 7;
	err = uart_configure(UART_DEV, &cfg);

	zassert_equal(err, -EINVAL, "invalid flow_ctrl not rejected with -EINVAL: %d", err);
}

/**
 * @brief A NULL configuration is rejected.
 * @details Passes a NULL configuration pointer and expects uart_configure() to return -EINVAL
 * rather than dereference it.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_configure_null_config)
{
	int err = uart_configure(UART_DEV, NULL);

	zassert_equal(err, -EINVAL, "NULL configuration not rejected with -EINVAL: %d", err);
}

/**
 * @brief A stop-bits value outside the enum range is rejected.
 * @details Sets stop_bits to 7, outside UART_CFG_STOP_BITS_0_5..2, and expects uart_configure()
 * to return -EINVAL. The driver validates stop bits before storing the configuration.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_configure_invalid_stop_bits)
{
	struct uart_config cfg = valid_cfg;
	int err;

	cfg.stop_bits = 7;
	err = uart_configure(UART_DEV, &cfg);

	zassert_equal(err, -EINVAL, "invalid stop_bits not rejected with -EINVAL: %d", err);
}

/**
 * @brief A data-bits value outside the enum range is rejected.
 * @details Sets data_bits to 7, outside UART_CFG_DATA_BITS_5..8, and expects uart_configure()
 * to return -EINVAL. The driver validates data bits before storing the configuration.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_configure_invalid_data_bits)
{
	struct uart_config cfg = valid_cfg;
	int err;

	cfg.data_bits = 7;
	err = uart_configure(UART_DEV, &cfg);

	zassert_equal(err, -EINVAL, "invalid data_bits not rejected with -EINVAL: %d", err);
}

static void noop_uart_callback(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
}

/**
 * @brief Interrupt enable and disable paths run with and without a callback.
 * @details Enables TX with no callback set, so the service path must run without one, then
 * disables it. Enables and disables RX and TX with a no-op callback set. The callback is cleared
 * at the end. Each call must return without faulting.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_irq_enable_disable_paths)
{
	uart_irq_callback_set(UART_DEV, NULL);
	uart_irq_tx_enable(UART_DEV);
	k_msleep(10);
	uart_irq_tx_disable(UART_DEV);

	uart_irq_callback_set(UART_DEV, noop_uart_callback);
	uart_irq_rx_enable(UART_DEV);
	uart_irq_rx_disable(UART_DEV);
	uart_irq_tx_enable(UART_DEV);
	uart_irq_tx_disable(UART_DEV);

	uart_irq_callback_set(UART_DEV, NULL);
}

/* The version the driver compares against: the register address in the MS byte, then the
 * version property in the low 24 bits (see uart_tt_virt config in the driver).
 */
#define VUART_CFG_VERSION(node)                                                                    \
	((DT_REG_ADDR(node) << 24) | (DT_PROP(node, version) & GENMASK(23, 0)))

/* Write a descriptor header at a VUART memory region, as a previous boot stage would leave it. */
static void fill_descriptor(volatile struct tt_vuart *desc, uint32_t magic, uint32_t version,
			    uint32_t rx_cap, uint32_t tx_cap, uint32_t tx_tail)
{
	desc->magic = magic;
	desc->version = version;
	desc->rx_cap = rx_cap;
	desc->rx_head = 0;
	desc->rx_tail = 0;
	desc->tx_cap = tx_cap;
	desc->tx_head = 0;
	desc->tx_tail = tx_tail;
	desc->tx_oflow = 0;
}

/**
 * @brief A stale descriptor with the wrong TX capacity is not adopted.
 * @details Writes a descriptor whose magic, version and RX capacity match the device but whose TX
 * capacity does not, with a stale tail. Points SCRATCH[2] at it and runs the deferred init. The
 * driver must reject the descriptor and reinitialise the header: TX capacity is restored and the
 * stale tail is cleared. SCRATCH[2] is restored before the checks.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_adopt_mismatch_reinitialises)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vuart2));
	volatile struct tt_vuart *desc =
		(volatile struct tt_vuart *)(uintptr_t)DT_REG_ADDR(DT_NODELABEL(vuart2_ram));
	uint32_t saved = READ_SCRATCH(2);
	int err;

	fill_descriptor(desc, DT_PROP(DT_NODELABEL(vuart2), magic),
			VUART_CFG_VERSION(DT_NODELABEL(vuart2)),
			DT_PROP(DT_NODELABEL(vuart2), rx_cap),
			0 /* wrong TX capacity */, 5 /* stale tail */);

	WRITE_SCRATCH(2, (uint32_t)(uintptr_t)desc);
	err = device_init(dev);
	WRITE_SCRATCH(2, saved);

	zassert_equal(err, 0, "deferred init failed: %d", err);
	zassert_equal(desc->tx_cap, DT_PROP(DT_NODELABEL(vuart2), tx_cap),
		      "mismatched descriptor was adopted: tx_cap=%u", desc->tx_cap);
	zassert_equal(desc->tx_tail, 0, "stale tail survived reinitialisation: %u", desc->tx_tail);
}

/**
 * @brief A matching descriptor is adopted and its buffered data is kept.
 * @details Writes a descriptor that matches the device in every field and carries a stale tail,
 * points SCRATCH[2] at it, and runs the deferred init. The driver must adopt the descriptor, so
 * the tail is left as it was. SCRATCH[2] is restored before the checks.
 * @ingroup tests_uart_tt_virt
 */
ZTEST(uart_tt_virt, test_adopt_matching_keeps_state)
{
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(vuart3));
	volatile struct tt_vuart *desc =
		(volatile struct tt_vuart *)(uintptr_t)DT_REG_ADDR(DT_NODELABEL(vuart3_ram));
	uint32_t saved = READ_SCRATCH(2);
	int err;

	fill_descriptor(desc, DT_PROP(DT_NODELABEL(vuart3), magic),
			VUART_CFG_VERSION(DT_NODELABEL(vuart3)),
			DT_PROP(DT_NODELABEL(vuart3), rx_cap),
			DT_PROP(DT_NODELABEL(vuart3), tx_cap), 5 /* buffered data */);

	WRITE_SCRATCH(2, (uint32_t)(uintptr_t)desc);
	err = device_init(dev);
	WRITE_SCRATCH(2, saved);

	zassert_equal(err, 0, "deferred init failed: %d", err);
	zassert_equal(desc->tx_tail, 5, "matching descriptor was not adopted: tx_tail=%u",
		      desc->tx_tail);
	zassert_equal(desc->tx_cap, DT_PROP(DT_NODELABEL(vuart3), tx_cap),
		      "adopted descriptor changed tx_cap: %u", desc->tx_cap);
}
