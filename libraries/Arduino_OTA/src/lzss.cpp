/*
 * Copyright (c) Arduino s.r.l. and/or its affiliated companies
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "lzss.h"
#include <string.h>

// Read n bits from the input file. Returns the value, or -1 on EOF.
static int getbits(struct fs_file_t *f, int n, uint32_t *buf, int *buf_size)
{
    while (*buf_size < n) {
        uint8_t c;
        if (fs_read(f, &c, 1) != 1) {
            return -1;
        }
        *buf = (*buf << 8) | c;
        *buf_size += 8;
    }
    int x = *buf >> (*buf_size - n);
    *buf &= (1 << (*buf_size - n)) - 1;
    *buf_size -= n;
    return x;
}

int32_t lzss_decompress(struct fs_file_t *input, struct fs_file_t *output)
{
    uint8_t ring[LZSS_N];
    memset(ring, ' ', sizeof(ring));
    int r = LZSS_N - LZSS_F;

    uint8_t outbuf[512];
    int outpos = 0;
    int32_t total = 0;

    uint32_t bits = 0;
    int nbits = 0;

    for (;;) {
        int flag = getbits(input, 1, &bits, &nbits);
        if (flag < 0) break;

        if (flag) {
            // Literal: 8-bit byte
            int c = getbits(input, 8, &bits, &nbits);
            if (c < 0) break;

            outbuf[outpos++] = (uint8_t)c;
            ring[r] = (uint8_t)c;
            r = (r + 1) & LZSS_N_MASK;

            if (outpos >= (int)sizeof(outbuf)) {
                if (fs_write(output, outbuf, outpos) != outpos) return -1;
                total += outpos;
                outpos = 0;
            }
        } else {
            // Match: EI-bit position + EJ-bit length
            int i = getbits(input, LZSS_EI, &bits, &nbits);
            if (i < 0) break;
            int j = getbits(input, LZSS_EJ, &bits, &nbits);
            if (j < 0) break;

            for (int k = 0; k <= j + 1; k++) {
                uint8_t c = ring[(i + k) & LZSS_N_MASK];
                outbuf[outpos++] = c;
                ring[r] = c;
                r = (r + 1) & LZSS_N_MASK;

                if (outpos >= (int)sizeof(outbuf)) {
                    if (fs_write(output, outbuf, outpos) != outpos) return -1;
                    total += outpos;
                    outpos = 0;
                }
            }
        }
    }

    // Flush remaining
    if (outpos > 0) {
        if (fs_write(output, outbuf, outpos) != outpos) return -1;
        total += outpos;
    }

    return total;
}
