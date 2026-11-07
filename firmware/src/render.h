// SPDX-License-Identifier: MIT
// Draws HH:MM in seven-segment digits, one panel RAM row at a time. The whole 250 x 122 frame is 3.8 KB and
// the FR2111 has 1 KB of RAM, so nothing is buffered: each row is computed as it's sent. Plain C with no
// MSP430 headers, so tools/preview.c can build it on the host.
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

#define EPD_ROWS      250   // RAM rows (gate lines): the panel's long side
#define EPD_ROW_BYTES 16    // 128 bits per row, of which the first 122 (the short side) are visible
#define EPD_VISIBLE   122

// d[0..3]: the four digits, tens of hours first. Fills out[] for RAM row `row` (0 = first row written):
// bit 7 of out[0] is pixel 0, and a 1 bit is white.
void render_row(const uint8_t d[4], uint8_t row, uint8_t out[EPD_ROW_BYTES]);

#endif
