/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/coverage/gcov_transport.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>

#include <tenstorrent/coverage_handoff.h>

BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(DT_NODELABEL(coverage_handoff)),
	     "coverage_handoff node must be enabled");

#define HANDOFF_REG           DT_REG_ADDR(DT_NODELABEL(coverage_handoff))
#define HANDOFF_TIMEOUT_MS    5000

static bool handoff_wait_clear(void)
{
	int64_t deadline = k_uptime_get() + HANDOFF_TIMEOUT_MS;

	while (sys_read32(HANDOFF_REG) != 0) {
		if (k_uptime_get() > deadline) {
			return false;
		}
	}
	return true;
}

int coverage_transport_send(const char *filename, const uint8_t *buffer, size_t size)
{
	ARG_UNUSED(filename);

	if (!handoff_wait_clear()) {
		return -ETIMEDOUT;
	}

	printk("%s%u\n", TT_COV_HANDOFF_MARKER, (unsigned int)size);
	sys_write32((uint32_t)(uintptr_t)buffer, HANDOFF_REG);

	/* The host clears the register once it has read the bytes. */
	if (!handoff_wait_clear()) {
		return -ETIMEDOUT;
	}
	return 0;
}
