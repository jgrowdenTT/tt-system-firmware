/*
 * Copyright (c) 2026 Tenstorrent AI ULC
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TENSTORRENT_COVERAGE_HANDOFF_H_
#define TENSTORRENT_COVERAGE_HANDOFF_H_

/*
 * Coverage hand-off protocol, shared by the firmware driver (drivers/misc/tt_coverage_handoff)
 * and the host reader (tt-kmd tools/keraunos_vuart_console.c, which mirrors these values).
 *
 * For each gcov buffer the firmware:
 *   1. prints "GCOV_HEAP_CHUNK <size>\n" on the VUART (the console CRLF is ignored by the host),
 *   2. writes the buffer's local address to the coverage-handoff scratch register,
 *   3. waits for the host to clear that register.
 * The host reads <size> bytes from the address, emits them as hex in place of the marker line,
 * and writes 0 to the register.
 */

/* Marker text printed before each chunk; the decimal byte count follows it. */
#define TT_COV_HANDOFF_MARKER "GCOV_HEAP_CHUNK "

/* Scratch register index used for the local address handshake (SCRATCH[15], 0xC0010178). */
#define TT_COV_HANDOFF_SCRATCH_INDEX 15U

#endif /* TENSTORRENT_COVERAGE_HANDOFF_H_ */
