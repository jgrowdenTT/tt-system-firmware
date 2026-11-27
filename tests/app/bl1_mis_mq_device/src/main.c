/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Tests for the BL1 Mimir/Quasar launcher, app/bl1/mis_launch_mq.c, built unchanged.
 *
 * The two mailboxes are rebound to a stub driver in the test overlay, so the real driver never
 * binds. The stub's API table points at FFF fakes. The inline mbox_send(), mbox_set_enabled(),
 * and mbox_register_callback() wrappers call those table entries, so the launcher's calls land
 * in the fakes. The Keraunos go is delivered by calling the callback the launcher registered,
 * from inside the fake ping.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/drivers/misc/tt_chiplet_remote_boot.h>
#include <zephyr/fff.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <tenstorrent/gr_smc.h>

#include "mis_launch.h"

DEFINE_FFF_GLOBALS;

/* FFF needs each fake declared before it is defined. */

/* gr_smc: not built in this test, so these are the definitions. */
DECLARE_FAKE_VALUE_FUNC0(struct gr_smc_chipid, gr_smc_get_chipid);
DECLARE_FAKE_VOID_FUNC(gr_smc_jump_to, uintptr_t);
DEFINE_FAKE_VALUE_FUNC0(struct gr_smc_chipid, gr_smc_get_chipid);
DEFINE_FAKE_VOID_FUNC(gr_smc_jump_to, uintptr_t);

/* mbox driver API entries. The inline mbox_*() wrappers dispatch to these. */
DECLARE_FAKE_VALUE_FUNC(int, fake_mbox_send, const struct device *, mbox_channel_id_t,
			const struct mbox_msg *);
DECLARE_FAKE_VALUE_FUNC(int, fake_mbox_set_enabled, const struct device *, mbox_channel_id_t,
			bool);
DECLARE_FAKE_VALUE_FUNC(int, fake_mbox_register_callback, const struct device *,
			mbox_channel_id_t, mbox_callback_t, void *);
DEFINE_FAKE_VALUE_FUNC(int, fake_mbox_send, const struct device *, mbox_channel_id_t,
		       const struct mbox_msg *);
DEFINE_FAKE_VALUE_FUNC(int, fake_mbox_set_enabled, const struct device *, mbox_channel_id_t,
		       bool);
DEFINE_FAKE_VALUE_FUNC(int, fake_mbox_register_callback, const struct device *,
		       mbox_channel_id_t, mbox_callback_t, void *);

#define MIS_ENTRY_POINT  0xC0067000UL
#define MIS_MBOX_CHANNEL 1

/* Callback and user data the launcher registered on mbox0. */
static mbox_callback_t registered_cb;
static void *registered_ud;
static const struct device *registered_dev;

/* When set, the fake ping delivers the Keraunos go through the registered callback. */
static bool deliver_go;

static int init_stub(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static DEVICE_API(mbox, stub_api) = {
	.send = fake_mbox_send,
	.set_enabled = fake_mbox_set_enabled,
	.register_callback = fake_mbox_register_callback,
};

/* The two mailboxes the launcher uses. Their nodes are rebound to the stub in the overlay. */
DEVICE_DT_DEFINE(DT_NODELABEL(mbox0), init_stub, NULL, NULL, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &stub_api);
DEVICE_DT_DEFINE(DT_NODELABEL(k_mbox), init_stub, NULL, NULL, NULL, POST_KERNEL,
		 CONFIG_KERNEL_INIT_PRIORITY_DEVICE, &stub_api);

static int register_capture(const struct device *dev, mbox_channel_id_t channel_id,
			    mbox_callback_t cb, void *user_data)
{
	ARG_UNUSED(channel_id);
	registered_dev = dev;
	registered_cb = cb;
	registered_ud = user_data;
	return fake_mbox_register_callback_fake.return_val;
}

/* The ready ping to Keraunos. Delivers the go on the registered channel when asked to. */
static int ping_capture(const struct device *dev, mbox_channel_id_t channel_id,
			const struct mbox_msg *msg)
{
	int ret = fake_mbox_send_fake.return_val;

	ARG_UNUSED(dev);
	ARG_UNUSED(channel_id);
	ARG_UNUSED(msg);

	if (ret == 0 && deliver_go && registered_cb != NULL) {
		/* The launcher's rx callback ignores its msg argument. */
		registered_cb(registered_dev, MIS_MBOX_CHANNEL, registered_ud, NULL);
	}

	return ret;
}

static void mis_mq_before(void *fixture)
{
	ARG_UNUSED(fixture);

	RESET_FAKE(gr_smc_get_chipid);
	RESET_FAKE(gr_smc_jump_to);
	RESET_FAKE(fake_mbox_send);
	RESET_FAKE(fake_mbox_set_enabled);
	RESET_FAKE(fake_mbox_register_callback);
	FFF_RESET_HISTORY();

	registered_cb = NULL;
	registered_ud = NULL;
	registered_dev = NULL;
	deliver_go = false;

	fake_mbox_register_callback_fake.custom_fake = register_capture;
	fake_mbox_send_fake.custom_fake = ping_capture;

	gr_smc_get_chipid_fake.return_val = (struct gr_smc_chipid){
		.type = GR_SMC_CHIP_MIMIR,
		.instance = 1,
	};
}

ZTEST_SUITE(mis_launch_mq_device, NULL, NULL, mis_mq_before, NULL, NULL);

/**
 * @brief A Mimir with instance 1 pings Keraunos on its pair, takes the go, and jumps to MIS.
 *
 * @ingroup tests_bl1_mis_mq_device
 */
ZTEST(mis_launch_mq_device, test_ping_then_go_jumps_to_mis)
{
	const struct device *mbox0 = DEVICE_DT_GET(DT_NODELABEL(mbox0));
	const struct device *k_mbox = DEVICE_DT_GET(DT_NODELABEL(k_mbox));
	uint32_t pair = TT_CHIPLET_READY_PAIR_MIMIR(1);

	deliver_go = true;

	launch_mis();

	zassert_equal(gr_smc_get_chipid_fake.call_count, 1);
	zassert_equal(fake_mbox_register_callback_fake.call_count, 1);
	zassert_equal_ptr(fake_mbox_register_callback_fake.arg0_val, mbox0);
	zassert_equal(fake_mbox_register_callback_fake.arg1_val, MIS_MBOX_CHANNEL);

	zassert_equal(fake_mbox_set_enabled_fake.call_count, 1);
	zassert_equal_ptr(fake_mbox_set_enabled_fake.arg0_val, mbox0);
	zassert_equal(fake_mbox_set_enabled_fake.arg1_val, MIS_MBOX_CHANNEL);
	zassert_true(fake_mbox_set_enabled_fake.arg2_val);

	zassert_equal(fake_mbox_send_fake.call_count, 1);
	zassert_equal_ptr(fake_mbox_send_fake.arg0_val, k_mbox);
	zassert_equal(fake_mbox_send_fake.arg1_val, TT_CHIPLET_READY_OUT(pair));

	zassert_equal(gr_smc_jump_to_fake.call_count, 1);
	zassert_equal(gr_smc_jump_to_fake.arg0_val, MIS_ENTRY_POINT);
}

/**
 * @brief The ready ping uses the pair for the chiplet's CHIP_ID instance.
 *
 * @ingroup tests_bl1_mis_mq_device
 */
ZTEST(mis_launch_mq_device, test_ready_pair_follows_chip_instance)
{
	const struct device *k_mbox = DEVICE_DT_GET(DT_NODELABEL(k_mbox));

	gr_smc_get_chipid_fake.return_val = (struct gr_smc_chipid){
		.type = GR_SMC_CHIP_MIMIR,
		.instance = 2,
	};
	deliver_go = true;

	launch_mis();

	zassert_equal(fake_mbox_send_fake.call_count, 1);
	zassert_equal_ptr(fake_mbox_send_fake.arg0_val, k_mbox);
	zassert_equal(fake_mbox_send_fake.arg1_val,
		      TT_CHIPLET_READY_OUT(TT_CHIPLET_READY_PAIR_MIMIR(2)));
	zassert_equal(gr_smc_jump_to_fake.call_count, 1);
}

/**
 * @brief A failed callback registration returns before any ping or jump.
 *
 * @ingroup tests_bl1_mis_mq_device
 */
ZTEST(mis_launch_mq_device, test_register_failure_returns_without_ping)
{
	fake_mbox_register_callback_fake.return_val = -EIO;

	launch_mis();

	zassert_equal(fake_mbox_register_callback_fake.call_count, 1);
	zassert_equal(fake_mbox_set_enabled_fake.call_count, 0);
	zassert_equal(fake_mbox_send_fake.call_count, 0);
	zassert_equal(gr_smc_jump_to_fake.call_count, 0);
}

/**
 * @brief A failed channel enable disables the channel again and returns before any ping.
 *
 * @ingroup tests_bl1_mis_mq_device
 */
ZTEST(mis_launch_mq_device, test_enable_failure_disables_and_returns)
{
	fake_mbox_set_enabled_fake.return_val = -EIO;

	launch_mis();

	zassert_equal(fake_mbox_set_enabled_fake.call_count, 2);
	zassert_false(fake_mbox_set_enabled_fake.arg2_val,
		      "last set_enabled call did not disable the channel");
	zassert_equal(fake_mbox_send_fake.call_count, 0);
	zassert_equal(gr_smc_jump_to_fake.call_count, 0);
}

/**
 * @brief A failed ready ping returns without a jump.
 *
 * @ingroup tests_bl1_mis_mq_device
 */
ZTEST(mis_launch_mq_device, test_ping_failure_returns_without_jump)
{
	fake_mbox_send_fake.return_val = -EIO;
	deliver_go = true;

	launch_mis();

	zassert_equal(fake_mbox_send_fake.call_count, 1);
	zassert_equal(gr_smc_jump_to_fake.call_count, 0);
}

/**
 * @brief A mailbox that is not ready makes the launcher return before any mailbox call.
 *
 * device_is_ready() reads the initialized flag in the device state, so the test clears it for
 * mbox0 and restores it afterwards.
 *
 * @ingroup tests_bl1_mis_mq_device
 */
ZTEST(mis_launch_mq_device, test_device_not_ready_returns_without_mbox_calls)
{
	const struct device *mbox0 = DEVICE_DT_GET(DT_NODELABEL(mbox0));
	struct device_state *state = (struct device_state *)mbox0->state;
	bool saved = state->initialized;

	state->initialized = false;
	launch_mis();
	state->initialized = saved;

	zassert_equal(fake_mbox_register_callback_fake.call_count, 0);
	zassert_equal(fake_mbox_set_enabled_fake.call_count, 0);
	zassert_equal(fake_mbox_send_fake.call_count, 0);
	zassert_equal(gr_smc_jump_to_fake.call_count, 0);
}
