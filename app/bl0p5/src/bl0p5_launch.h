/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BL0P5_LAUNCH_H_
#define BL0P5_LAUNCH_H_

#include <stdint.h>

/**
 * @brief Addresses and hooks the BL0.5 launcher needs.
 *
 * main() fills this in with the devicetree and SoC addresses. The tests fill it in with RAM
 * buffers and fake hooks, so the launcher runs on the host.
 */
struct bl0p5_regs {
	/** Host boot state register, written and polled by the launcher. */
	uintptr_t host_boot_state;
	/** Bundle validation register, written and polled by the launcher. */
	uintptr_t bundle_validation;
	/** Base of the BUN2 staging area that the host writes the bundle into. */
	uintptr_t staging_area;
	/** SMC reset vector 0, which holds the entry point of the image that is jumped to. */
	uintptr_t reset_vector;
	/** Polls allowed for each host wait (staging, then validation), before giving up. */
	uint32_t max_polls;
	/** Called between register polls while waiting for the host. */
	void (*pause)(void);
	/** Transfers control to the image at @p entry_point. Normally does not return. */
	void (*jump)(uintptr_t entry_point);
};

/**
 * @brief Run the BL0.5 handshake, copy the SMC BL1 image out of the bundle, and jump to it.
 *
 * Returns only if the handoff does not happen. Each host wait (staging, then validation) polls at
 * most regs->max_polls times. If the host does not finish in time, the launcher returns
 * -ETIMEDOUT without copying or jumping. It also returns 0 if @c jump returns, which the hardware
 * never does.
 *
 * @param regs Addresses and hooks to use. Must not be NULL.
 * @retval 0 The jump returned.
 * @retval -ENOENT No SMC BL1 image at TOC index 0 in the staged bundle.
 * @retval -ETIMEDOUT The host did not stage or did not validate within regs->max_polls polls.
 */
int bl0p5_launch(const struct bl0p5_regs *regs);

#endif /* BL0P5_LAUNCH_H_ */
