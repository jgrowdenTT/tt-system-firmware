/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 * SPDX-License-Identifier: Apache-2.0
 */

#include "timer.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/init.h>
#include <tenstorrent/sys_init_defines.h>

LOG_MODULE_REGISTER(main, CONFIG_TT_APP_LOG_LEVEL);

static const struct device *const wdt0 = DEVICE_DT_GET(DT_NODELABEL(wdt0));

BUILD_ASSERT(PARTITION_EXISTS(cmfw), "cmfw fixed-partition does not exist");

#ifdef CONFIG_BOOTLOADER_MCUBOOT
/*
 * DEBUG: confirm the image as early as possible (right after flash is usable and
 * before PLLInit reclocks SPI) so a reset during ARC init does not cause MCUBoot
 * to revert/erase the main slot. Normally this happens at the end of boot in main().
 */
#define early_confirm_PRIO 96
BUILD_ASSERT(early_confirm_PRIO > CONFIG_FLASH_TRAINING_PRIORITY,
	     "early confirm must run after flash training");
BUILD_ASSERT(early_confirm_PRIO < PLLInit_PRIO, "early confirm must run before PLL reclock");

static int early_confirm(void)
{
	int rc;

	if (boot_is_img_confirmed()) {
		return 0;
	}

	rc = boot_write_img_confirmed();
	if (rc < 0) {
		printk("Early image confirm failed: %d\n", rc);
		return 0;
	}
	printk("Firmware update is confirmed (early).\n");

	return 0;
}
SYS_INIT(early_confirm, POST_KERNEL, early_confirm_PRIO);
#endif

int main(void)
{
	while (1) {
		sys_trace_named_event("main_loop", TimerTimestamp(), 0);
		k_msleep(CONFIG_TT_BH_ARC_WDT_FEED_INTERVAL);
		wdt_feed(wdt0, 0);
	}

	return 0;
}
