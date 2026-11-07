// SPDX-License-Identifier: MIT
// Draws the screen one panel RAM row at a time: HH:MM in seven-segment digits, and under them a status line
// with the battery and the temperature. The whole 250 x 122 frame is 3.8 KB and the FR2111 has 1 KB of RAM,
// so nothing is buffered: each row is computed as it's sent. Plain C with no MSP430 headers, so
// tools/preview.c can build it on the host.
#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>

#define EPD_ROWS      250   // RAM rows (gate lines): the panel's long side
#define EPD_ROW_BYTES 16    // 128 bits per row, of which the first 122 (the short side) are visible
#define EPD_VISIBLE   122

#define TEMP_NONE (-128)    // temp_c value that leaves the temperature off

struct screen {
    uint8_t d[4];           // the count, tens of hours first
    uint16_t vbat_cv;       // battery in hundredths of a volt; 0 leaves the battery off
    int8_t temp_c;          // whole degrees C, or TEMP_NONE
    uint8_t stored;         // 1: the stored screen (time small at the top, a graphic, the status line)
};

// Fills out[] for RAM row `row` (0 = first row written): bit 7 of out[0] is pixel 0, and a 1 bit is white.
void render_row(const struct screen *s, uint8_t row, uint8_t out[EPD_ROW_BYTES]);

#endif
