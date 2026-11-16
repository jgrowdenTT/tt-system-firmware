/*
 * Copyright (c) 2024 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include <librpmi.h>

/**
 * @brief Placeholder: the RPMI common library builds and runs.
 * @details No checks yet. The test passes once the suite builds and runs.
 * @ingroup tests_rpmi
 */
ZTEST(rpmi_common, test_nada)
{
	zassert_equal(true, true);
}

ZTEST_SUITE(rpmi_common, NULL, NULL, NULL, NULL, NULL);
