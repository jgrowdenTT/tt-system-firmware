/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host-side tests for the BL0.5 launcher (app/bl0p5/src/bl0p5_launch.c).
 *
 * The launcher runs against RAM. The BUN2 staging area, the host boot state and bundle validation
 * registers, and the reset vector are all buffers in this file. The pause hook plays the host: it
 * writes the next scripted register value on each poll. The jump hook is an FFF fake, so the
 * launcher returns instead of transferring control.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/fff.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "bl0p5_launch.h"

DEFINE_FFF_GLOBALS;

/* Host side of the handshake, run once per poll. Each entry is applied on one poll, in order. */
DECLARE_FAKE_VOID_FUNC(fake_pause);
DEFINE_FAKE_VOID_FUNC(fake_pause);
/* Stands in for gr_smc_jump_to(). */
DECLARE_FAKE_VOID_FUNC(fake_jump, uintptr_t);
DEFINE_FAKE_VOID_FUNC(fake_jump, uintptr_t);

#define STAGING_SIZE       8192
#define PAYLOAD_OFFSET     sizeof(struct fw_bundle_manifest)
#define PAYLOAD_DATA_OFF   1024
#define MAX_SCRIPT_STEPS   8
#define MAX_SEEN_STATES    8
#define BL1_ENTRY_POINT    0x0000000012345678ULL
#define NOT_COPIED_BYTE    0xAA
/* Poll budget per wait for the tests. Large enough for the scripted host, small enough that a
 * timeout test does not poll for long.
 */
#define TEST_MAX_POLLS     16

struct step {
	volatile uint32_t *reg;
	uint32_t val;
};

static uint8_t staging[STAGING_SIZE] __aligned(8);
static uint8_t bl1_dest[64] __aligned(8);
static uint8_t other_dest[64] __aligned(8);
static uint8_t reset_vector[8] __aligned(8);

static volatile uint32_t host_boot_state;
static volatile uint32_t bundle_validation;

/* Stand-in BL1 payload. Filled in by the before-hook, so every byte is distinct from the
 * NOT_COPIED_BYTE sentinel and from its neighbours.
 */
static uint8_t bl1_image[64];

static const struct step *script;
static size_t script_len;
static size_t script_pos;

/* Host boot state observed at each poll, before the scripted write for that poll. */
static uint32_t seen_host_state[MAX_SEEN_STATES];
static size_t seen_count;

/* What the jump saw: the host state and whether the BL1 image had already been copied. */
static uint32_t jump_host_state;
static bool jump_image_copied;
static uintptr_t jump_entry;

static void pause_step(void)
{
	if (seen_count < MAX_SEEN_STATES) {
		seen_host_state[seen_count++] = host_boot_state;
	}
	if (script_pos < script_len) {
		*script[script_pos].reg = script[script_pos].val;
		script_pos++;
	}
}

static void jump_capture(uintptr_t entry_point)
{
	jump_host_state = host_boot_state;
	jump_image_copied = (memcmp(bl1_dest, bl1_image, sizeof(bl1_image)) == 0);
	jump_entry = entry_point;
}

/* True if every byte still holds the value the before-hook filled it with. */
static bool is_untouched(const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (buf[i] != NOT_COPIED_BYTE) {
			return false;
		}
	}
	return true;
}

static void run_script(const struct step *steps, size_t len)
{
	script = steps;
	script_len = len;
	script_pos = 0;
}

/* Lay out a BUN2 bundle in the staging area. TOC entry 0 has type0 and loads BL1 into bl1_dest.
 * TOC entry 1 has type1 and loads into other_dest, so a test can check that it is ignored.
 */
static void stage_bundle(uint64_t image_count, uint64_t type0, uint64_t type1)
{
	struct fw_bundle_manifest *manifest = (struct fw_bundle_manifest *)staging;
	struct fw_bundle_toc *toc = (struct fw_bundle_toc *)(staging + PAYLOAD_OFFSET);
	uint8_t *payload = staging + PAYLOAD_OFFSET;

	memset(staging, 0, sizeof(staging));
	manifest->payload_offset = PAYLOAD_OFFSET;

	toc->image_count = image_count;
	memcpy(payload + PAYLOAD_DATA_OFF, bl1_image, sizeof(bl1_image));

	toc->entries[0].type = type0;
	toc->entries[0].offset = PAYLOAD_DATA_OFF;
	toc->entries[0].length = sizeof(bl1_image);
	toc->entries[0].load_addr = (uint64_t)(uintptr_t)bl1_dest;
	toc->entries[0].entry_point = BL1_ENTRY_POINT;

	toc->entries[1].type = type1;
	toc->entries[1].offset = PAYLOAD_DATA_OFF;
	toc->entries[1].length = sizeof(bl1_image);
	toc->entries[1].load_addr = (uint64_t)(uintptr_t)other_dest;
	toc->entries[1].entry_point = 0;
}

static struct bl0p5_regs make_regs(void)
{
	return (struct bl0p5_regs){
		.host_boot_state = (uintptr_t)&host_boot_state,
		.bundle_validation = (uintptr_t)&bundle_validation,
		.staging_area = (uintptr_t)staging,
		.reset_vector = (uintptr_t)reset_vector,
		.max_polls = TEST_MAX_POLLS,
		.pause = fake_pause,
		.jump = fake_jump,
	};
}

static void tt_bl0p5_launch_before(void *fixture)
{
	ARG_UNUSED(fixture);

	for (size_t i = 0; i < sizeof(bl1_image); i++) {
		bl1_image[i] = (uint8_t)(0x10 + i);
	}

	RESET_FAKE(fake_pause);
	RESET_FAKE(fake_jump);
	FFF_RESET_HISTORY();

	fake_pause_fake.custom_fake = pause_step;
	fake_jump_fake.custom_fake = jump_capture;

	host_boot_state = 0;
	bundle_validation = 0;
	memset(bl1_dest, NOT_COPIED_BYTE, sizeof(bl1_dest));
	memset(other_dest, NOT_COPIED_BYTE, sizeof(other_dest));
	memset(reset_vector, 0, sizeof(reset_vector));
	memset(seen_host_state, 0, sizeof(seen_host_state));
	seen_count = 0;
	jump_host_state = 0;
	jump_image_copied = false;
	jump_entry = 0;

	run_script(NULL, 0);
	stage_bundle(1, FW_BUNDLE_IMG_TYPE_SMC_BL1, FW_BUNDLE_IMG_TYPE_SMC_BL0P5);
}

ZTEST_SUITE(tt_bl0p5_launch, NULL, NULL, tt_bl0p5_launch_before, NULL, NULL);

/**
 * @brief The launcher announces it is waiting for a bundle before it polls.
 *
 * Writes WAIT_FOR_BUNDLE (1) to the host boot state register on entry. The first poll then
 * observes that value, not an earlier one.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_announces_wait_for_bundle_first)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};

	run_script(steps, ARRAY_SIZE(steps));
	zassert_equal(bl0p5_launch(&regs), 0);

	zassert_true(seen_count >= 1, "no poll happened");
	zassert_equal(seen_host_state[0], 1, "first poll saw %u", seen_host_state[0]);
}

/**
 * @brief The staged check looks only at the low nibble of the host boot state.
 *
 * A value of 0x11 has low nibble 1, so the launcher keeps waiting. 0x12 has low nibble 2
 * (BUNDLE_STAGED), so it proceeds. The test expects exactly three polls: two for staging and one
 * for validation.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_staged_check_masks_upper_bits)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 0x11},
		{&host_boot_state, 0x12},
		{&bundle_validation, 2},
	};

	run_script(steps, ARRAY_SIZE(steps));
	zassert_equal(bl0p5_launch(&regs), 0);

	zassert_equal(fake_pause_fake.call_count, 3, "polls: %d", fake_pause_fake.call_count);
	zassert_equal(seen_host_state[1], 0x11, "second poll saw 0x%x", seen_host_state[1]);
	zassert_equal(seen_host_state[2], 0x12, "third poll saw 0x%x", seen_host_state[2]);
	zassert_equal(fake_jump_fake.call_count, 1);
}

/**
 * @brief Validation completes only when the VALIDATED bit (bit 1) is set.
 *
 * The validation register is set to 0x1, the READY bit alone, and the launcher must keep
 * polling. It proceeds only once 0x2 is written.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_validation_requires_validated_bit)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
		{&bundle_validation, 1},
		{&bundle_validation, 2},
	};

	run_script(steps, ARRAY_SIZE(steps));
	zassert_equal(bl0p5_launch(&regs), 0);

	zassert_equal(fake_pause_fake.call_count, 3,
		      "polls: %d, so the READY-only state was not waited out",
		      fake_pause_fake.call_count);
}

/**
 * @brief A valid bundle is copied to its load address and the launcher jumps to its entry point.
 *
 * Checks, in order: the host state is CONSUMED (3) and the BL1 image is already in place when the
 * jump happens. Then that the reset vector holds the 64-bit entry point, that the jump got that
 * entry point, and that the image at TOC index 1 was not copied.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_valid_bundle_copies_bl1_and_jumps)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};
	uint64_t vector = 0;

	run_script(steps, ARRAY_SIZE(steps));
	zassert_equal(bl0p5_launch(&regs), 0);

	zassert_equal(fake_jump_fake.call_count, 1);
	zassert_equal(jump_entry, (uintptr_t)BL1_ENTRY_POINT);
	zassert_equal(jump_host_state, 3, "host state at jump was %u", jump_host_state);
	zassert_true(jump_image_copied, "BL1 image was not in place at the jump");
	zassert_equal(host_boot_state, 3);

	memcpy(&vector, reset_vector, sizeof(vector));
	zassert_equal(vector, BL1_ENTRY_POINT, "reset vector is 0x%llx", vector);

	zassert_true(is_untouched(other_dest, sizeof(other_dest)),
		     "TOC entry 1 was copied (first byte 0x%02x)", other_dest[0]);
}

/**
 * @brief A bundle with no TOC entries is rejected without a copy or a jump.
 *
 * Returns -ENOENT. The host state stays at STAGED (2), not CONSUMED, so the host can see the
 * handoff did not happen. Neither the BL1 image nor the reset vector is touched.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_empty_bundle_returns_enoent)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};

	stage_bundle(0, FW_BUNDLE_IMG_TYPE_SMC_BL1, FW_BUNDLE_IMG_TYPE_SMC_BL0P5);
	run_script(steps, ARRAY_SIZE(steps));

	zassert_equal(bl0p5_launch(&regs), -ENOENT);
	zassert_equal(fake_jump_fake.call_count, 0);
	zassert_equal(host_boot_state, 2, "host state changed to %u", host_boot_state);
	zassert_true(is_untouched(bl1_dest, sizeof(bl1_dest)), "BL1 image was copied");
	zassert_equal(reset_vector[0], 0, "reset vector was written");
}

/**
 * @brief TOC entry 0 of a type other than SMC BL1 is rejected.
 *
 * The bundle has an SMC BL0.5 image at index 0 and an SMC BL1 image at index 1. The launcher
 * returns -ENOENT, so it does not fall through to the BL1 image at index 1.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_wrong_type_at_index0_returns_enoent)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};

	stage_bundle(2, FW_BUNDLE_IMG_TYPE_SMC_BL0P5, FW_BUNDLE_IMG_TYPE_SMC_BL1);
	run_script(steps, ARRAY_SIZE(steps));

	zassert_equal(bl0p5_launch(&regs), -ENOENT);
	zassert_equal(fake_jump_fake.call_count, 0);
	zassert_equal(host_boot_state, 2);
	zassert_true(is_untouched(bl1_dest, sizeof(bl1_dest)), "BL1 image was copied");
}

/**
 * @brief Only TOC index 0 is used. An SMC BL1 image at a later index is not a substitute.
 *
 * The bundle has a SEP BL1 image at index 0 and an SMC BL1 image at index 1. The launcher returns
 * -ENOENT. This matches the comment in the launcher that BL1 always sits at index 0.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_bl1_at_later_index_is_not_used)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};

	stage_bundle(2, FW_BUNDLE_IMG_TYPE_SEP_BL1, FW_BUNDLE_IMG_TYPE_SMC_BL1);
	run_script(steps, ARRAY_SIZE(steps));

	zassert_equal(bl0p5_launch(&regs), -ENOENT);
	zassert_equal(fake_jump_fake.call_count, 0);
	zassert_true(is_untouched(bl1_dest, sizeof(bl1_dest)), "BL1 image was copied");
}

/**
 * @brief A host that never stages the bundle makes the launcher time out.
 *
 * The staging wait polls max_polls times and then returns -ETIMEDOUT. Expects exactly max_polls
 * pauses, no copy, no jump, and the host state left at WAIT_FOR_BUNDLE (1).
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_staging_timeout_returns_etimedout)
{
	struct bl0p5_regs regs = make_regs();

	regs.max_polls = 5;
	run_script(NULL, 0);

	zassert_equal(bl0p5_launch(&regs), -ETIMEDOUT);
	zassert_equal(fake_pause_fake.call_count, 5, "polls: %d", fake_pause_fake.call_count);
	zassert_equal(fake_jump_fake.call_count, 0);
	zassert_equal(host_boot_state, 1, "host state changed to %u", host_boot_state);
	zassert_true(is_untouched(bl1_dest, sizeof(bl1_dest)), "BL1 image was copied");
}

/**
 * @brief A host that stages but never validates makes the launcher time out.
 *
 * The staging wait completes with one poll. The validation wait then polls max_polls times and
 * returns -ETIMEDOUT. Expects one staging pause plus max_polls validation pauses, no copy, no
 * jump, and the host state left at STAGED (2), so it was not CONSUMED.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_validation_timeout_returns_etimedout)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 2},
	};

	regs.max_polls = 5;
	run_script(steps, ARRAY_SIZE(steps));

	zassert_equal(bl0p5_launch(&regs), -ETIMEDOUT);
	zassert_equal(fake_pause_fake.call_count, 6, "polls: %d", fake_pause_fake.call_count);
	zassert_equal(fake_jump_fake.call_count, 0);
	zassert_equal(host_boot_state, 2, "host state changed to %u", host_boot_state);
	zassert_true(is_untouched(bl1_dest, sizeof(bl1_dest)), "BL1 image was copied");
}

/**
 * @brief Staging that completes on the last allowed poll is accepted.
 *
 * The host is still WAIT (1) on the first two polls and STAGED (2) on the third. With max_polls
 * set to 3 that is the last poll allowed, so the launcher proceeds and jumps.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_staging_on_last_allowed_poll_succeeds)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 1},
		{&host_boot_state, 1},
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};

	regs.max_polls = 3;
	run_script(steps, ARRAY_SIZE(steps));

	zassert_equal(bl0p5_launch(&regs), 0);
	zassert_equal(fake_jump_fake.call_count, 1);
}

/**
 * @brief Staging that would complete one poll too late times out.
 *
 * The same host script as the previous test, with max_polls set to 2. The third poll that would
 * stage the bundle is not allowed, so the launcher returns -ETIMEDOUT without copying or jumping.
 *
 * @ingroup tests_bl0p5_launch
 */
ZTEST(tt_bl0p5_launch, test_staging_one_poll_late_times_out)
{
	struct bl0p5_regs regs = make_regs();
	const struct step steps[] = {
		{&host_boot_state, 1},
		{&host_boot_state, 1},
		{&host_boot_state, 2},
		{&bundle_validation, 2},
	};

	regs.max_polls = 2;
	run_script(steps, ARRAY_SIZE(steps));

	zassert_equal(bl0p5_launch(&regs), -ETIMEDOUT);
	zassert_equal(fake_pause_fake.call_count, 2);
	zassert_equal(fake_jump_fake.call_count, 0);
	zassert_true(is_untouched(bl1_dest, sizeof(bl1_dest)), "BL1 image was copied");
}
