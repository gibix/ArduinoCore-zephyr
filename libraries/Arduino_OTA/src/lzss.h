/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/fs/fs.h>
#include <stdint.h>

// LZSS parameters matching the Arduino OTA format
#define LZSS_EI      11
#define LZSS_EJ      4
#define LZSS_N       (1 << LZSS_EI)        // 2048
#define LZSS_F       ((1 << LZSS_EJ) + 1)  // 17
#define LZSS_N_MASK  (LZSS_N - 1)

// Decompress LZSS data from input file to output file.
// Input file should be positioned at the start of compressed data.
// Returns decompressed size on success, negative on error.
int32_t lzss_decompress(struct fs_file_t *input, struct fs_file_t *output);
