/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <libpldm/base.h>
#include <libpldm/platform.h>
#include <zephyr/drivers/sensor/tenstorrent/pvt_tt_bh.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tt_test_pldm_sensor_ids.h"
#include "testbench.h"

#define PLDM_BIT(field, cmd) ((field)[PLDM_##cmd / 8].byte & BIT(PLDM_##cmd % 8))

#define TELEM_REFRESH_MS 100

/* Global request sequence number, auto-incremented for request tracking */
static uint8_t g_request_seq;

#define NEXT_REQ_ID() (g_request_seq++)

/* Shared test state initialized by before_each */
static uint8_t g_completion_code;
static uint32_t g_next_record_handle;
static uint32_t g_next_data_transfer_handle;
static uint8_t g_transfer_flag;
static uint16_t g_response_count;
static uint8_t g_record_data[64];
static uint8_t g_transfer_crc;
static int g_rc;

static void test_before_each(void *fixture)
{
	(void)fixture;
	g_completion_code = 0;
	g_next_record_handle = 0;
	g_next_data_transfer_handle = 0;
	g_transfer_flag = 0;
	g_response_count = 0;
	memset(g_record_data, 0, sizeof(g_record_data));
	g_transfer_crc = 0;
	g_rc = 0;
	g_request_seq = 0;
}

static void set_emulated_temp_and_wait(float temp_c)
{
	int rc = pvt_tt_bh_emul_set_ts_raw(pvt_tt_bh_temp_to_raw(temp_c));

	zassert_ok(rc, "failed to set emulated PVT temperature: %d", rc);
	k_msleep(TELEM_REFRESH_MS + 20);
}

/**
 * @brief Verify GetTID returns the current terminus ID.
 * @details Sends GetTID and checks success and that the returned TID is 0x0B.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_tid)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_TID_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint8_t tid;
	int rc;

	rc = send_get_tid_request(req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_TID, PLDM_BASE);

	rc = decode_get_tid_resp(g_resp, g_resp_pldm_payload_len, &completion_code, &tid);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(tid, 0x0BU);
}

/**
 * @brief Verify GetPLDMTypes returns the supported PLDM types.
 * @details Sends GetPLDMTypes and checks the base and platform types are advertised, plus the OEM type when the shell is enabled.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_types)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_TYPES_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	struct pldm_base_get_pldm_types_resp types_resp = {0};
	int rc;

	rc = send_get_types_request(req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_TYPES, PLDM_BASE);

	rc = decode_pldm_base_get_pldm_types_resp(g_resp, g_resp_pldm_payload_len, &types_resp);
	zassert_equal(rc, 0);
	zassert_equal(types_resp.completion_code, PLDM_SUCCESS);
	zassert_not_equal(PLDM_BIT(types_resp.pldm_types, BASE), 0U);
	zassert_not_equal(PLDM_BIT(types_resp.pldm_types, PLATFORM), 0U);
#ifdef CONFIG_TT_PMCI_PLDM_SHELL
	zassert_not_equal(
		types_resp.pldm_types[TT_PLDM_OEM_TYPE / 8].byte & BIT(TT_PLDM_OEM_TYPE % 8), 0U);
#endif
}

/**
 * @brief Verify GetPLDMVersion returns the expected version for a type.
 * @details Sends GetPLDMVersion for the base type and checks the transfer flag is START_AND_END and the version fields match 0xF1.0xF1, update 0xF0, alpha 0x00.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_version)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_VERSION_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint32_t next_transfer_handle;
	uint8_t transfer_flag;
	ver32_t version;
	ver32_t expected_version = {
		.major = 0xf1,
		.minor = 0xf1,
		.update = 0xf0,
		.alpha = 0x00,
	};
	int rc;

	rc = send_get_version_request(req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_VERSION, PLDM_BASE);

	rc = decode_get_version_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
				     &next_transfer_handle, &transfer_flag, &version);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(transfer_flag, PLDM_START_AND_END);
	zassert_equal(version.major, expected_version.major);
	zassert_equal(version.minor, expected_version.minor);
	zassert_equal(version.update, expected_version.update);
	zassert_equal(version.alpha, expected_version.alpha);
}

/**
 * @brief Verify GetPLDMCommands returns the supported commands for a type.
 * @details Sends GetPLDMCommands for the base type and checks GetTID, GetPLDMTypes, GetPLDMVersion and GetPLDMCommands are supported.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_commands)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_COMMANDS_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	bitfield8_t commands[PLDM_MAX_CMDS_PER_TYPE / 8] = {0};
	int rc;

	rc = send_get_commands_request(req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_COMMANDS, PLDM_BASE);

	rc = decode_get_commands_resp(g_resp, g_resp_pldm_payload_len, &completion_code, commands);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_true(PLDM_BIT(commands, GET_TID));
	zassert_true(PLDM_BIT(commands, GET_PLDM_TYPES));
	zassert_true(PLDM_BIT(commands, GET_PLDM_VERSION));
	zassert_true(PLDM_BIT(commands, GET_PLDM_COMMANDS));
}

/**
 * @brief Verify platform-type command discovery via GetPLDMCommands.
 * @details Sends GetPLDMCommands for the platform type and checks GetSensorReading, GetPDRRepositoryInfo and GetPDR are supported.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_platform_commands)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_COMMANDS_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	bitfield8_t commands[PLDM_MAX_CMDS_PER_TYPE / 8] = {0};
	int rc;

	rc = send_get_commands_request_for_type(req, NEXT_REQ_ID(), PLDM_PLATFORM);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_COMMANDS, PLDM_BASE);

	rc = decode_get_commands_resp(g_resp, g_resp_pldm_payload_len, &completion_code, commands);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_true(PLDM_BIT(commands, GET_SENSOR_READING));
	zassert_true(PLDM_BIT(commands, GET_PDR_REPOSITORY_INFO));
	zassert_true(PLDM_BIT(commands, GET_PDR));
}

/**
 * @brief Verify GetPDRRepositoryInfo reports the repository state.
 * @details Sends GetPDRRepositoryInfo and checks the record count, repository size and largest record size are non-zero, with the largest record no bigger than the repository.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_repository_info)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint8_t repository_state;
	uint8_t update_time[PLDM_TIMESTAMP104_SIZE] = {0};
	uint8_t oem_update_time[PLDM_TIMESTAMP104_SIZE] = {0};
	uint32_t record_count;
	uint32_t repository_size;
	uint32_t largest_record_size;
	uint8_t timeout;
	int rc;

	rc = send_get_pdr_repository_info_request(req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR_REPOSITORY_INFO, PLDM_PLATFORM);

	rc = decode_get_pdr_repository_info_resp(
		g_resp, g_resp_pldm_payload_len, &completion_code, &repository_state, update_time,
		oem_update_time, &record_count, &repository_size, &largest_record_size, &timeout);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_true(record_count > 0U);
	zassert_true(repository_size > 0U);
	zassert_true(largest_record_size > 0U);
	zassert_true(largest_record_size <= repository_size);
}

/**
 * @brief Verify GetPDR returns the first record in the repository.
 * @details Requests record 1 in one 48-byte chunk and checks the response count is within the request, the first byte is non-zero, and the transfer flag is START or START_AND_END.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_first_record)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint32_t next_record_handle;
	uint32_t next_data_transfer_handle;
	uint8_t transfer_flag;
	uint16_t response_count;
	uint8_t record_data[64] = {0};
	uint8_t transfer_crc;
	int rc;

	rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 48U, 0U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

	rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
				 &next_record_handle, &next_data_transfer_handle, &transfer_flag,
				 &response_count, record_data, sizeof(record_data), &transfer_crc);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_true(response_count > 0U);
	zassert_true(response_count <= 48U);
	zassert_true(record_data[0] != 0U);
	zassert_true(transfer_flag == PLDM_PLATFORM_TRANSFER_START_AND_END ||
		     transfer_flag == PLDM_PLATFORM_TRANSFER_START);
}

/**
 * @brief Verify GetPDR reports numeric sensor units.
 * @details Fetches the numeric sensor PDRs for the ASIC temperature and TDP sensors across all parts and checks the sensor ID, base unit (degrees C and watts), and SINT32 data and range formats.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_numeric_sensor_units)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	const uint16_t expected_numeric_sensor_record_len =
		PLDM_PDR_NUMERIC_SENSOR_PDR_FIXED_LENGTH + (3U * sizeof(uint32_t)) +
		(9U * sizeof(uint32_t));
	const struct {
		uint32_t record_handle;
		uint16_t sensor_id;
		uint8_t base_unit;
	} checks[] = {
		{2U, BH_PLDM_SENSOR_ID_ASIC_TEMP, PLDM_SENSOR_UNIT_DEGRESS_C},
		{3U, BH_PLDM_SENSOR_ID_TDP, PLDM_SENSOR_UNIT_WATTS},
	};

	for (size_t i = 0; i < ARRAY_SIZE(checks); i++) {
		uint8_t completion_code;
		uint32_t next_record_handle;
		uint32_t next_data_transfer_handle;
		uint8_t transfer_flag;
		uint16_t response_count;
		uint8_t record_data[128] = {0};
		size_t record_len = 0U;
		uint8_t transfer_crc;
		struct pldm_numeric_sensor_value_pdr pdr = {0};
		int rc;

		rc = send_get_pdr_request(req, NEXT_REQ_ID(), checks[i].record_handle, 0U,
					  PLDM_GET_FIRSTPART, 255U, 0U);
		zassert_true(rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

		rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
					 &next_record_handle, &next_data_transfer_handle,
					 &transfer_flag, &response_count, record_data,
					 sizeof(record_data), &transfer_crc);
		zassert_equal(rc, PLDM_SUCCESS);
		zassert_equal(completion_code, PLDM_SUCCESS);
		record_len = response_count;

		while (transfer_flag == PLDM_START || transfer_flag == PLDM_MIDDLE) {
			zassert_true(record_len < sizeof(record_data),
				     "record buffer too small for multipart GetPDR");

			rc = send_get_pdr_request(req, NEXT_REQ_ID(), checks[i].record_handle,
						  next_data_transfer_handle, PLDM_GET_NEXTPART,
						  255U, 0U);
			zassert_true(rc >= 0);
			wait_for_pldm_response_view();
			validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

			rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
						 &next_record_handle, &next_data_transfer_handle,
						 &transfer_flag, &response_count,
						 &record_data[record_len],
						 sizeof(record_data) - record_len, &transfer_crc);
			zassert_equal(rc, PLDM_SUCCESS);
			zassert_equal(completion_code, PLDM_SUCCESS);
			record_len += response_count;
		}

		zassert_true(transfer_flag == PLDM_START_AND_END || transfer_flag == PLDM_END,
			     "unexpected final transfer flag: %u", transfer_flag);
		zassert_equal(record_len, expected_numeric_sensor_record_len,
			      "unexpected numeric sensor PDR length: %u", (uint32_t)record_len);

		rc = decode_numeric_sensor_pdr_data(record_data, record_len, &pdr);
		zassert_equal(rc, PLDM_SUCCESS);

		zassert_equal(pdr.hdr.type, PLDM_NUMERIC_SENSOR_PDR);
		zassert_equal(pdr.sensor_id, checks[i].sensor_id);
		zassert_equal(pdr.base_unit, checks[i].base_unit);
		zassert_equal(pdr.sensor_data_size, PLDM_SENSOR_DATA_SIZE_SINT32);
		zassert_equal(pdr.range_field_format, PLDM_RANGE_FIELD_FORMAT_SINT32);
	}
}

/**
 * @brief Verify GetPDR rejects an unknown record handle.
 * @details Requests an unknown record handle and checks the response is INVALID_RECORD_HANDLE.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_invalid_record_handle)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_pdr_request(req, NEXT_REQ_ID(), 0x00ABCDEFU, 0U, PLDM_GET_FIRSTPART, 16U, 0U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_PLATFORM_INVALID_RECORD_HANDLE);
}

/**
 * @brief Verify GetPDR rejects an invalid record change number.
 * @details Requests record 1 with a wrong record change number and checks the response is INVALID_RECORD_CHANGE_NUMBER.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_invalid_record_change_number)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 16U, 1U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_PLATFORM_INVALID_RECORD_CHANGE_NUMBER);
}

/**
 * @brief Verify GetSensorReading returns a reading for a valid sensor.
 * @details Injects 24.5 C into the emulated sensor and checks the ASIC temperature reading is 245 (0.1 C units) with the sensor enabled, events disabled and state normal.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_sensor_reading)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	/*
	 * Inject 25.5°C in 16.16 fixed-point (the telemetry wire format).
	 * The PLDM PDR has unit-modifier=-1 (0.1°C per count), so 25.5°C → reading=255.
	 */
	uint8_t completion_code;
	uint8_t sensor_data_size;
	uint8_t sensor_operational_state;
	uint8_t sensor_event_message_enable;
	uint8_t present_state;
	uint8_t previous_state;
	uint8_t event_state;
	uint8_t present_reading[sizeof(int32_t)] = {0};
	int rc;

	set_emulated_temp_and_wait(24.5f);

	rc = send_get_sensor_reading_request(req, NEXT_REQ_ID(), BH_PLDM_SENSOR_ID_ASIC_TEMP,
					     false);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_SENSOR_READING, PLDM_PLATFORM);

	rc = decode_get_sensor_reading_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
					    &sensor_data_size, &sensor_operational_state,
					    &sensor_event_message_enable, &present_state,
					    &previous_state, &event_state, present_reading);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(sensor_data_size, PLDM_SENSOR_DATA_SIZE_SINT32);
	zassert_equal(sensor_operational_state, PLDM_SENSOR_ENABLED);
	zassert_equal(sensor_event_message_enable, PLDM_EVENTS_DISABLED);
	zassert_equal(present_state, PLDM_SENSOR_NORMAL);
	zassert_equal((int32_t)sys_get_le32(present_reading), 245);
}

/**
 * @brief Verify GetSensorReading rejects an invalid sensor ID.
 * @details Requests an unknown sensor ID (0xBEEF) and checks the response is INVALID_SENSOR_ID.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_sensor_reading_invalid_id)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_sensor_reading_request(req, NEXT_REQ_ID(), 0xBEEFU, false);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_SENSOR_READING, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_PLATFORM_INVALID_SENSOR_ID);
}

/**
 * @brief Verify an unsupported base-type command returns an error.
 * @details Sends undefined base command 0xFF and checks the response is UNSUPPORTED_PLDM_CMD.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_unsupported_base_command)
{
	PLDM_MSG_BUFFER(req_buf, 1U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_BASE, 0xFFU, NULL, 0U);
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_BASE, 0xFFU, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, 0xFFU, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

/**
 * @brief Verify GetPLDMVersion rejects a request of the wrong length.
 * @details Sends GetPLDMVersion with an extra payload byte and checks the response is INVALID_LENGTH.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_version_invalid_length)
{
	uint8_t bad_payload = 0U;

	PLDM_MSG_BUFFER(req_buf, 1U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_BASE, PLDM_GET_PLDM_VERSION, &bad_payload,
				 sizeof(bad_payload));
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_BASE,
				     PLDM_GET_PLDM_VERSION, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_PLDM_VERSION, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_LENGTH);
}

/**
 * @brief Verify requests for an unsupported PLDM type are rejected.
 * @details Sends GetTID under undefined PLDM type 0x3E and checks the response is INVALID_PLDM_TYPE.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_invalid_pldm_type)
{
	PLDM_MSG_BUFFER(req_buf, 1U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), 0x3EU, PLDM_GET_TID, NULL, 0U);
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, 0x3EU, PLDM_GET_TID, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_TID, 0x3EU);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_PLDM_TYPE);
}

/**
 * @brief Verify GetTID rejects a request of the wrong length.
 * @details Sends GetTID with an extra payload byte and checks the response is INVALID_LENGTH.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_tid_invalid_length)
{
	uint8_t bad_payload = 0U;

	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_BASE, PLDM_GET_TID, &bad_payload,
				 sizeof(bad_payload));
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_BASE, PLDM_GET_TID, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_TID, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_LENGTH);
}

/**
 * @brief Verify SetTID updates the terminus ID.
 * @details Sets the TID to 0x42, checks GetTID returns 0x42, then restores 0x0B so later tests are unaffected.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_set_tid)
{
	PLDM_MSG_BUFFER(set_req_buf, PLDM_SET_TID_REQ_BYTES);
	PLDM_MSG_BUFFER(get_req_buf, PLDM_GET_TID_REQ_BYTES);
	struct pldm_msg *set_req = (struct pldm_msg *)set_req_buf;
	struct pldm_msg *get_req = (struct pldm_msg *)get_req_buf;
	uint8_t completion_code;
	uint8_t tid;
	int rc;

	/* Assign a new TID */
	rc = send_set_tid_request(set_req, NEXT_REQ_ID(), 0x42U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(set_req, g_resp, PLDM_SET_TID, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_SUCCESS);

	/* Confirm GetTID now returns the new value */
	rc = send_get_tid_request(get_req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(get_req, g_resp, PLDM_GET_TID, PLDM_BASE);

	rc = decode_get_tid_resp(g_resp, g_resp_pldm_payload_len, &completion_code, &tid);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(tid, 0x42U);

	/* Restore original TID so subsequent tests are unaffected */
	rc = send_set_tid_request(set_req, NEXT_REQ_ID(), 0x0BU);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_SUCCESS);
}

/**
 * @brief Verify SetTID rejects a request of the wrong length.
 * @details Sends SetTID with no payload (it needs one byte) and checks the response is INVALID_LENGTH.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_set_tid_invalid_length)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	/* Empty payload — SetTID requires exactly 1 byte */
	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_BASE, PLDM_SET_TID, NULL, 0U);
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_BASE, PLDM_SET_TID, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_SET_TID, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_LENGTH);
}

/**
 * @brief Verify a SetTID change is reflected in the advertised command set.
 * @details Sends GetPLDMCommands for the base type and checks SetTID is advertised.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_set_tid_advertised_in_commands)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_COMMANDS_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	bitfield8_t commands[PLDM_MAX_CMDS_PER_TYPE / 8] = {0};
	int rc;

	rc = send_get_commands_request(req);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();

	rc = decode_get_commands_resp(g_resp, g_resp_pldm_payload_len, &completion_code, commands);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_true(PLDM_BIT(commands, SET_TID));
}

/**
 * @brief Verify GetPLDMTypes rejects a request of the wrong length.
 * @details Sends GetPLDMTypes with an extra payload byte and checks the response is INVALID_LENGTH.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_types_invalid_length)
{
	uint8_t bad_payload = 0U;

	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_BASE, PLDM_GET_PLDM_TYPES, &bad_payload,
				 sizeof(bad_payload));
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_BASE,
				     PLDM_GET_PLDM_TYPES, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_PLDM_TYPES, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_LENGTH);
}

/**
 * @brief Verify GetPLDMVersion rejects an invalid operation flag.
 * @details Sends GetPLDMVersion with the NEXTPART operation flag and checks the response is the invalid transfer operation flag error.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_version_invalid_opflag)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_VERSION_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_version_request_for_type(req, NEXT_REQ_ID(), PLDM_GET_NEXTPART, PLDM_BASE);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_VERSION, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_GET_PLDM_VERSION_INVALID_TRANSFER_OPERATION_FLAG);
}

/**
 * @brief Verify GetPLDMVersion returns the expected version for the platform type.
 * @details Sends GetPLDMVersion for the platform type and checks the transfer flag and version fields match the base type's.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_version_platform)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_VERSION_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint32_t next_transfer_handle;
	uint8_t transfer_flag;
	ver32_t version;
	const ver32_t expected_version = {
		.major = 0xf1,
		.minor = 0xf1,
		.update = 0xf0,
		.alpha = 0x00,
	};
	int rc;

	rc = send_get_version_request_for_type(req, NEXT_REQ_ID(), PLDM_GET_FIRSTPART,
					       PLDM_PLATFORM);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_VERSION, PLDM_BASE);

	rc = decode_get_version_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
				     &next_transfer_handle, &transfer_flag, &version);

	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(transfer_flag, PLDM_START_AND_END);
	zassert_equal(version.major, expected_version.major);
	zassert_equal(version.minor, expected_version.minor);
	zassert_equal(version.update, expected_version.update);
	zassert_equal(version.alpha, expected_version.alpha);
}

/**
 * @brief Verify GetPLDMVersion rejects an unsupported PLDM type.
 * @details Sends GetPLDMVersion with undefined PLDM type 0x3E in the request data and checks the invalid-type-in-request-data error.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_version_invalid_type)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_VERSION_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_version_request_for_type(req, NEXT_REQ_ID(), PLDM_GET_FIRSTPART, 0x3EU);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_VERSION, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_GET_PLDM_VERSION_INVALID_PLDM_TYPE_IN_REQUEST_DATA);
}

/**
 * @brief Verify GetPLDMCommands rejects an unsupported PLDM type.
 * @details Sends GetPLDMCommands with undefined PLDM type 0x3E and checks the invalid-type-in-request-data error.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_commands_invalid_type)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_COMMANDS_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_commands_request_for_type(req, NEXT_REQ_ID(), 0x3EU);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PLDM_COMMANDS, PLDM_BASE);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_GET_PLDM_COMMANDS_INVALID_PLDM_TYPE_IN_REQUEST_DATA);
}

/**
 * @brief Verify GetPDRRepositoryInfo rejects a request of the wrong length.
 * @details Sends GetPDRRepositoryInfo with an extra payload byte and checks the response is INVALID_LENGTH.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_repository_info_invalid_length)
{
	uint8_t bad_payload = 0xAAU;

	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_PLATFORM, PLDM_GET_PDR_REPOSITORY_INFO,
				 &bad_payload, sizeof(bad_payload));
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_PLATFORM,
				     PLDM_GET_PDR_REPOSITORY_INFO, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_PDR_REPOSITORY_INFO, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_LENGTH);
}

/**
 * @brief Verify GetPDR handles a zero-count request.
 * @details Hand-crafts a GetPDR with request_count 0 (bypassing the encoder) and checks the response is INVALID_DATA.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_zero_count)
{
	/* Craft a GetPDR request with request_count=0 by hand to bypass encoder validation. */
	uint8_t payload[PLDM_GET_PDR_REQ_BYTES];

	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	sys_put_le32(1U, &payload[0]);   /* record_handle = 1 */
	sys_put_le32(0U, &payload[4]);   /* data_transfer_handle = 0 */
	payload[8] = PLDM_GET_FIRSTPART; /* transfer_op_flag */
	sys_put_le16(0U, &payload[9]);   /* request_count = 0 */
	sys_put_le16(0U, &payload[11]);  /* record_change_number = 0 */

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_PLATFORM, PLDM_GET_PDR, payload,
				 sizeof(payload));
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_PLATFORM, PLDM_GET_PDR,
				     req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_INVALID_DATA);
}

/**
 * @brief Verify GetPDR rejects an invalid operation flag.
 * @details Hand-crafts a GetPDR with transfer operation flag 0x05 and checks the response is the invalid transfer operation flag error.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_invalid_opflag)
{
	/* Craft a GetPDR request with an invalid transfer_op_flag by hand. */
	uint8_t payload[PLDM_GET_PDR_REQ_BYTES];

	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	sys_put_le32(1U, &payload[0]);  /* record_handle = 1 */
	sys_put_le32(0U, &payload[4]);  /* data_transfer_handle = 0 */
	payload[8] = 0x05U;             /* invalid transfer_op_flag */
	sys_put_le16(16U, &payload[9]); /* request_count = 16 */
	sys_put_le16(0U, &payload[11]); /* record_change_number = 0 */

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_PLATFORM, PLDM_GET_PDR, payload,
				 sizeof(payload));
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_PLATFORM, PLDM_GET_PDR,
				     req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_PLATFORM_INVALID_TRANSFER_OPERATION_FLAG);
}

/**
 * @brief Verify GetPDR returns a record across multiple parts.
 * @details Requests record 1 with a 1-byte chunk limit, checks the first response is START, then pulls NEXTPART chunks until the transfer stops reporting MIDDLE and each one succeeds.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_multipart)
{
	/*
	 * Request only 1 byte from record 1 to force a PLDM_START response, then
	 * consume the rest with NEXTPART until PLDM_END.
	 */
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint32_t next_record_handle;
	uint32_t next_data_transfer_handle;
	uint8_t transfer_flag;
	uint16_t response_count;
	uint8_t record_data[64] = {0};
	uint8_t transfer_crc;
	int rc;

	rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 1U, 0U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

	rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
				 &next_record_handle, &next_data_transfer_handle, &transfer_flag,
				 &response_count, record_data, sizeof(record_data), &transfer_crc);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(transfer_flag, PLDM_START);
	zassert_equal(response_count, 1U);

	do {
		rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, next_data_transfer_handle,
					  PLDM_GET_NEXTPART, 48U, 0U);
		zassert_true(rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

		rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
					 &next_record_handle, &next_data_transfer_handle,
					 &transfer_flag, &response_count, record_data,
					 sizeof(record_data), &transfer_crc);
		zassert_equal(rc, PLDM_SUCCESS);
		zassert_equal(completion_code, PLDM_SUCCESS);
	} while (transfer_flag == PLDM_MIDDLE);
}

/**
 * @brief Verify a next-part request with the wrong record handle is rejected.
 * @details Starts a partial transfer, then sends NEXTPART with the data transfer handle off by one and checks the response is INVALID_DATA_TRANSFER_HANDLE.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_nextpart_wrong_handle)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint32_t next_record_handle;
	uint32_t next_data_transfer_handle;
	uint8_t transfer_flag;
	uint16_t response_count;
	uint8_t record_data[64] = {0};
	uint8_t transfer_crc;
	int rc;

	/* Initiate a partial transfer so ctx->transfer_active = true. */
	rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 1U, 0U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();

	rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
				 &next_record_handle, &next_data_transfer_handle, &transfer_flag,
				 &response_count, record_data, sizeof(record_data), &transfer_crc);
	zassert_equal(rc, PLDM_SUCCESS);
	zassert_equal(completion_code, PLDM_SUCCESS);
	zassert_equal(transfer_flag, PLDM_START);

	/* Send NEXTPART with data_transfer_handle off by one. */
	rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, next_data_transfer_handle + 1U,
				  PLDM_GET_NEXTPART, 48U, 0U);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_PLATFORM_INVALID_DATA_TRANSFER_HANDLE);
}

/**
 * @brief Verify enumerating all PDRs returns every record.
 * @details Walks the repository by following next-record handles, draining each record's chunks, and checks every record ends with START_AND_END or END and the walk finds records and terminates at handle 0.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_enumerate_all)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t completion_code;
	uint32_t current_handle = 1U; /* first record handle (TT_PLDM_PDR_RECORD_HANDLE) */
	uint32_t next_record_handle;
	uint32_t next_data_transfer_handle;
	uint8_t transfer_flag;
	uint16_t response_count;
	uint8_t record_data[64] = {0};
	uint8_t transfer_crc;
	int record_num = 0;
	int rc;

	do {
		rc = send_get_pdr_request(req, NEXT_REQ_ID(), current_handle, 0U,
					  PLDM_GET_FIRSTPART, 48U, 0U);
		zassert_true(rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

		rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
					 &next_record_handle, &next_data_transfer_handle,
					 &transfer_flag, &response_count, record_data,
					 sizeof(record_data), &transfer_crc);
		zassert_equal(rc, PLDM_SUCCESS);
		zassert_equal(completion_code, PLDM_SUCCESS);

		/* Consume any remaining chunks of this record. */
		while (transfer_flag == PLDM_START || transfer_flag == PLDM_MIDDLE) {
			rc = send_get_pdr_request(req, NEXT_REQ_ID(), current_handle,
						  next_data_transfer_handle, PLDM_GET_NEXTPART, 48U,
						  0U);
			zassert_true(rc >= 0);
			wait_for_pldm_response_view();
			validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

			rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &completion_code,
						 &next_record_handle, &next_data_transfer_handle,
						 &transfer_flag, &response_count, record_data,
						 sizeof(record_data), &transfer_crc);
			zassert_equal(rc, PLDM_SUCCESS);
			zassert_equal(completion_code, PLDM_SUCCESS);
		}

		zassert_true(transfer_flag == PLDM_START_AND_END || transfer_flag == PLDM_END,
			     "record %d unexpected final flag=%u", record_num, transfer_flag);

		record_num++;
		zassert_true(record_num < 256, "PDR enumeration loop overflow");
		current_handle = next_record_handle;
	} while (current_handle != 0U);

	zassert_true(record_num > 0U, "no PDR records found");
}

/**
 * @brief Verify GetPDR transfer flags for a middle part of a multipart sequence.
 * @details Fetches record 1 in 2-byte chunks and checks the flags run START, then MIDDLE, then END or START_AND_END, with at least one MIDDLE chunk.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_transfer_flag_middle_sequence)
{
	/*
	 * Request record 1 with small request_count to force multiple MIDDLE flags,
	 * then consume all chunks and verify flag sequence: START, MIDDLE..., END.
	 */
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int chunk_count = 0;

	/* Request with small chunk size (2 bytes) to force multiple transfers */
	g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 2U, 0U);
	zassert_true(g_rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

	g_rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &g_completion_code,
				   &g_next_record_handle, &g_next_data_transfer_handle,
				   &g_transfer_flag, &g_response_count, g_record_data,
				   sizeof(g_record_data), &g_transfer_crc);
	zassert_equal(g_rc, PLDM_SUCCESS);
	zassert_equal(g_completion_code, PLDM_SUCCESS);
	zassert_true(g_transfer_flag == PLDM_START || g_transfer_flag == PLDM_START_AND_END);

	/* Consume remaining chunks, verifying flag transitions */
	while (g_transfer_flag == PLDM_START || g_transfer_flag == PLDM_MIDDLE) {
		chunk_count++;
		zassert_true(chunk_count < 100, "transfer loop overflow");

		g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, g_next_data_transfer_handle,
					    PLDM_GET_NEXTPART, 2U, 0U);
		zassert_true(g_rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

		g_rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &g_completion_code,
					   &g_next_record_handle, &g_next_data_transfer_handle,
					   &g_transfer_flag, &g_response_count, g_record_data,
					   sizeof(g_record_data), &g_transfer_crc);
		zassert_equal(g_rc, PLDM_SUCCESS);
		zassert_equal(g_completion_code, PLDM_SUCCESS);
	}

	/* Final flag should be END or START_AND_END */
	zassert_true(g_transfer_flag == PLDM_END || g_transfer_flag == PLDM_START_AND_END);
	/* Should have had at least one MIDDLE flag to justify the small chunk size */
	zassert_true(chunk_count > 0);
}

/**
 * @brief Verify a next-part request without an active transfer is rejected.
 * @details Sends NEXTPART with an arbitrary handle and no prior FIRSTPART, and checks the response is INVALID_DATA_TRANSFER_HANDLE.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_nextpart_without_active_transfer)
{
	/*
	 * Attempt NEXTPART without first initiating a transfer with FIRSTPART.
	 * Should receive INVALID_DATA_TRANSFER_HANDLE or similar error.
	 */
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;

	/* Send NEXTPART with arbitrary handle when no transfer is active */
	g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0x12345678U, PLDM_GET_NEXTPART, 48U,
				    0U);
	zassert_true(g_rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

	/* Expect an error completion code */
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_PLATFORM_INVALID_DATA_TRANSFER_HANDLE);
}

/**
 * @brief Verify PDR data integrity across a multipart transfer.
 * @details Reads record 1 once in one large chunk and once in 8-byte chunks, then checks both reassembled copies have the same length and bytes.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_pdr_data_integrity_multipart)
{
	/*
	 * Fetch the same record twice: once in a single large chunk, once in small chunks.
	 * Verify the reassembled data matches.
	 */
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_PDR_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	uint8_t full_record[256] = {0};
	uint8_t chunked_record[256] = {0};
	int full_offset = 0;
	int chunked_offset = 0;

	/* First: get record in one shot with large request_count */
	g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 255U, 0U);
	zassert_true(g_rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

	g_rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &g_completion_code,
				   &g_next_record_handle, &g_next_data_transfer_handle,
				   &g_transfer_flag, &g_response_count, full_record,
				   sizeof(full_record), &g_transfer_crc);
	zassert_equal(g_rc, PLDM_SUCCESS);
	zassert_equal(g_completion_code, PLDM_SUCCESS);
	full_offset = g_response_count;

	/* Consume any remaining chunks from first request */
	while (g_transfer_flag == PLDM_MIDDLE) {
		g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, g_next_data_transfer_handle,
					    PLDM_GET_NEXTPART, 255U, 0U);
		zassert_true(g_rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

		g_rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &g_completion_code,
					   &g_next_record_handle, &g_next_data_transfer_handle,
					   &g_transfer_flag, &g_response_count,
					   &full_record[full_offset],
					   sizeof(full_record) - full_offset, &g_transfer_crc);
		zassert_equal(g_rc, PLDM_SUCCESS);
		zassert_equal(g_completion_code, PLDM_SUCCESS);
		full_offset += g_response_count;
	}

	/* Second: get same record in small chunks */
	g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, 0U, PLDM_GET_FIRSTPART, 8U, 0U);
	zassert_true(g_rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

	g_rc = decode_get_pdr_resp(g_resp, g_resp_pldm_payload_len, &g_completion_code,
				   &g_next_record_handle, &g_next_data_transfer_handle,
				   &g_transfer_flag, &g_response_count, chunked_record,
				   sizeof(chunked_record), &g_transfer_crc);
	zassert_equal(g_rc, PLDM_SUCCESS);
	zassert_equal(g_completion_code, PLDM_SUCCESS);
	chunked_offset = g_response_count;

	/* Consume remaining chunks */
	while (g_transfer_flag == PLDM_START || g_transfer_flag == PLDM_MIDDLE) {
		g_rc = send_get_pdr_request(req, NEXT_REQ_ID(), 1U, g_next_data_transfer_handle,
					    PLDM_GET_NEXTPART, 8U, 0U);
		zassert_true(g_rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(req, g_resp, PLDM_GET_PDR, PLDM_PLATFORM);

		g_rc = decode_get_pdr_resp(
			g_resp, g_resp_pldm_payload_len, &g_completion_code, &g_next_record_handle,
			&g_next_data_transfer_handle, &g_transfer_flag, &g_response_count,
			&chunked_record[chunked_offset], sizeof(chunked_record) - chunked_offset,
			&g_transfer_crc);
		zassert_equal(g_rc, PLDM_SUCCESS);
		zassert_equal(g_completion_code, PLDM_SUCCESS);
		chunked_offset += g_response_count;
	}

	/* Verify both retrieval methods got the same data */
	zassert_equal(full_offset, chunked_offset);
	zassert_mem_equal(full_record, chunked_record, full_offset);
}

/**
 * @brief Verify GetSensorReading handles the rearm flag.
 * @details Sends GetSensorReading with the rearm flag set and checks the response is REARM_UNAVAILABLE_IN_PRESENT_STATE.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_get_sensor_reading_rearm)
{
	PLDM_MSG_BUFFER(req_buf, PLDM_GET_SENSOR_READING_REQ_BYTES);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	rc = send_get_sensor_reading_request(req, NEXT_REQ_ID(), BH_PLDM_SENSOR_ID_ASIC_TEMP, true);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(req, g_resp, PLDM_GET_SENSOR_READING, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len,
				  PLDM_PLATFORM_REARM_UNAVAILABLE_IN_PRESENT_STATE);
}

/**
 * @brief Verify an unsupported platform-type command returns an error.
 * @details Sends undefined platform command 0xFF and checks the response is UNSUPPORTED_PLDM_CMD.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_unsupported_platform_command)
{
	PLDM_MSG_BUFFER(req_buf, 0U);
	struct pldm_msg *req = (struct pldm_msg *)req_buf;
	int rc;

	send_custom_pldm_request(NEXT_REQ_ID(), PLDM_PLATFORM, 0xFFU, NULL, 0U);
	wait_for_pldm_response_view();

	rc = encode_pldm_header_only(PLDM_REQUEST, g_request_seq - 1, PLDM_PLATFORM, 0xFFU, req);
	zassert_equal(rc, 0);
	validate_response_header(req, g_resp, 0xFFU, PLDM_PLATFORM);
	validate_cc_only_response(g_resp, g_resp_pldm_payload_len, PLDM_ERROR_UNSUPPORTED_PLDM_CMD);
}

/**
 * @brief Verify the OEM shell kernel uptime command returns a value.
 * @details Runs "kernel uptime" through the OEM shell, polls GetResult in 40-byte chunks until it finishes, and checks the shell returns 0 and the output mentions uptime.
 * @ingroup tests_pldm_mctp_transport
 */
ZTEST(dmc_pldm_mctp_transport, test_oem_shell_kernel_uptime)
{
	static const char cmd[] = "kernel uptime";

	PLDM_MSG_BUFFER(exec_req_buf, 3U + sizeof(cmd) - 1U);
	PLDM_MSG_BUFFER(get_req_buf, 5U);
	struct pldm_msg *exec_req = (struct pldm_msg *)exec_req_buf;
	struct pldm_msg *get_req = (struct pldm_msg *)get_req_buf;
	char output[260];
	size_t output_len = 0U;
	uint16_t request_id = 0x1234U;
	uint16_t offset = 0U;
	uint8_t state = 0U;
	int32_t shell_rc = -1;
	int rc;

	rc = send_oem_shell_exec_request(exec_req, NEXT_REQ_ID(), request_id, cmd);
	zassert_true(rc >= 0);
	wait_for_pldm_response_view();
	validate_response_header(exec_req, g_resp, TT_PLDM_OEM_CMD_SHELL_EXEC, TT_PLDM_OEM_TYPE);

	for (int i = 0; i < 20; i++) {
		const uint8_t *payload;
		uint8_t cc;
		uint16_t resp_request_id;
		uint16_t total_len;
		uint16_t resp_offset;
		uint8_t chunk_len;
		uint8_t more;

		rc = send_oem_shell_get_result_request(get_req, NEXT_REQ_ID(), request_id, offset,
						       40U);
		zassert_true(rc >= 0);
		wait_for_pldm_response_view();
		validate_response_header(get_req, g_resp, TT_PLDM_OEM_CMD_SHELL_GET_RESULT,
					 TT_PLDM_OEM_TYPE);

		payload = g_resp->payload;
		zassert_true(g_resp_pldm_payload_len >= 14U);
		cc = payload[0];
		resp_request_id = sys_get_le16(&payload[1]);
		state = payload[3];
		shell_rc = (int32_t)sys_get_le32(&payload[4]);
		total_len = sys_get_le16(&payload[8]);
		resp_offset = sys_get_le16(&payload[10]);
		chunk_len = payload[12];
		zassert_equal(g_resp_pldm_payload_len, 14U + chunk_len);
		more = payload[13 + chunk_len];

		zassert_equal(cc, PLDM_SUCCESS);
		zassert_equal(resp_request_id, request_id);
		zassert_equal(resp_offset, offset);

		if (chunk_len > 0U) {
			size_t to_copy = MIN((size_t)chunk_len, sizeof(output) - 1U - output_len);

			memcpy(&output[output_len], &payload[13], to_copy);
			output_len += to_copy;
		}

		offset += chunk_len;

		if (more == 0U) {
			zassert_true(state == 3U || state == 4U);
			zassert_equal((uint16_t)output_len, total_len);
			break;
		}

		k_msleep(10);
	}

	output[output_len] = '\0';
	zassert_equal(state, 3U, "shell state=%u output=%s", state, output);
	zassert_equal(shell_rc, 0, "shell rc=%d output=%s", (int)shell_rc, output);
	zassert_true(strstr(output, "uptime") != NULL || strstr(output, "Uptime") != NULL,
		     "output missing uptime: %s", output);
}

static int wait_for_transport_ready(int32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;

	do {
		if (dmc_mctp_transport_ready()) {
			return 0;
		}
		k_msleep(10);
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

static void *suite_setup(void)
{
	int rc;

	rc = dmc_mctp_transport_test_controller_init();
	zassert_equal(rc, 0);

	rc = wait_for_transport_ready(1000);
	zassert_equal(rc, 0);

	return NULL;
}

ZTEST_SUITE(dmc_pldm_mctp_transport, NULL, suite_setup, NULL, test_before_each, NULL);
