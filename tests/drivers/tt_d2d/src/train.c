/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Train D2D links. MM: M0.D2D1 <-> M1.D2D0. MMK adds M1.D2D1 <-> K0.D2D2.
 *
 * Both Mimirs share one image and one overlay; the die is identified from
 * the reset unit chip id. The host releases every Rocket together after each die writes
 * TS_D2D_WAIT_TRAINING -- neither end starts itself.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/ztest.h>

#include <soc.h>

#include <zephyr/drivers/misc/tt_d2d.h>

#include <platform.h>
#include <test_codes.h>

#include "chip_init.h"
#include "d2d_fw_blob.h"

/*
 * Chip id from the SMC reset unit, so no host has to publish it. Keraunos is 0,
 * distinguished by which image is running.
 */
#define RESET_UNIT_CHIP_ID_REG 0xC0002008U
#define CHIP_ID_MASK      0xFFU
#define CHIP_ID_MIMIR0    0x03U
#define CHIP_ID_MIMIR1    0x13U
#define CHIP_ID_KERAUNOS0 0x00U

/*
 * Mimir SMN D2D-select mux (sideband manager FLAGOUTSET0 / FLAGOUTCLR0 in
 * mimir_smn_a_s__memorymap_smc2smn_iniu.h). The drop's full register names run
 * past 100 columns, so base + the offsets from the same header. Keraunos has no
 * such register.
 */
#if !defined(CONFIG_SOC_TT_KERAUNOS_SMC)
#define SMN_D2D_SELECT_BASE                                                                        \
	SMN_SMC2SMN_INIU_SIDEBANDMANAGER_D2D_SELECT_MAIN_SIDEBANDMANAGER_A_REG_MAP_BASE_ADDR
#define SMN_D2D_SELECT_SET (SMN_D2D_SELECT_BASE + 0x50U)
#define SMN_D2D_SELECT_CLR (SMN_D2D_SELECT_BASE + 0x54U)
#endif

struct link_tile {
	const struct device *dev;
	uintptr_t base;
};

#define TILE(label)                                                                                \
	{                                                                                          \
		.dev = DEVICE_DT_GET(DT_NODELABEL(label)),                                         \
		.base = DT_REG_ADDR(DT_NODELABEL(label)),                                          \
	}

/* All status-okay tiles (straps sample the current clock; PLL switch is chip-wide). */
#define TILE_ENTRY(node_id)                                                                        \
	{                                                                                          \
		.dev = DEVICE_DT_GET(node_id),                                                     \
		.base = DT_REG_ADDR(node_id),                                                      \
	},

static const struct link_tile chip_tiles[] = {
	DT_FOREACH_STATUS_OKAY(tenstorrent_grendel_d2d, TILE_ENTRY)};

/* Per chip id, which tiles face another die in this package. */
#if defined(CONFIG_SOC_TT_KERAUNOS_SMC)

/* Keraunos reaches Mimir1 through D2D2, in HSIO tile 2. */
static const struct link_tile tiles_keraunos0[] = {TILE(d2d2)};

#else

/* Mimir0 reaches Mimir1 through D2D1; Mimir1 answers on D2D0. */
static const struct link_tile tiles_mimir0[] = {TILE(d2d1)};

static const struct link_tile tiles_mimir1[] = {
	TILE(d2d0),
#ifdef CONFIG_TT_D2D_TEST_TRAIN_KERAUNOS
	TILE(d2d1), /* faces Keraunos0 D2D2 */
#endif
};

#endif

#define RELEASE_TIMEOUT K_SECONDS(5)
#define TRAIN_SLICE     K_MSEC(10)
#define TRAIN_SLICES    100U

/* Duplicated from the driver so a shared header cannot hide a mismatch. */
#define CPU_CTRL_OFFSET 0x1800U
#define CPU_CTRL_HALT   0x00010001U
#define CPU_CTRL_START  0x00010000U

#define STEP(step)                  WRITE_SCRATCH(3, 0xB1000000U | (step))
#define STEP_RESULT(val)            WRITE_SCRATCH(4, (uint32_t)(val))
#define STEP_RESULT_TILE(tile, val) WRITE_SCRATCH(4 + ((tile) * 2), (uint32_t)(val))

static const struct link_tile *link_tiles;
static size_t link_count;
static uint32_t chip_id;

static const struct link_tile *link_tiles_for(uint32_t id, size_t *count)
{
#if defined(CONFIG_SOC_TT_KERAUNOS_SMC)
	if (id == CHIP_ID_KERAUNOS0) {
		*count = ARRAY_SIZE(tiles_keraunos0);
		return tiles_keraunos0;
	}
#else
	if (id == CHIP_ID_MIMIR0) {
		*count = ARRAY_SIZE(tiles_mimir0);
		return tiles_mimir0;
	}

	if (id == CHIP_ID_MIMIR1) {
		*count = ARRAY_SIZE(tiles_mimir1);
		return tiles_mimir1;
	}
#endif

	*count = 0;
	return NULL;
}

/* Separate from the train wait: an unreleased Rocket looks like a dead link. */
static int wait_for_release(void)
{
	k_timepoint_t deadline = sys_timepoint_calc(RELEASE_TIMEOUT);

	do {
		size_t running = 0;

		for (size_t i = 0; i < link_count; i++) {
			if (sys_read32(link_tiles[i].base + CPU_CTRL_OFFSET) == CPU_CTRL_START) {
				running++;
			}
		}

		if (running == link_count) {
			return 0;
		}

		k_msleep(1);
	} while (!sys_timepoint_expired(deadline));

	return -ETIMEDOUT;
}

static void *train_setup(void)
{
	chip_id = sys_read32(RESET_UNIT_CHIP_ID_REG) & CHIP_ID_MASK;
	WRITE_SCRATCH(5, chip_id);

	STEP(1);
	for (size_t i = 0; i < ARRAY_SIZE(chip_tiles); i++) {
		zassert_true(device_is_ready(chip_tiles[i].dev), "%s not ready",
			     chip_tiles[i].dev->name);
	}

	STEP(2);
	link_tiles = link_tiles_for(chip_id, &link_count);
	zassert_not_null(link_tiles, "reset unit chip id 0x%02x is not a die this image knows", chip_id);

	STEP(3);
	zassert_ok(tt_d2d_test_chip_init(), "chip init failed");

	STEP(4);
	for (size_t i = 0; i < ARRAY_SIZE(chip_tiles); i++) {
		zassert_ok(tt_d2d_reset_release(chip_tiles[i].dev), "%s reset release failed",
			   chip_tiles[i].dev->name);
	}

	STEP(5);
	zassert_ok(tt_d2d_test_chip_clock_up(), "clock switch failed");

	/* Mimir0 only: SMN mux onto D2D1. The drop does not set it on M1. */
#if !defined(CONFIG_SOC_TT_KERAUNOS_SMC)
	if (chip_id == CHIP_ID_MIMIR0) {
		STEP(6);
		sys_write32(0x1, SMN_D2D_SELECT_CLR);
		sys_write32(0x1, SMN_D2D_SELECT_SET);
	}
#endif

	STEP(7);

	return NULL;
}

ZTEST(tt_d2d_train, test_link_trains)
{
	const uint8_t *img;
	size_t size = 0;
	int ret;

#ifdef D2D_FW_STUB_UNIT
	ztest_test_skip();
#endif

	STEP(8);
	img = d2d_fw_image(&size);
	zassert_not_null(img, "no firmware image linked in");
	zassert_true(size > 0, "linked firmware image is empty");

	for (size_t i = 0; i < link_count; i++) {
		const struct link_tile *tile = &link_tiles[i];

		STEP(9);
		ret = tt_d2d_load_fw(tile->dev, img, size);
		STEP_RESULT_TILE(i, ret);
		zassert_ok(ret, "loading %s failed", tile->dev->name);

		STEP(10);
		zassert_equal(sys_read32(tile->base + CPU_CTRL_OFFSET), CPU_CTRL_HALT,
			      "%s should still be halted after a load", tile->dev->name);
	}

	STEP(11);
	WRITE_SCRATCH(0, TS_D2D_WAIT_TRAINING);

	STEP(12);
	zassert_ok(wait_for_release(),
		   "Rocket never released; host waits for TS_D2D_WAIT_TRAINING on every die");

	/* Sliced so scratch tracks progress; a long single wait looks wedged. */
	STEP(13);
	for (size_t i = 0; i < link_count; i++) {
		const struct link_tile *tile = &link_tiles[i];

		for (unsigned int slice = 0; slice < TRAIN_SLICES; slice++) {
			ret = tt_d2d_wait_link(tile->dev, TRAIN_SLICE);
			STEP_RESULT_TILE(i, tt_d2d_progress_code(tile->dev));
			if (ret != -ETIMEDOUT) {
				break;
			}
		}

		zassert_ok(ret, "%s did not train; the far die must have reached this point too",
			   tile->dev->name);
	}

	STEP(14);
}

/* Failures must write TS_FAIL; leaving TS_D2D_WAIT_TRAINING makes the host hang. */
static void train_teardown(void *fixture)
{
	ARG_UNUSED(fixture);

	for (struct ztest_unit_test *t = _ztest_unit_test_list_start; t < _ztest_unit_test_list_end;
	     t++) {
		if (t->stats->fail_count != 0) {
			WRITE_SCRATCH(0, TS_FAIL);
			return;
		}
	}
}

ZTEST_SUITE(tt_d2d_train, NULL, train_setup, NULL, NULL, train_teardown);
