// SPDX-License-Identifier: MIT
// HH:MM as four bevelled seven-segment digits and a colon, on the panel read landscape: 250 pixels across,
// 122 down. The panel's RAM runs the other way (rows of 122 pixels, 250 rows), so each RAM row is one
// column of the landscape picture.
//
// Which end of the panel is up isn't settled yet. With FLIP=0, RAM row 0 is the right-hand column and a
// row's first pixel is the top; FLIP=1 turns the picture through 180 degrees. Neither mirrors it: the RAM
// order is the one GxEPD2 uses for this panel, which draws text the right way round.
#include <string.h>
#include "render.h"

#ifndef FLIP
#define FLIP 0
#endif

#define W   250             // landscape width (columns)
#define H   EPD_VISIBLE     // landscape height (rows)

// One digit is DW x DH pixels with strokes T thick. The segments' centre lines run between the skeleton
// points (XL or XR, YT or YM or YB); each segment stops GAP short of a corner and is bevelled at 45 degrees.
#define DH   100
#define DW   48
#define T    12
#define HT   (T / 2)
#define GAP  1
#define TOP  ((H - DH) / 2)
#define XL   HT
#define XR   (DW - HT)
#define YT   HT
#define YM   (DH / 2)
#define YB   (DH - HT)

static const uint8_t digit_x[4] = {6, 62, 140, 196};    // left edges; the colon sits between 110 and 140
#define COLON_X  120
#define DOT      10
#define DOT1_Y   (TOP + 25)
#define DOT2_Y   (TOP + 65)

enum { SA = 0x01, SB = 0x02, SC = 0x04, SD = 0x08, SE = 0x10, SF = 0x20, SG = 0x40 };
static const uint8_t segments[10] = {
    SA | SB | SC | SD | SE | SF,       // 0
    SB | SC,                           // 1
    SA | SB | SD | SE | SG,            // 2
    SA | SB | SC | SD | SG,            // 3
    SB | SC | SF | SG,                 // 4
    SA | SC | SD | SF | SG,            // 5
    SA | SC | SD | SE | SF | SG,       // 6
    SA | SB | SC,                      // 7
    SA | SB | SC | SD | SE | SF | SG,  // 8
    SA | SB | SC | SD | SF | SG,       // 9
};

// Blackens landscape rows y0..y1 (inclusive) of the current column.
static void black(uint8_t *out, int16_t y0, int16_t y1)
{
    if (y0 < 0) y0 = 0;
    if (y1 > H - 1) y1 = H - 1;
#if FLIP
    int16_t p0 = H - 1 - y1, p1 = H - 1 - y0;
#else
    int16_t p0 = y0, p1 = y1;
#endif
    for (int16_t p = p0; p <= p1; p++) {
        out[p >> 3] &= (uint8_t)~(0x80u >> (p & 7));
    }
}

static int16_t min3(int16_t a, int16_t b, int16_t c)
{
    int16_t m = a < b ? a : b;
    return m < c ? m : c;
}

// A vertical stroke centred on column xc: at dd columns from its centre it is shortened by dd at each end.
static void vertical(uint8_t *out, int16_t u, int16_t xc, uint8_t upper, uint8_t lower)
{
    if (u < xc - HT || u >= xc + HT) return;
    int16_t dd = u < xc ? xc - 1 - u : u - xc;
    if (upper) black(out, TOP + YT + GAP + dd + 1, TOP + YM - GAP - dd - 1);
    if (lower) black(out, TOP + YM + GAP + dd + 1, TOP + YB - GAP - dd - 1);
}

// Column u (0..DW-1) of a digit.
static void digit_column(uint8_t *out, uint8_t s, int16_t u)
{
    int16_t hh = min3(HT, u - (XL + GAP), (XR - GAP) - u);     // half-thickness of the horizontal strokes here
    if (hh > 0) {
        if (s & SA) black(out, TOP + YT - hh, TOP + YT + hh - 1);
        if (s & SG) black(out, TOP + YM - hh, TOP + YM + hh - 1);
        if (s & SD) black(out, TOP + YB - hh, TOP + YB + hh - 1);
    }
    vertical(out, u, XL, s & SF, s & SE);
    vertical(out, u, XR, s & SB, s & SC);
}

void render_row(const uint8_t d[4], uint8_t row, uint8_t out[EPD_ROW_BYTES])
{
    memset(out, 0xFF, EPD_ROW_BYTES);
#if FLIP
    int16_t x = row;
#else
    int16_t x = W - 1 - row;
#endif
    for (uint8_t i = 0; i < 4; i++) {
        int16_t u = x - digit_x[i];
        if (u >= 0 && u < DW && d[i] < 10) {
            digit_column(out, segments[d[i]], u);
        }
    }
    if (x >= COLON_X && x < COLON_X + DOT) {
        black(out, DOT1_Y, DOT1_Y + DOT - 1);
        black(out, DOT2_Y, DOT2_Y + DOT - 1);
    }
}
