/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * tt_grendel_pcie (HW_INIT) expects BL1 to have staged a firmware bundle at
 * TT_BUN1_STAGING_AREA_ADDR with the SERDES FW at TOC[2]. This test runs
 * standalone, so stage a minimal bundle before the PCIe device initialises.
 *
 * Layout: only the manifest and TOC are written into the staging area, which
 * lies below ram0 and so can never collide with this image. The SERDES blob is
 * an ordinary array inside the image; its TOC offset is simply the distance
 * from the end of the manifest to the array, so no payload is copied into the
 * staging area and nothing there overlaps the image.
 *
 * The blob holds the real SERDES ICCM and DCCM images from the SiVal SDK
 * (embedded by CMakeLists.txt), so the test also brings the link up on silicon.
 */

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/misc/tt_bundle_loader.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>

#define TOC_ENTRY_COUNT 3U
#define SERDES_TOC_IDX  2U

/* The driver points DCCM 16384 words past ICCM; give each a full 16384 words. */
#define SERDES_MEM_BYTES (16384U * sizeof(uint32_t))

#define STAGING_BASE  TT_BUN1_STAGING_AREA_ADDR
#define STAGING_LIMIT DT_REG_ADDR(DT_NODELABEL(ram0))

#define TOC_OFFSET ROUND_UP(sizeof(struct fw_bundle_manifest), 8)
#define STAGING_USED                                                                               \
	(TOC_OFFSET + sizeof(struct fw_bundle_toc) +                                               \
	 TOC_ENTRY_COUNT * sizeof(struct fw_bundle_toc_entry))

BUILD_ASSERT(STAGING_BASE + STAGING_USED <= STAGING_LIMIT,
	     "staged manifest+TOC overruns the area below ram0");

/* CMakeLists.txt rejects images larger than SERDES_MEM_BYTES; shorter ones are zero-padded. */

/* clang-format off */
static const struct {
	uint8_t iccm[SERDES_MEM_BYTES];
	uint8_t dccm[SERDES_MEM_BYTES];
} serdes_blob __aligned(8) = {
	.iccm = {
		#include "serdes_iccm_fw.bin.inc"
		},
	.dccm = {
		#include "serdes_dccm_fw.bin.inc"
		},
};
/* clang-format on */

static int stage_bundle(void)
{
	struct fw_bundle_manifest *manifest = (struct fw_bundle_manifest *)STAGING_BASE;
	struct fw_bundle_toc *toc = (struct fw_bundle_toc *)(STAGING_BASE + TOC_OFFSET);
	uintptr_t blob = (uintptr_t)&serdes_blob;

	/* The blob must be outside the staged bytes; it is also required to be above them. */
	if (blob < STAGING_BASE + STAGING_USED) {
		return -EFAULT;
	}

	memset(manifest, 0, STAGING_USED);
	manifest->manifest_identifier = MANIFEST_ID_BL1;
	manifest->manifest_length = sizeof(*manifest);
	manifest->payload_offset = TOC_OFFSET;

	toc->toc_identifier = FW_BUNDLE_TOC_ID;
	toc->toc_version_major = 1;
	toc->image_count = TOC_ENTRY_COUNT;

	/* Offset is relative to staging base + payload_offset, as the driver computes it. */
	toc->entries[SERDES_TOC_IDX].offset = blob - STAGING_BASE - TOC_OFFSET;
	toc->entries[SERDES_TOC_IDX].length = sizeof(serdes_blob);

	return 0;
}

/* Must run before the PCIe device init (POST_KERNEL). */
SYS_INIT(stage_bundle, PRE_KERNEL_1, 0);
