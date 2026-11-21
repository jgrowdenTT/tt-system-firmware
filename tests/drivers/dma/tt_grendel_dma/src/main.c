/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/ztest.h>

#define DMA_DEV      DEVICE_DT_GET(DT_NODELABEL(dma0))
#define NUM_CHANNELS 16U
#define XFER_LEN     64U
#define UNUSED_CH    15U /* never configured by any test in this suite */

static uint8_t tx_buf[XFER_LEN] __aligned(8);
static uint8_t rx_buf[XFER_LEN] __aligned(8);
static volatile int last_dma_status = -1;

static void dma_done(const struct device *dev, void *user_data, uint32_t channel, int status)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
	ARG_UNUSED(channel);
	last_dma_status = status;
}

static void fill_config(struct dma_config *cfg, struct dma_block_config *blk, const void *src,
			void *dst, uint32_t len)
{
	memset(cfg, 0, sizeof(*cfg));
	memset(blk, 0, sizeof(*blk));

	cfg->channel_direction = MEMORY_TO_MEMORY;
	cfg->source_data_size = 1U;
	cfg->dest_data_size = 1U;
	cfg->source_burst_length = 1U;
	cfg->dest_burst_length = 1U;
	cfg->dma_callback = dma_done;
	cfg->block_count = 1U;
	cfg->head_block = blk;

	blk->block_size = len;
	blk->source_address = (uintptr_t)src;
	blk->dest_address = (uintptr_t)dst;
}

ZTEST_SUITE(tt_grendel_dma, NULL, NULL, NULL, NULL, NULL);

/**
 * @brief Channel numbers outside the controller are rejected by config, start and status.
 * @details Uses channel NUM_CHANNELS, the first invalid index. Each call must return -EINVAL
 * without touching a channel.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_invalid_channel_rejected)
{
	struct dma_config cfg;
	struct dma_block_config blk;
	struct dma_status st;

	fill_config(&cfg, &blk, tx_buf, rx_buf, XFER_LEN);

	zassert_equal(dma_config(DMA_DEV, NUM_CHANNELS, &cfg), -EINVAL);
	zassert_equal(dma_start(DMA_DEV, NUM_CHANNELS), -EINVAL);
	zassert_equal(dma_get_status(DMA_DEV, NUM_CHANNELS, &st), -EINVAL);
}

/**
 * @brief A configuration with no blocks, or no head block, is rejected.
 * @details Tries block_count 0 with a valid head block, and block_count 1 with a NULL head block.
 * Both must return -EINVAL.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_config_without_blocks_rejected)
{
	struct dma_config cfg;
	struct dma_block_config blk;

	fill_config(&cfg, &blk, tx_buf, rx_buf, XFER_LEN);

	cfg.block_count = 0U;
	zassert_equal(dma_config(DMA_DEV, 0, &cfg), -EINVAL);

	cfg.block_count = 1U;
	cfg.head_block = NULL;
	zassert_equal(dma_config(DMA_DEV, 0, &cfg), -EINVAL);
}

/**
 * @brief A block chain longer than CONFIG_DMA_TT_GRENDEL_SMC_MAX_BLOCKS is rejected.
 * @details Builds a chain one block longer than the driver's limit and sets block_count to match.
 * The driver must return -EINVAL before copying any block.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_config_too_many_blocks_rejected)
{
	struct dma_config cfg;
	struct dma_block_config blks[CONFIG_DMA_TT_GRENDEL_SMC_MAX_BLOCKS + 1];

	fill_config(&cfg, &blks[0], tx_buf, rx_buf, XFER_LEN);
	for (unsigned int i = 0; i < ARRAY_SIZE(blks); i++) {
		memset(&blks[i], 0, sizeof(blks[i]));
		blks[i].block_size = XFER_LEN;
		blks[i].source_address = (uintptr_t)tx_buf;
		blks[i].dest_address = (uintptr_t)rx_buf;
		blks[i].next_block = (i + 1 < ARRAY_SIZE(blks)) ? &blks[i + 1] : NULL;
	}

	cfg.block_count = ARRAY_SIZE(blks);
	zassert_equal(dma_config(DMA_DEV, 0, &cfg), -EINVAL);
}

/**
 * @brief Starting a channel that was never configured is rejected.
 * @details Channel UNUSED_CH is not configured in this suite, so dma_start() must return -EINVAL
 * without touching the hardware.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_start_unconfigured_rejected)
{
	zassert_equal(dma_start(DMA_DEV, UNUSED_CH), -EINVAL);
}

/**
 * @brief Status on a configured channel before any transfer reports no pending data.
 * @details Configures channel 0 without starting it. The status query must succeed and report
 * zero pending length, since no transfer has been issued.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_status_before_start)
{
	struct dma_config cfg;
	struct dma_block_config blk;
	struct dma_status st = { 0 };

	fill_config(&cfg, &blk, tx_buf, rx_buf, XFER_LEN);
	zassert_equal(dma_config(DMA_DEV, 0, &cfg), 0);

	zassert_equal(dma_get_status(DMA_DEV, 0, &st), 0);
	zassert_equal(st.pending_length, 0, "pending length before start: %u", st.pending_length);
}

/**
 * @brief A single-block memory-to-memory transfer copies the data and completes.
 * @details Copies XFER_LEN bytes on channel 1 and checks that the completion callback reports
 * DMA_STATUS_COMPLETE, the destination matches the source, and no data is pending afterwards.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_single_block_transfer)
{
	struct dma_config cfg;
	struct dma_block_config blk;
	struct dma_status st = { 0 };

	for (unsigned int i = 0; i < XFER_LEN; i++) {
		tx_buf[i] = (uint8_t)(i ^ 0x5A);
	}
	memset(rx_buf, 0, sizeof(rx_buf));

	fill_config(&cfg, &blk, tx_buf, rx_buf, XFER_LEN);
	last_dma_status = -1;

	zassert_equal(dma_config(DMA_DEV, 1, &cfg), 0);
	zassert_equal(dma_start(DMA_DEV, 1), 0);
	zassert_equal(last_dma_status, DMA_STATUS_COMPLETE, "callback status: %d",
		      last_dma_status);
	zassert_mem_equal(rx_buf, tx_buf, XFER_LEN);

	zassert_equal(dma_get_status(DMA_DEV, 1, &st), 0);
	zassert_equal(st.pending_length, 0, "pending length after completion: %u",
		      st.pending_length);
}

/**
 * @brief A two-block chain copies every block in order.
 * @details Links two XFER_LEN/2 blocks on channel 2 so the driver walks next_block. Both halves
 * must reach the destination and the completion callback must report success.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_chained_blocks_transfer)
{
	struct dma_config cfg;
	struct dma_block_config blks[2];
	const uint32_t half = XFER_LEN / 2;

	for (unsigned int i = 0; i < XFER_LEN; i++) {
		tx_buf[i] = (uint8_t)(0xC0 + i);
	}
	memset(rx_buf, 0, sizeof(rx_buf));

	fill_config(&cfg, &blks[0], tx_buf, rx_buf, half);
	memset(&blks[1], 0, sizeof(blks[1]));
	blks[1].block_size = half;
	blks[1].source_address = (uintptr_t)(tx_buf + half);
	blks[1].dest_address = (uintptr_t)(rx_buf + half);
	blks[0].next_block = &blks[1];
	cfg.block_count = 2U;
	last_dma_status = -1;

	zassert_equal(dma_config(DMA_DEV, 2, &cfg), 0);
	zassert_equal(dma_start(DMA_DEV, 2), 0);
	zassert_equal(last_dma_status, DMA_STATUS_COMPLETE, "callback status: %d",
		      last_dma_status);
	zassert_mem_equal(rx_buf, tx_buf, XFER_LEN);
}

/**
 * @brief Stopping a channel returns success.
 * @details dma_stop() on a configured channel must return 0.
 * @ingroup tests_tt_grendel_dma
 */
ZTEST(tt_grendel_dma, test_stop_returns_ok)
{
	zassert_equal(dma_stop(DMA_DEV, 2), 0);
}
