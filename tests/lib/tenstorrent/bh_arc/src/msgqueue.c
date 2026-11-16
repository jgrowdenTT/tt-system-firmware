/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_tt_bh.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include <zephyr/device.h>
#include <zephyr/drivers/misc/bh_fwtable.h>

#include <tenstorrent/board_variables.h>
#include <tenstorrent/smc_msg.h>
#include <tenstorrent/msgqueue.h>
#include <tenstorrent/tt_smbus_regs.h>
#include <tenstorrent/bh_arc.h>
#include "asic_state.h"
#include "clock_wave.h"
#include "cm2dm_msg.h"
#include "noc_init.h"
#include "aiclk_ppm.h"

#include "reg_mock.h"
#include "telemetry.h"
#include "voltage.h"

/* Custom fake for ReadReg to simulate timer progression */
#define RESET_UNIT_REFCLK_CNT_LO_REG_ADDR         0x800300E0
#define PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR 0x80020038
#define ARC_NOC0_TLB_BASE_ADDR                    0xC0000000U
#define ARC_NOC1_TLB_BASE_ADDR                    0xE0000000U
#define ARC_NOC_TLB_WINDOW_SPAN                   0x10000000U
static uint32_t timer_counter;
static uint8_t i2c_read_buf_emul[256] = {0};
static uint8_t i2c_read_buf_idx;

static uint8_t i2c_write_buf_emul[256] = {0};
static uint8_t i2c_write_buf_idx;

static uint32_t clock_wave_value;
static uint32_t noc_2_axi_last_write;

union request req = {0};
struct response rsp = {0};

static const struct device *const i2c0_dev = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0));
static const uint8_t tt_i2c_addr = 0xA;

/* Build-time-resolved handle for the bh_fwtable device. */
#define FWTABLE_DEV DEVICE_DT_GET(DT_NODELABEL(fwtable))

/* Runs FIRST in the msgqueue suite. ztest orders tests by SORT_BY_NAME on the
 * symbol z_ztest_unit_test__<suite>__<fn>, so the "0_" prefix sorts this ahead
 * of every other test. The build guarantees flash.bin exists (CMake FATAL_ERRORs
 * when protoc is missing), so a not-ready fwtable device is a real bh_fwtable
 * init/load regression and must FAIL — never skip.
 */
/**
 * @brief The bh_fwtable device is ready before any other test runs.
 * @details Checks that the fwtable device pointer is non-NULL and reports ready, so a failed table
 * load fails here first.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_0_fwtable_device_ready)
{
	const struct device *dev = FWTABLE_DEV;

	zassert_not_null(dev, "fwtable device pointer is NULL");
	zassert_true(device_is_ready(dev),
		     "fwtable device not ready — bh_fwtable init/load failed. "
		     "flash.bin is built by CMake; check the driver's load paths.");
}

/* Helper function to simulate DMC reading posted SMBUS messages */
static cm2dmMessage read_posted_smbus_message(void)
{
	cm2dmMessage msg = {0};
	uint8_t write_data[] = {CMFW_SMBUS_REQ};
	uint8_t read_data[7]; /* 48 bits = 6 bytes for cm2dmMessage */

	/* Read the posted message */
	int ret = i2c_write_read(i2c0_dev, tt_i2c_addr, write_data, sizeof(write_data), read_data,
				 sizeof(read_data));

	if (ret == 0) {
		/* Parse the cm2dmMessage struct */
		msg.msg_id = read_data[1];
		msg.seq_num = read_data[2];
		/* data is uint32_t, so combine bytes 2-5 */
		msg.data = (uint32_t)read_data[3] | ((uint32_t)read_data[4] << 8) |
			   ((uint32_t)read_data[5] << 16) | ((uint32_t)read_data[6] << 24);
	}

	return msg;
}

static inline uint8_t pec_crc_8(uint8_t crc, uint8_t data)
{
	return crc8(&data, 1, 0x7, crc, false);
}

/* Helper function to send ACK for received message */
static void ack_smbus_message(const cm2dmMessage *msg)
{
	cm2dmAck ack;
	uint8_t pec = 0;

	ack.msg_id = msg->msg_id;
	ack.seq_num = msg->seq_num;

	pec = pec_crc_8(pec, (tt_i2c_addr << 1) | I2C_MSG_WRITE);
	pec = pec_crc_8(pec, CMFW_SMBUS_ACK);
	pec = pec_crc_8(pec, ack.msg_id);
	pec = pec_crc_8(pec, ack.seq_num);

	uint8_t write_data[] = {CMFW_SMBUS_ACK, ack.msg_id, ack.seq_num, pec};
	int x = i2c_write(i2c0_dev, write_data, sizeof(write_data), tt_i2c_addr);

	printf("%d", x);
}

/* Helper function to clear all pending SMBUS messages */
static void clear_pending_smbus_messages(void)
{
	cm2dmMessage msg;
	int attempts = 10; /* Prevent infinite loop */

	do {
		msg = read_posted_smbus_message();
		if (msg.msg_id != 0) {
			ack_smbus_message(&msg);
			attempts--;
		}
	} while (msg.msg_id != 0 && attempts > 0);
}

/*
 * These variadic macros can be used to attach a printf-style context
 * message describing a specific sub-case inside a ZTEST, instead of
 * 1 message per ZTEST
 *
 * Uses zexpect_* (soft assertions) so a single sub-case failure doesn't
 * short-circuit the rest of the test
 */
#define push_msg_success(...)                                                                      \
	do {                                                                                       \
		msgqueue_request_push(0, &req);                                                    \
		process_message_queues();                                                          \
		msgqueue_response_pop(0, &rsp);                                                    \
		zexpect_equal(rsp.data[0], 0, ##__VA_ARGS__);                                      \
	} while (0)

#define push_msg_failure(...)                                                                      \
	do {                                                                                       \
		msgqueue_request_push(0, &req);                                                    \
		process_message_queues();                                                          \
		msgqueue_response_pop(0, &rsp);                                                    \
		zexpect_not_equal(rsp.data[0], 0, ##__VA_ARGS__);                                  \
	} while (0)

static uint32_t ReadReg_msgqueue_fake(uint32_t addr)
{
	/* IC_STATUS; Fake out TX_FIFO to say empty and not full Fake out RX_FIFO to say not empty.
	 * This should be replaced by a emulated i2c driver once we use
	 * a real zephyr i2c controller in our app.
	 */
	if (addr == 0x80090070) {
		return 0b1110;
	}

	/* IC_DATA_CMD; Fake out RX data to provide emulated data*/
	if (addr == 0x80090010) {
		return i2c_read_buf_emul[i2c_read_buf_idx++];
	}

	if (addr == RESET_UNIT_REFCLK_CNT_LO_REG_ADDR) {
		return timer_counter++;
	}

	/*BH_PCIE_DWC_PCIE_USP_PF0_MSI_CAP_PCI_MSI_CAP_ID_NEXT_CTRL_REG_REG_ADDR*/
	if (addr == 0xCE000050) {
		return BIT(16) | BIT(20); /*pci_msi_enable | pci_msi_multiple_msg_en == 1*/
	}

	return 0;
}

static void WriteReg_msgqueue_fake(uint32_t addr, uint32_t value)
{
	/* IC_DATA_CMD; Fake out TX data to get test visibility on sent data
	 * Note the truncation; data to I2C is in the LSB
	 * More significant bytes of the value word contain i2c transaction flags
	 */
	if (addr == 0x80090010) {
		i2c_write_buf_emul[i2c_write_buf_idx++] = value;
	}

	if (addr == PLL_CNTL_WRAPPER_CLOCK_WAVE_CNTL_REG_ADDR) {
		clock_wave_value = value;
	}

	if ((addr >= ARC_NOC0_TLB_BASE_ADDR &&
	     addr < ARC_NOC0_TLB_BASE_ADDR + ARC_NOC_TLB_WINDOW_SPAN) ||
	    (addr >= ARC_NOC1_TLB_BASE_ADDR &&
	     addr < ARC_NOC1_TLB_BASE_ADDR + ARC_NOC_TLB_WINDOW_SPAN)) {
		noc_2_axi_last_write = value;
	}
}

static uint8_t msgqueue_handler_73(const union request *req, struct response *rsp)
{
	BUILD_ASSERT(MSG_TYPE_SHIFT % 8 == 0);
	rsp->data[1] = req->data[0];
	return 0;
}

/**
 * @brief A handler registered for a message ID is dispatched.
 * @details Registers a handler for opcode 0x73, sends a request and checks the response holds the
 * value the handler produced.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msgqueue_register_handler)
{
	msgqueue_register_handler(0x73, msgqueue_handler_73);

	req.data[0] = 0x73737373;
	push_msg_success();

	zassert_equal(rsp.data[1], 0x73737373);
}

/**
 * @brief POWER_SETTING flags switch the max AICLK target and the L2CPU clocks.
 * @details Sends POWER_SETTING with max AICLK on, then off, and checks the AICLK target moves
 * between Fmax and Fmin. Checks the emulated L2CPU clocks stay off.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msgqueue_power_settings_cmd)
{
	const struct device *pll4 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pll4));

	/* LSB to MSB:
	 * 0x21: TT_SMC_MSG_POWER_SETTING
	 * 0x04: 4 power flags valid, 0 power settings valid
	 * 0x0003: max_ai_clk on, mrisc power on, tensix power off, l2cpu off
	 */
	req.data[0] = 0x00030421;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmax());

	zexpect_equal(rsp.data[0], 0x0);

	/* Validate that status of emulated L2CPUCLKs are disabled */
	zexpect_true(device_is_ready(pll4));
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0),
		      CLOCK_CONTROL_STATUS_OFF);
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_1),
		      CLOCK_CONTROL_STATUS_OFF);
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_2),
		      CLOCK_CONTROL_STATUS_OFF);
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_3),
		      CLOCK_CONTROL_STATUS_OFF);

	/* LSB to MSB:
	 * 0x21: TT_SMC_MSG_POWER_SETTING
	 * 0x04: 4 power flags valid, 0 power settings valid
	 * 0x0000: max_ai_clk off, mrisc power off, tensix power off, l2cpu off
	 */
	req.data[0] = 0x00000421;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmin());

	/* Validate that status of emulated L2CPUCLKs are disabled */
	zexpect_true(device_is_ready(pll4));
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_0),
		      CLOCK_CONTROL_STATUS_OFF);
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_1),
		      CLOCK_CONTROL_STATUS_OFF);
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_2),
		      CLOCK_CONTROL_STATUS_OFF);
	zexpect_equal(clock_control_get_status(
			      pll4, (clock_control_subsys_t)CLOCK_CONTROL_TT_BH_CLOCK_L2CPUCLK_3),
		      CLOCK_CONTROL_STATUS_OFF);
}

/**
 * @brief GO_BUSY holds AICLK at Fmax across POWER_SETTING changes.
 * @details AICLK stays at Fmax while busy or while POWER_SETTING is on, and drops to Fmin after a
 * final POWER_SETTING off.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msgqueue_power_settings_with_go_busy)
{
	/* LSB to MSB:
	 * 0x21: TT_SMC_MSG_POWER_SETTING
	 * 0x01: 1 power flags valid,  power settings valid
	 * 0x0000: max_ai_clk off or 0x0001: max_ai_clk on
	 */
	static const uint32_t on_power_cmd = 0x00010121;
	static const uint32_t off_power_cmd = 0x00000121;

	req.data[0] = off_power_cmd;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmin());

	/* Go busy should set targ to max */
	req.data[0] = TT_SMC_MSG_AICLK_GO_BUSY;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmax());

	/*
	 * Because we got GO_BUSY, AICLK should remain at FMax after aiclk off POWER_SETTING
	 */
	req.data[0] = off_power_cmd;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmax());

	/*
	 * Send POWER_SETTING with AICLK high
	 */

	req.data[0] = on_power_cmd;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmax());

	/*
	 * Send GO_LONG_IDLE. We should remain at FMax because POWER_SETTING was set high
	 */
	req.data[0] = TT_SMC_MSG_AICLK_GO_LONG_IDLE;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmax());

	/*
	 * Send POWER_SETTING with AICLK low. Now we should go to Fmin
	 */

	req.data[0] = off_power_cmd;
	push_msg_success();

	CalculateTargAiclk();
	zexpect_equal(GetAiclkTarg(), GetAiclkFmin());
}

/**
 * @brief SET_VOLTAGE writes the regulator's VOUT_COMMAND over I2C.
 * @details Sends a set-voltage request for 800 mV and checks the I2C write carries VOUT_COMMAND
 * and the value doubled.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_set_voltage)
{
	req.data[0] = TT_SMC_MSG_SET_VOLTAGE;
	req.data[1] = 0x64; /* regulator id */
	req.data[2] = 800;  /* voltage in mV */
	push_msg_success();

	zexpect_equal(i2c_write_buf_emul[0], 33); /*VOUT_COMMAND*/

	uint32_t received_voltage;

	memcpy(&received_voltage, &i2c_write_buf_emul[1], sizeof(received_voltage));

	zexpect_equal(received_voltage, 800 * 2);
}

/**
 * @brief GET_VOLTAGE reads the regulator voltage over I2C.
 * @details Simulates an I2C read of 950 and checks the response reports that value halved.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_get_voltage)
{
	/*Setup the simulated voltage for the i2c read*/
	uint32_t simulated_voltage_mv = 950;

	memcpy(i2c_read_buf_emul, &simulated_voltage_mv, sizeof(simulated_voltage_mv));

	req.data[0] = TT_SMC_MSG_GET_VOLTAGE;
	req.data[1] = 0x64; /* regulator id */
	push_msg_success();

	zexpect_equal(rsp.data[1], simulated_voltage_mv / 2);
}

/**
 * @brief SWITCH_VOUT_CONTROL enables VOUT control on a regulator.
 * @details Checks the I2C traffic reads back OPERATION, then writes enable with the transition and
 * command-source bits set.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_switch_vout_control)
{
	req.data[0] = TT_SMC_MSG_SWITCH_VOUT_CONTROL;
	req.data[1] = 0x01; /* regulator id */
	req.data[2] = 1;    /* enable */
	push_msg_success();

	zexpect_equal(i2c_write_buf_emul[0], 1); /*OPERATION command, for readback*/

	zexpect_equal(i2c_write_buf_emul[2], 1);    /*OPERATION command, writ*/
	zexpect_equal(i2c_write_buf_emul[3], 0x12); /* transition_control and command_source high*/
}

/**
 * @brief SWITCH_CLK_SCHEME selects the clock wave or zero-skew scheme.
 * @details Switches to the clock wave scheme, then to zero skew, and checks the clock wave value
 * changes from 2 to 1.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_switch_clk_scheme)
{
	/* Reset timer counter and set up the fake */
	timer_counter = 0;

	req.data[0] = TT_SMC_MSG_SWITCH_CLK_SCHEME;
	req.data[1] = TT_CLK_SCHEME_CLOCK_WAVE;
	push_msg_success();

	zassert_equal(clock_wave_value, 2U);

	req.data[1] = TT_CLK_SCHEME_ZERO_SKEW;
	push_msg_success();

	zassert_equal(clock_wave_value, 1U);
}

/**
 * @brief DEBUG_NOC_TRANSLATION rejects an out-of-range GDDR index.
 * @details Names GDDR 8 as bad and checks the reply is EINVAL truncated to a byte. Resends with no
 * bad GDDR and checks success.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_debug_noc_translation)
{
	req.data[0] = TT_SMC_MSG_DEBUG_NOC_TRANSLATION | (BIT(0) << 8U) /* Enable translation*/
		      | (BIT(1) << 8U)                                  /* PCIE Instance  = 1*/
		      | (BIT(2) << 8U)                                  /*PCIE instance override*/
		      | ((BIT(0) | BIT(3)) << 16U) /*Bad tensix columns 0 and 3*/
		;
	req.data[1] = 8U /* Bad GDDR 8 */ | ((BIT(1) | BIT(3)) << 8U) /*skip eth 1 and 3*/;
	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	zassert_equal(rsp.data[0], 234); /* uin8_t EINVAL -> GDDR out of range*/

	req.data[1] = NO_BAD_GDDR | ((BIT(1) | BIT(3)) << 8U) /*skip eth 1 and 3*/;

	push_msg_success();
}

static void test_setup(void *ctx)
{
	(void)ctx;
	ReadReg_fake.custom_fake = ReadReg_msgqueue_fake;
	WriteReg_fake.custom_fake = WriteReg_msgqueue_fake;
	timer_counter = 0U;
	i2c_read_buf_idx = 0U;
	i2c_write_buf_idx = 0U;
	clock_wave_value = 0U;
	memset(i2c_read_buf_emul, 0, sizeof(i2c_read_buf_emul));
	memset(i2c_write_buf_emul, 0, sizeof(i2c_write_buf_emul));
}

/**
 * @brief SEND_PCIE_MSI writes the MSI number to the NOC-to-AXI window.
 * @details Sends MSI numbers 0 and 1 for PCIe instance 1 and checks the last NOC-to-AXI write
 * equals each number.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_send_pcie_msi)
{
	noc_2_axi_last_write = 0xffffffffU;
	req.data[0] = TT_SMC_MSG_SEND_PCIE_MSI | (BIT(0) << 8U) /*PCIe instance 1*/;
	req.data[1] = 0x00; /* MSI number */
	push_msg_success();

	zexpect_equal(noc_2_axi_last_write, 0);

	req.data[1] = 0x01; /* MSI number */

	push_msg_success();
	zexpect_equal(noc_2_axi_last_write, 1);
}

/**
 * @brief I2C_MESSAGE rejects an invalid I2C line ID.
 * @details Sends a write to target 0x50 on line 5 and checks the reply is an error of 1.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_i2c_message_bad_line_id)
{
	union request req = {0};
	struct response rsp = {0};

	/* Reset timer counter and set up the fake */
	timer_counter = 0;
	ReadReg_fake.custom_fake = ReadReg_msgqueue_fake;

	req.data[0] = BIT(24U)                     /*Write Operation*/
		      | FIELD_PREP(0xFF0000, 0x50) /* target address */
		      | FIELD_PREP(0xFF00, 0x5U)   /*Invalid Line Id*/
		      | TT_SMC_MSG_I2C_MESSAGE;
	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	zassert_equal(rsp.data[0], 1);
}

/**
 * @brief I2C_MESSAGE writes and reads four bytes on line 1.
 * @details Writes four bytes to target 0x50 and checks they appear on the emulated bus in order.
 * Checks the read-back is assembled little-endian.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_i2c_message)
{
	i2c_read_buf_emul[0] = 0x1U;
	i2c_read_buf_emul[1] = 0x2U;
	i2c_read_buf_emul[2] = 0x3U;
	i2c_read_buf_emul[3] = 0x4U;

	/* Reset timer counter and set up the fake */
	timer_counter = 0;
	ReadReg_fake.custom_fake = ReadReg_msgqueue_fake;

	req.data[0] = FIELD_PREP(0xFF000000, 0x4)  /*Write 4 bytes*/
		      | FIELD_PREP(0xFF0000, 0x50) /* target address */
		      | FIELD_PREP(0xFF00, 0x1U)   /*Line Id*/
		      | TT_SMC_MSG_I2C_MESSAGE;

	req.data[1] = 4Ul;         /*Read 4 bytes*/
	req.data[2] = 0xDDCCBBAAU; /*Write data*/

	push_msg_success();
	zexpect_equal(rsp.data[1], 0x04030201);

	zexpect_equal(i2c_write_buf_emul[0], 0xaa);
	zexpect_equal(i2c_write_buf_emul[1], 0xbb);
	zexpect_equal(i2c_write_buf_emul[2], 0xcc);
	zexpect_equal(i2c_write_buf_emul[3], 0xdd);
}

/**
 * @brief BLINKY posts an LED blink message to the DMC over SMBus.
 * @details Sends a blink request with value 1, then reads the posted SMBus message as the DMC and
 * checks it is an LED blink carrying 1. Then ACKs it.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_blink_led)
{
	clear_pending_smbus_messages();

	req.data[0] = TT_SMC_MSG_BLINKY;
	req.data[1] = 0x1;

	push_msg_success();
	zassert_equal(rsp.data[1], 0);

	/* Now act as DMC and read the posted SMBUS message */
	cm2dmMessage posted_msg = read_posted_smbus_message();

	/* Verify the posted message contains the correct LED blink data */
	zassert_equal(posted_msg.msg_id, kCm2DmMsgIdLedBlink, "Posted message should be LedBlink");
	zassert_equal(posted_msg.data, 1, "Posted message data should contain blink value 1");

	/* Send ACK for the message */
	ack_smbus_message(&posted_msg);
}

/**
 * @brief TEST returns its argument plus one.
 * @details Sends 42 and checks the response is 43.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_test)
{
	req.data[0] = TT_SMC_MSG_TEST;
	req.data[1] = 42; /* test_value to be incremented */

	push_msg_success();
	zexpect_equal(rsp.data[1], 43); /* test_value + 1 */
}

/**
 * @brief ASIC_STATE3 and ASIC_STATE0 switch the ASIC state.
 * @details Requests state 3 and checks the ASIC is in A3. Requests state 0 and checks it returns
 * to A0 with success.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_asic_state)
{
	req.data[0] = TT_SMC_MSG_ASIC_STATE3;
	push_msg_success();
	zexpect_equal(get_asic_state(), A3State);

	/* Test ASIC_STATE0 to return to state 0 */
	req.data[0] = TT_SMC_MSG_ASIC_STATE0;
	push_msg_success();

	zexpect_equal(rsp.data[0], 0);
	zexpect_equal(get_asic_state(), A0State);
}

/**
 * @brief READ_EEPROM fails when no flash device is present.
 * @details Sends a 64-byte read at SPI address 0x1000 with no flash available and checks the reply
 * is 1.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_read_eeprom_no_flash)
{
	req = (union request){0};
	rsp = (struct response){0};

	req.eeprom.command_code = TT_SMC_MSG_READ_EEPROM;
	req.eeprom.buffer_mem_type = 0;
	req.eeprom.spi_address = 0x1000;
	req.eeprom.num_bytes = 64;

	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	/* Flash device is not available in test, handler returns 1 */
	zassert_equal(rsp.data[0], 1);
}

/**
 * @brief FORCE_VDD accepts voltages inside the envelope and 0, and rejects others.
 * @details Derives the voltage envelope from the loaded fw_table. Checks an in-range value and 0
 * are accepted, and values just above and below the envelope are rejected.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_force_vdd)
{
	/* Exercises TT_SMC_MSG_FORCE_VDD. Handler under test: ForceVddHandler
	 * (lib/.../voltage.c). The handler's gate is:
	 *   if ((v > vdd_max || v < vdd_min) && v != 0) return 1;   // reject
	 *   else                                        return 0;   // accept
	 * All test inputs are derived from the loaded fw_table so this test is
	 * board-portable.
	 */
	(void)InitVoltagePPM();

	const uint32_t vdd_min = voltage_arbiter.vdd_min;
	const uint32_t vdd_max = voltage_arbiter.vdd_max;

	zassert_true(vdd_min > 0 && vdd_max > vdd_min,
		     "Invalid voltage envelope from fw_table: vdd_min=%u, vdd_max=%u", vdd_min,
		     vdd_max);

	const uint32_t valid = (vdd_min + vdd_max) / 2U; /* in range, nonzero */
	const uint32_t too_high = vdd_max + 1U;          /* out of range, high */
	const uint32_t too_low = vdd_min - 1U;           /* out of range, low (vdd_min ≥ 650) */

	/* Case 1: in-range — accept */
	req = (union request){0};
	rsp = (struct response){0};
	req.force_vdd.command_code = TT_SMC_MSG_FORCE_VDD;
	req.force_vdd.forced_voltage = valid;
	push_msg_success("in-range %u (envelope [%u, %u])", valid, vdd_min, vdd_max);

	/* Case 2: release sentinel 0 — accept */
	req = (union request){0};
	rsp = (struct response){0};
	req.force_vdd.command_code = TT_SMC_MSG_FORCE_VDD;
	req.force_vdd.forced_voltage = 0;
	push_msg_success("forced_voltage=0 (release sentinel)");

	/* Case 3: above vdd_max — reject */
	req = (union request){0};
	rsp = (struct response){0};
	req.force_vdd.command_code = TT_SMC_MSG_FORCE_VDD;
	req.force_vdd.forced_voltage = too_high;
	push_msg_failure("above-range %u (envelope [%u, %u])", too_high, vdd_min, vdd_max);

	/* Case 4: below vdd_min — reject */
	req = (union request){0};
	rsp = (struct response){0};
	req.force_vdd.command_code = TT_SMC_MSG_FORCE_VDD;
	req.force_vdd.forced_voltage = too_low;
	push_msg_failure("below-range %u (envelope [%u, %u])", too_low, vdd_min, vdd_max);
}

/**
 * @brief A valid chip-to-host PCIe DMA transfer is accepted.
 * @details Sends a 4 KiB chip-to-host transfer with valid addresses and checks it succeeds.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_pcie_dma_chip_to_host)
{
	req.pcie_dma_transfer.command_code = TT_SMC_MSG_PCIE_DMA_CHIP_TO_HOST_TRANSFER;
	req.pcie_dma_transfer.completion_data = 0xAB;
	req.pcie_dma_transfer.transfer_size_bytes = 4096;
	req.pcie_dma_transfer.chip_addr = 0x100000;
	req.pcie_dma_transfer.host_addr = 0x200000;
	req.pcie_dma_transfer.msi_completion_addr = 0x300000;

	push_msg_success();

	zassert_equal(rsp.data[0], 0, "Chip-to-host DMA transfer should succeed");
}

/**
 * @brief A valid host-to-chip PCIe DMA transfer is accepted.
 * @details Sends a 2 KiB host-to-chip transfer with valid addresses and checks it succeeds.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_pcie_dma_host_to_chip)
{
	req.pcie_dma_transfer.command_code = TT_SMC_MSG_PCIE_DMA_HOST_TO_CHIP_TRANSFER;
	req.pcie_dma_transfer.completion_data = 0xCD;
	req.pcie_dma_transfer.transfer_size_bytes = 2048;
	req.pcie_dma_transfer.chip_addr = 0x400000;
	req.pcie_dma_transfer.host_addr = 0x500000;
	req.pcie_dma_transfer.msi_completion_addr = 0x600000;

	push_msg_success();

	zassert_equal(rsp.data[0], 0, "Host-to-chip DMA transfer should succeed");
}

/**
 * @brief TRIGGER_RESET rejects an unknown reset level.
 * @details Sends reset level 5 and checks the reply returns that level as the error.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_trigger_reset_invalid)
{
	/* Invalid reset level should be rejected */
	req.trigger_reset.command_code = TT_SMC_MSG_TRIGGER_RESET;
	req.trigger_reset.reset_level = 5;

	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	zassert_equal(rsp.data[0], 5, "Invalid level should return the level as error");
}

/* Micron MT25QU512ABB, the part FLASH_UNLOCK exempts from the JEDEC ID check */
#define FLASH_JEDEC_ID_MT25QU512ABB 0x20BB20
/* GigaDevice GD25LF512MF, standing in for any second-source part */
#define FLASH_JEDEC_ID_GD25LF512MF  0xC8631A

/* A write that reaches the flash driver fails with 1 (flash not ready) in
 * this environment; one refused by the lock fails with 2.
 */
#define FLASH_WRITE_NOT_READY 1
#define FLASH_WRITE_LOCKED    2

/* Put the flash back in its boot state: locked, with no board variables
 * accepted. The flash state is static in spi_eeprom.c and outlives a single
 * test, so each test establishes it rather than inheriting it.
 */
static void flash_lock_reset(void)
{
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.flash_lock.command_code = TT_SMC_MSG_FLASH_LOCK;

	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);
	zassert_equal(rsp.data[0], 0, "Flash lock should succeed");
}

static uint32_t send_flash_unlock(uint8_t num_variable_words, uint32_t verified)
{
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.flash_unlock.command_code = TT_SMC_MSG_FLASH_UNLOCK;
	req.flash_unlock.num_variable_words = num_variable_words;
	req.flash_unlock.verified_variables[0] = verified;

	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	return rsp.data[0];
}

static uint32_t try_eeprom_write(void)
{
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.eeprom.command_code = TT_SMC_MSG_WRITE_EEPROM;
	req.eeprom.spi_address = 0x1000;
	req.eeprom.num_bytes = 4;
	req.eeprom.csm_addr = 0x12345678; /* Valid CSM address range */
	req.eeprom.buffer_mem_type = 0;

	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	return rsp.data[0];
}

/**
 * @brief An MT25 flash unlocks without any board variables.
 * @details Unlocks an MT25QU512ABB flash with no variables and checks it succeeds with none
 * required. A following write fails as not ready, not locked.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_flash_unlock)
{
	/* An MT25 board requires nothing of the host, so a request that
	 * predates board variables still unlocks it.
	 */
	UpdateTelemetryFlashJedecId(FLASH_JEDEC_ID_MT25QU512ABB);
	flash_lock_reset();

	zassert_equal(send_flash_unlock(0, 0), 0, "Flash unlock should succeed");
	zassert_equal(rsp.data[1], 0, "No board variables should be required");
	zassert_equal(try_eeprom_write(), FLASH_WRITE_NOT_READY,
		      "Write should fail due to flash not ready, not locked");
}

/**
 * @brief Unlock is refused without the JEDEC ID variable on a non-MT25 part.
 * @details Unlocks a GD25LF512MF with no variables and checks the reply names the JEDEC ID
 * variable. Checks writes stay locked.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_flash_unlock_missing_variables)
{
	/* Any other part needs the host to have checked the JEDEC ID */
	UpdateTelemetryFlashJedecId(FLASH_JEDEC_ID_GD25LF512MF);
	flash_lock_reset();

	zassert_equal(send_flash_unlock(0, 0), 1,
		      "Flash unlock should be refused without the JEDEC ID variable");
	zassert_equal(rsp.data[1], BIT(BOARD_VAR_SPI_JEDEC_ID),
		      "The reply should name the required board variable");
	zassert_equal(try_eeprom_write(), FLASH_WRITE_LOCKED,
		      "Write should fail due to flash locked");
}

/**
 * @brief Unlock succeeds once the JEDEC ID variable is verified.
 * @details Checks a verified unlock succeeds and a bare unlock rides on it. Checks a lock
 * withdraws the variables so the next unlock is refused.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_flash_unlock_verified_variables)
{
	UpdateTelemetryFlashJedecId(FLASH_JEDEC_ID_GD25LF512MF);
	flash_lock_reset();

	zassert_equal(send_flash_unlock(1, BIT(BOARD_VAR_SPI_JEDEC_ID)), 0,
		      "Flash unlock should succeed once the JEDEC ID is verified");
	zassert_equal(rsp.data[1], BIT(BOARD_VAR_SPI_JEDEC_ID),
		      "The requirement should be reported on success too");
	zassert_equal(try_eeprom_write(), FLASH_WRITE_NOT_READY,
		      "Write should fail due to flash not ready, not locked");

	/* The SPI write path issues its own bare unlock per transfer, which
	 * rides on the variables already accepted.
	 */
	zassert_equal(send_flash_unlock(0, 0), 0,
		      "A bare unlock should ride on the accepted variables");

	/* Locking withdraws them again */
	flash_lock_reset();
	zassert_equal(send_flash_unlock(0, 0), 1, "Locking should withdraw the accepted variables");
}

/**
 * @brief Variable words past the stated word count are not read.
 * @details Sends the JEDEC ID bit with a word count of zero and checks the unlock is refused.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_flash_unlock_word_count)
{
	/* Variable words past num_variable_words are not read */
	UpdateTelemetryFlashJedecId(FLASH_JEDEC_ID_GD25LF512MF);
	flash_lock_reset();

	zassert_equal(send_flash_unlock(0, BIT(BOARD_VAR_SPI_JEDEC_ID)), 1,
		      "A word count of zero should leave the variables unread");
}

/**
 * @brief A tt-flash unlock and luwen-style writes stay open until locked.
 * @details Runs twice an unlock, bare unlock, two write chunks and a lock. Checks chunks are not
 * locked out until the lock, and are refused after it.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_flash_unlock_write_sequence)
{
	/* The message sequence a real flash produces. tt-flash unlocks with
	 * the variables it verified, then hands off to luwen, whose spi_write
	 * issues its own bare unlock, writes the data a chunk at a time, and
	 * locks again. A flash is many such calls, so run it twice.
	 */
	UpdateTelemetryFlashJedecId(FLASH_JEDEC_ID_GD25LF512MF);
	flash_lock_reset();

	for (int cycle = 0; cycle < 2; cycle++) {
		zassert_equal(send_flash_unlock(1, BIT(BOARD_VAR_SPI_JEDEC_ID)), 0,
			      "tt-flash unlock should succeed in cycle %d", cycle);

		zassert_equal(send_flash_unlock(0, 0), 0,
			      "luwen bare unlock should succeed in cycle %d", cycle);

		/* The bare unlock must leave the flash open, not merely report
		 * success: luwen writes every chunk after it.
		 */
		zassert_equal(try_eeprom_write(), FLASH_WRITE_NOT_READY,
			      "First chunk should not be locked out in cycle %d", cycle);
		zassert_equal(try_eeprom_write(), FLASH_WRITE_NOT_READY,
			      "Second chunk should not be locked out in cycle %d", cycle);

		flash_lock_reset();

		zassert_equal(try_eeprom_write(), FLASH_WRITE_LOCKED,
			      "Write after the lock should be refused in cycle %d", cycle);
	}
}

/**
 * @brief Locking the flash refuses later EEPROM writes.
 * @details Unlocks an MT25QU512ABB flash, resets the lock and checks a following write fails as
 * locked.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_flash_lock)
{
	UpdateTelemetryFlashJedecId(FLASH_JEDEC_ID_MT25QU512ABB);

	/* First unlock flash */
	zassert_equal(send_flash_unlock(0, 0), 0, "Flash unlock should succeed");

	/* Then lock it */
	flash_lock_reset();

	zassert_equal(try_eeprom_write(), FLASH_WRITE_LOCKED,
		      "Write should fail due to flash locked");
}

/**
 * @brief CONFIRM_FLASHED_SPI succeeds and echoes the challenge.
 * @details Sends a challenge word and checks the reply is success carrying the same word.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_confirm_flashed_spi)
{
	uint32_t challenge_data = 0xDEADBEEF;

	/* Test SPI flash confirmation with challenge data */
	req.confirm_flashed_spi.command_code = TT_SMC_MSG_CONFIRM_FLASHED_SPI;
	req.data[1] = challenge_data;

	push_msg_success();

	/* Should succeed */
	zassert_equal(rsp.data[0], 0, "Confirm flash should succeed");
	/* Response should echo the challenge data */
	zassert_equal(rsp.data[1], challenge_data, "Challenge data should be echoed back");
}

/**
 * @brief SET_LAST_SERIAL sets the serial reported by TEST.
 * @details Sets a serial and checks TEST reports serial plus one. Repeats with a second serial to
 * confirm the update takes effect.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_set_last_serial)
{
	uint32_t test_serial_number = 0x12345678;

	/* Test setting a specific serial number */
	req.set_last_serial.command_code = TT_SMC_MSG_SET_LAST_SERIAL;
	req.set_last_serial.serial_number = test_serial_number;

	push_msg_success();

	/* Verify the operation succeeded */
	zassert_equal(rsp.data[0], 0, "Set last serial should succeed");

	/* Verify the serial number was set by using a test message */
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.test.command_code = TT_SMC_MSG_TEST;
	req.test.test_value = 42; /* arbitrary test value */

	push_msg_success();

	/* Response data[2] contains last_serial + 1 */
	zassert_equal(rsp.data[2], test_serial_number + 1,
		      "Serial number should be set to the specified value");
	/* Verify test value was also processed correctly */
	zassert_equal(rsp.data[1], 43, "Test value should be incremented");

	/* Test setting a different serial number */
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	uint32_t new_serial_number = 0xABCDEF00;

	req.set_last_serial.command_code = TT_SMC_MSG_SET_LAST_SERIAL;
	req.set_last_serial.serial_number = new_serial_number;

	push_msg_success();

	/* Verify the new serial number was set */
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.test.command_code = TT_SMC_MSG_TEST;
	req.test.test_value = 99; /* arbitrary test value */

	push_msg_success();

	/* Verify new serial number is active */
	zassert_equal(rsp.data[2], new_serial_number + 1,
		      "Serial number should be updated to the new value");
}

/**
 * @brief SET_WDT_TIMEOUT posts timeout updates and rejects tiny timeouts.
 * @details Checks 5000 ms and 0 (disable) each post an auto-reset timeout update with that value.
 * A 1 ms timeout fails with ENOTSUP and posts nothing.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_set_wdt_timeout)
{
	/* Clear any pending messages from previous tests */
	clear_pending_smbus_messages();

	/* Test setting watchdog timeout with valid value */
	req.set_wdt_timeout.command_code = TT_SMC_MSG_SET_WDT_TIMEOUT;
	req.set_wdt_timeout.timeout_ms = 5000; /* 5 seconds - should be valid */

	push_msg_success();

	/* Now act as DMC and read the posted SMBUS message */
	cm2dmMessage posted_msg = read_posted_smbus_message();

	/* Verify the posted message contains the correct timeout data */
	zassert_equal(posted_msg.msg_id, kCm2DmMsgIdAutoResetTimeoutUpdate,
		      "Posted message should be AutoResetTimeoutUpdate");
	zassert_equal(posted_msg.data, 5000,
		      "Posted message data should contain timeout value 5000ms");

	/* Send ACK for the message */
	ack_smbus_message(&posted_msg);

	/* Test disabling watchdog (timeout = 0) */
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.set_wdt_timeout.command_code = TT_SMC_MSG_SET_WDT_TIMEOUT;
	req.set_wdt_timeout.timeout_ms = 0; /* Disable watchdog */

	push_msg_success();

	/* Read the posted disable message */
	cm2dmMessage disable_msg = read_posted_smbus_message();

	/* Verify the disable message */
	zassert_equal(disable_msg.msg_id, kCm2DmMsgIdAutoResetTimeoutUpdate,
		      "Posted message should be AutoResetTimeoutUpdate");
	zassert_equal(disable_msg.data, 0,
		      "Posted message data should contain timeout value 0 (disabled)");

	/* Send ACK for the disable message */
	ack_smbus_message(&disable_msg);

	/* Test setting timeout too small (should fail with ENOTSUP) */
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.set_wdt_timeout.command_code = TT_SMC_MSG_SET_WDT_TIMEOUT;
	req.set_wdt_timeout.timeout_ms = 1; /* Very small timeout - should be rejected */

	msgqueue_request_push(0, &req);
	process_message_queues();
	msgqueue_response_pop(0, &rsp);

	/* Should fail with ENOTSUP (too small) */
	zassert_equal(rsp.data[0], ENOTSUP, "Small watchdog timeout should fail with ENOTSUP");

	/* Verify no message was posted for invalid timeout */
	cm2dmMessage invalid_msg = read_posted_smbus_message();

	zassert_equal(invalid_msg.msg_id, 0, "No message should be posted for invalid timeout");
}

/**
 * @brief PING_DM posts a ping to the DMC in read and write forms.
 * @details Sends a ping with legacy_ping false, then true. Checks each posts a ping message with
 * the matching data flag and the reply is success.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_ping_dm)
{
	/* Clear any pending messages from previous tests */
	clear_pending_smbus_messages();

	/* Test ping DMC with legacy_ping = false (read transaction) */
	req.dmc_ping.command_code = TT_SMC_MSG_PING_DM;
	req.dmc_ping.legacy_ping = false;

	push_msg_success();

	/* Now act as DMC and read the posted SMBUS message */
	cm2dmMessage posted_msg = read_posted_smbus_message();

	/* Verify the posted message contains the correct ping data */
	zassert_equal(posted_msg.msg_id, 2, "Posted message should be Ping (kCm2DmMsgIdPing = 2)");
	zassert_equal(posted_msg.data, 0, "Posted message data should contain legacy_ping = false");

	/* Send ACK for the message */
	ack_smbus_message(&posted_msg);
	/* Note the DM PING SMBUS message is not simulated in this test */

	/* Test ping DMC with legacy_ping = true (write transaction) */
	clear_pending_smbus_messages();
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));

	req.dmc_ping.command_code = TT_SMC_MSG_PING_DM;
	req.dmc_ping.legacy_ping = true;

	push_msg_success();

	/* Verify the command was processed */
	zassert_equal(rsp.data[0], 0, "Ping DM command should be processed successfully");

	/* Read the posted SMBUS message for legacy ping */
	cm2dmMessage legacy_msg = read_posted_smbus_message();

	/* Verify the legacy ping message */
	zassert_equal(legacy_msg.msg_id, 2, "Posted message should be Ping (kCm2DmMsgIdPing = 2)");
	zassert_equal(legacy_msg.data, 1, "Posted message data should contain legacy_ping = true");

	/* Send ACK for the legacy message */
	ack_smbus_message(&legacy_msg);
	/* Note the DM PING SMBUS message is not simulated in this test */
}

/* Send TT_SUB_MSG_SET_TELEMETRY_UPDATE_INTERVAL with the given payload. */
static void push_set_telem_interval(uint32_t interval_ms)
{
	memset(&req, 0, sizeof(req));
	memset(&rsp, 0, sizeof(rsp));
	req.characterisation_msg.command_code = TT_SMC_MSG_CHARACTERISATION;
	req.characterisation_msg.submsg_ID = TT_SUB_MSG_SET_TELEMETRY_UPDATE_INTERVAL;
	req.characterisation_msg.submsg_data.telemetry_interval.interval_ms = interval_ms;
}

/* The interval the firmware is running at, as published to the host. */
static uint32_t get_telem_interval(void)
{
	return GetTelemetryTag(TAG_UPDATE_TELEM_SPEED);
}

/**
 * @brief A valid telemetry interval is accepted and applied.
 * @details Sets 50 ms, the minimum, then 0 to restore the default. Checks the running interval and
 * the reported TAG_UPDATE_TELEM_SPEED match each time.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_set_telemetry_interval)
{
	/* A value inside the accepted range takes effect and is reported back. */
	push_set_telem_interval(50);
	push_msg_success("Valid telemetry interval should be accepted");
	zassert_equal(get_telem_interval(), 50,
		      "TAG_UPDATE_TELEM_SPEED should report the new interval");

	/* The minimum is inclusive. There is no upper bound. */
	push_set_telem_interval(TELEM_UPDATE_INTERVAL_MIN_MS);
	push_msg_success("Minimum interval should be accepted");
	zassert_equal(get_telem_interval(), TELEM_UPDATE_INTERVAL_MIN_MS);

	/* 0 restores the compiled-in default. */
	push_set_telem_interval(0);
	push_msg_success("Restore default should be accepted");
	zassert_equal(get_telem_interval(), TELEM_UPDATE_INTERVAL_DEFAULT_MS,
		      "Interval should return to the default");
}

/* Rejection is only reachable when the minimum is above 1: at a minimum of 1 the only smaller
 * value is 0, which is the restore-default request rather than an out-of-range one.
 */
#if TELEM_UPDATE_INTERVAL_MIN_MS > 1
/**
 * @brief An interval below the minimum is rejected.
 * @details Sets 200 ms, then one below the minimum. Checks the second request fails and the
 * interval stays at 200 ms.
 * @ingroup tests_bh_arc_msgqueue
 */
ZTEST(msgqueue, test_msg_type_set_telemetry_interval_invalid)
{
	/* Establish a known non-default interval to detect any clobbering below. */
	push_set_telem_interval(200);
	push_msg_success();
	zassert_equal(get_telem_interval(), 200);

	push_set_telem_interval(TELEM_UPDATE_INTERVAL_MIN_MS - 1);
	push_msg_failure("Interval below the minimum should be rejected");
	zassert_equal(get_telem_interval(), 200,
		      "A rejected interval must not change the running interval");

	/* Leave the default in place for any test that runs after this one. */
	push_set_telem_interval(0);
	push_msg_success();
}
#endif

ZTEST_SUITE(msgqueue, NULL, NULL, test_setup, NULL, NULL);
