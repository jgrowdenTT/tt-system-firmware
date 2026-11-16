/*
 * Copyright (c) 2025 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/fff.h>
#include <tenstorrent/tt_smbus_regs.h>
#include <zephyr/drivers/i2c.h>
#include <tenstorrent/smbus_target.h>
#include "reg_mock.h"
#include "asic_state.h"
#include "telemetry.h"
#include "status_reg.h"
#include "cm2dm_msg.h"

static const struct device *const i2c0_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));
static const struct device *const smbus_target_dev =
	DEVICE_DT_GET_OR_NULL(DT_NODELABEL(smbus_target0));
static const uint8_t tt_i2c_addr = 0xA;

static uint32_t get_smbus_error_count(void)
{
	return smbus_target_get_error_count(smbus_target_dev);
}
static void tear_down_tc(void *fixture)
{
	(void)fixture;

	/*Whiteboxing this isn't great */
	struct i2c_target_config *smbus_target_cfg =
		(struct i2c_target_config *)smbus_target_dev->data;

	smbus_target_cfg->callbacks->stop(smbus_target_cfg);
}

/**
 * @brief Writing command 0 is rejected.
 * @details Sends command 0, which is not valid, and checks the write fails and the SMBus error
 * count rises by one.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_received_bad_cmd_0)
{
	/* 0 is not a valid command */
	uint8_t write_data[] = {0U};
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief Writing command 255 is rejected.
 * @details Sends command 255, which is not valid, and checks the write fails and the error count
 * rises by one.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_received_bad_cmd_255)
{
	/* 255 is not a valid command */
	uint8_t write_data[] = {255U};
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief Writing the out-of-range command CMFW_SMBUS_MSG_MAX is rejected.
 * @details Checks the write fails and the error count rises by one.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_received_bad_cmd_msg_max)
{
	uint8_t write_data[] = {CMFW_SMBUS_MSG_MAX};
	uint32_t err_count_before = get_smbus_error_count();

	/* 255 is not a valid command */
	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A write with an invalid command byte followed by another is rejected.
 * @details Sends command 0 followed by CMFW_SMBUS_MSG_MAX in one write. Checks it fails and counts
 * one error.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_received_cmd_before_stop)
{
	uint8_t write_data[] = {0, CMFW_SMBUS_MSG_MAX};
	uint32_t err_count_before = get_smbus_error_count();

	/* starting a valid command before stopping the invalid command should fail */
	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A read with no preceding command is rejected.
 * @details Reads without first writing a command and checks the read fails, returns 0xFF and
 * counts one error.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_unsolicited_read_received)
{
	uint8_t read_data[1];
	uint32_t err_count_before = get_smbus_error_count();

	/* We always need to get a command to process first. */
	zassert_equal(-1, i2c_read(i2c0_dev, read_data, sizeof(read_data), tt_i2c_addr));
	zexpect_equal(0xFF, read_data[0]);
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A block write with a mismatched byte count is rejected.
 * @details Sends a block write declaring five data bytes but carrying an extra byte. Checks it
 * fails and counts one error.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_received_bad_blocksize)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_WRITE_BLOCK, 5U, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 54U};
	uint32_t err_count_before = get_smbus_error_count();

	/* WRITE_BLOCK is a valid command */
	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A read-only command given data to write is rejected.
 * @details Sends TEST_READ with an extra data byte and checks the write fails and counts one
 * error.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_received_data_for_read_cmd)
{
	/* TEST_READ is a valid command but expects the user to write no additional data */
	uint8_t write_data[] = {CMFW_SMBUS_TEST_READ, 10U};
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A write-only command issued as a write-read is rejected.
 * @details Issues a combined write-then-read for TEST_WRITE_BLOCK. Checks it fails, returns 0xFF
 * and counts one error.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_rx_rqst_for_write_cmd)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_WRITE_BLOCK, 4U};
	uint8_t read_data[1];
	uint32_t err_count_before = get_smbus_error_count();
	/* TEST_WRITE_BLOCK is a valid command but expects data to be written, not read */
	zassert_equal(-1, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					 read_data, sizeof(read_data)));
	zexpect_equal(0xFF, read_data[0]);
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A byte read returns the stored register value.
 * @details Sets the register to 0x5A, reads TEST_READ and checks the byte is 0x5A with no error
 * counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_read_byte_test)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_READ};
	uint8_t read_data[2];
	uint32_t err_count_before = get_smbus_error_count();

	ReadReg_fake.return_val = 0x5AU;
	/* The value received by var should match what the fake stored */
	zassert_equal(0, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					read_data, sizeof(read_data)));
	zexpect_equal(0x5AU, read_data[0]);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A word read returns the two bytes of the stored value.
 * @details Sets the register to 0x915A, reads TEST_READ_WORD and checks the bytes are 0x5A then
 * 0x91. No error is counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_read_word_test)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_READ_WORD};
	uint8_t read_data[3];
	uint32_t err_count_before = get_smbus_error_count();

	ReadReg_fake.return_val = 0x915AU;

	/* The value received by var should match what the fake stored */
	zassert_equal(0, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					read_data, sizeof(read_data)));
	zexpect_equal(0x5AU, read_data[0]);
	zexpect_equal(0x91U, read_data[1]);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A block read returns a length byte followed by the stored data.
 * @details Sets the register to 0x8765915A and reads TEST_READ_BLOCK. Checks the length is 4, the
 * four data bytes follow in order, and no error is counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_read_block_test)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_READ_BLOCK};
	uint8_t read_data[6];
	uint32_t err_count_before = get_smbus_error_count();

	ReadReg_fake.return_val = 0x8765915AU;
	/* The value received by var should match what the fake stored */
	zassert_equal(0, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					read_data, sizeof(read_data)));
	zexpect_equal(4U, read_data[0]);
	zexpect_equal(0x5AU, read_data[1]);
	zexpect_equal(0x91U, read_data[2]);
	zexpect_equal(0x65U, read_data[3]);
	zexpect_equal(0x87U, read_data[4]);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A byte write stores the value in the scratch register.
 * @details Writes one byte with TEST_WRITE. Checks the scratch register is written once with 0x5B
 * tagged with length one.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_byte_test)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_WRITE, 0x5BU, 136U};
	uint8_t set_count = 0U;
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	for (uint8_t i = 0U; i < WriteReg_fake.call_count; i++) {
		if (WriteReg_fake.arg0_history[i] == STATUS_FW_SCRATCH_REG_ADDR) {
			zassert_equal(WriteReg_fake.arg1_history[i], (1 << 16U) | 0x5BU);
			set_count++;
		}
	}
	zexpect_equal(1U, set_count);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A word write stores two bytes in the scratch register.
 * @details Writes two bytes with TEST_WRITE_WORD. Checks the scratch register is written once with
 * 0x2A5B tagged with length two.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_word_test)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_WRITE_WORD, 0x5BU, 0x2AU, 177U};
	uint8_t set_count = 0U;
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));

	for (uint8_t i = 0U; i < WriteReg_fake.call_count; i++) {
		if (WriteReg_fake.arg0_history[i] == STATUS_FW_SCRATCH_REG_ADDR) {
			zexpect_equal(WriteReg_fake.arg1_history[i], (2 << 16U) | 0x2A5BU);
			set_count++;
		}
	}
	zexpect_equal(1U, set_count);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A block write stores its data bytes in the scratch register.
 * @details Writes a four-byte block with TEST_WRITE_BLOCK. Checks the scratch register is written
 * once with the bytes packed in order.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_write_block_test)
{
	uint8_t write_data[] = {CMFW_SMBUS_TEST_WRITE_BLOCK, 0x4, 0x5BU, 0x2AU, 0x13U, 0x99U, 243U};
	uint8_t set_count = 0U;
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));

	for (uint8_t i = 0U; i < WriteReg_fake.call_count; i++) {
		if (WriteReg_fake.arg0_history[i] == STATUS_FW_SCRATCH_REG_ADDR) {
			zexpect_equal(WriteReg_fake.arg1_history[i], 0x99132A5BU);
			set_count++;
		}
	}
	zexpect_equal(1U, set_count);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief Updating the ARC state to 3 sets the ASIC to state A3.
 * @details Sends an update-ARC-state request with state 3 and checks the ASIC is in A3 with no
 * error counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_update_arc_test_state_3)
{
	uint8_t write_data[] = {CMFW_SMBUS_UPDATE_ARC_STATE, 0x3, 0x3U, 0xDEU, 0xAFU};
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(A3State, get_asic_state());
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief Updating the ARC state to 0 sets the ASIC to state A0.
 * @details Sends an update-ARC-state request with state 0 and checks the ASIC is in A0 with no
 * error counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_update_arc_test_state_0)
{
	uint8_t write_data[] = {CMFW_SMBUS_UPDATE_ARC_STATE, 0x3, 0x0U, 0xDEU, 0xAFU};
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(A0State, get_asic_state());
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A telemetry read with a wrong block length is rejected.
 * @details Sends a telemetry read declaring a length of three where one is expected. Checks it
 * fails and counts one error.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_telem_read_bad_w_blocksize)
{
	uint8_t write_data[] = {CMFW_SMBUS_TELEMETRY_READ, 0x3, 0xAA, 0xBB, 0xCC};
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(-1, i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr));
	zassert_equal(err_count_before + 1, get_smbus_error_count());
}

/**
 * @brief A telemetry read returns the requested tag.
 * @details Reads the AICLK tag and checks the response starts with length 7 and a zero second
 * byte. No error is counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_telem_read)
{
	uint8_t write_data[] = {CMFW_SMBUS_TELEMETRY_READ, 0x1U, TAG_AICLK};
	uint8_t read_data[8];
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					read_data, sizeof(read_data)));
	zexpect_equal(7U, read_data[0]);
	zexpect_equal(0U, read_data[1]);
	/* bytes 2-3 are DC. Bytes 4-7 are telem data but currently aren't emulated */
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A telemetry write leaves the control word clear.
 * @details Sends a 33-byte telemetry write and reads back 21 bytes. Checks the first byte is 20
 * and the control word at offset 12 is zero.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_telem_write_no_reset)
{
	uint8_t write_data[35] = {[0] = CMFW_SMBUS_TELEMETRY_WRITE, [1] = 33U};
	uint8_t read_data[21];
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					read_data, sizeof(read_data)));
	zexpect_equal(20U, read_data[0]);

	uint32_t ctl = 0;

	(void)memcpy(&ctl, &read_data[12], sizeof(ctl));
	zexpect_equal(0U, ctl);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

/**
 * @brief A block write followed by a block read returns the data.
 * @details Writes four bytes to command 0xDE and reads a block back. Checks the length byte is 4
 * and no error is counted.
 * @ingroup tests_bh_arc_smbus
 */
ZTEST(smbus_target, test_block_write_block_read_with_pec)
{
	uint8_t write_data[] = {0xDE, 4, 0xde, 0xad, 0xbe, 0xef};
	uint8_t read_data[6];
	uint32_t err_count_before = get_smbus_error_count();

	zassert_equal(0, i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data),
					read_data, sizeof(read_data)));
	zexpect_equal(4, read_data[0]);
	/* Success - error counter should not change */
	zassert_equal(err_count_before, get_smbus_error_count());
}

ZTEST_SUITE(smbus_target, NULL, NULL, NULL, tear_down_tc, NULL);
