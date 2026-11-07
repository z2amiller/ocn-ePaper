// SPDX-License-Identifier: MIT
// The screen, read landscape: 250 pixels across, 122 down. HH:MM fills the top as four bevelled
// seven-segment digits and a colon; a status line along the bottom shows a battery gauge and its voltage on
// the left and the temperature on the right, in a 5 x 7 font drawn at twice size. The status line also says
// which way up the panel is, which 00:00 alone doesn't.
//
// The panel's RAM runs the other way (rows of 122 pixels, 250 rows), so each RAM row is one column of the
// landscape picture. With FLIP=0, RAM row 0 is the right-hand column and a row's first pixel is the top;
// FLIP=1 turns the picture through 180 degrees. Neither mirrors it: the RAM order is the one GxEPD2 uses for
// this panel.
#include <string.h>
#include "render.h"

#ifndef FLIP
#define FLIP 0
#endif
#ifndef STORED_SCREEN
#define STORED_SCREEN 1     // the stored screen; the counting firmware builds without it (see the Makefile)
#endif

#define W   250             // landscape width (columns)
#define H   EPD_VISIBLE     // landscape height (rows)

// HH:MM in one of two sizes: large on the counting screen, small on the stored screen. A digit is dw x dh
// pixels with strokes ht * 2 thick, from row top. The segments' centre lines run between the skeleton points
// (ht or dw - ht across; ht, dh / 2 or dh - ht down); each segment stops GAP short of a corner and is
// bevelled at 45 degrees. The colon is two dot x dot squares from column colon_x.
#define GAP  1
struct geom {
    uint8_t top, dh, dw, ht;
    uint8_t x[4];           // left edges of the four digits
    uint8_t colon_x, dot, dot1_y, dot2_y;
};
static const struct geom big = {6, 80, 40, 5, {27, 73, 137, 183}, 121, 8, 26, 58};
#if STORED_SCREEN
static const struct geom small = {4, 36, 20, 3, {74, 100, 130, 156}, 123, 4, 13, 27};

#endif

// The stored screen's placeholder for a graphic: an outlined, lightly hatched band.
#define PH_X0  6
#define PH_X1  243
#define PH_Y0  46
#define PH_Y1  91

// The status line: 5 x 7 glyphs at S times size, from row STATUS_Y.
#define S         2
#define STATUS_Y  98
#define BAT_X     27        // battery gauge: 26 x 14, then its terminal
#define VOLT_X    60        // the voltage, left-aligned
#define TEMP_END  223       // the temperature, right-aligned to here

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

// 5 x 7 glyphs, a byte per column, bit 0 the top row.
enum { G_DOT = 10, G_V, G_C, G_DEG, G_MINUS, G_COUNT };
static const uint8_t font[G_COUNT][5] = {
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00}, {0x62, 0x51, 0x49, 0x49, 0x46},
    {0x22, 0x41, 0x49, 0x49, 0x36}, {0x18, 0x14, 0x12, 0x7F, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1E},
    {0x60, 0x60},                         // .
    {0x07, 0x18, 0x60, 0x18, 0x07},       // V
    {0x3E, 0x41, 0x41, 0x41, 0x22},       // C
    {0x06, 0x09, 0x09, 0x06},             // degree sign
    {0x08, 0x08, 0x08, 0x08, 0x08},       // -
};
static const uint8_t font_w[G_COUNT] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 2, 5, 5, 4, 5};

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
static void vertical(uint8_t *out, const struct geom *g, int16_t u, int16_t xc, uint8_t upper, uint8_t lower)
{
    int16_t ht = g->ht, yt = g->top + ht, ym = g->top + (g->dh >> 1), yb = g->top + g->dh - ht;
    if (u < xc - ht || u >= xc + ht) return;
    int16_t dd = u < xc ? xc - 1 - u : u - xc;
    if (upper) black(out, yt + GAP + dd + 1, ym - GAP - dd - 1);
    if (lower) black(out, ym + GAP + dd + 1, yb - GAP - dd - 1);
}

// Column u (0..dw-1) of a digit.
static void digit_column(uint8_t *out, const struct geom *g, uint8_t s, int16_t u)
{
    int16_t ht = g->ht, xr = g->dw - ht;
    int16_t yt = g->top + ht, ym = g->top + (g->dh >> 1), yb = g->top + g->dh - ht;
    int16_t hh = min3(ht, u - (ht + GAP), (xr - GAP) - u);     // half-thickness of the horizontal strokes here
    if (hh > 0) {
        if (s & SA) black(out, yt - hh, yt + hh - 1);
        if (s & SG) black(out, ym - hh, ym + hh - 1);
        if (s & SD) black(out, yb - hh, yb + hh - 1);
    }
    vertical(out, g, u, ht, s & SF, s & SE);
    vertical(out, g, u, xr, s & SB, s & SC);
}

// Column x of HH:MM drawn with geometry g.
static void clock_column(uint8_t *out, const struct geom *g, const uint8_t d[4], int16_t x)
{
    for (uint8_t i = 0; i < 4; i++) {
        int16_t u = x - g->x[i];
        if (u >= 0 && u < g->dw && d[i] < 10) digit_column(out, g, segments[d[i]], u);
    }
    if (x >= g->colon_x && x < g->colon_x + g->dot) {
        black(out, g->dot1_y, g->dot1_y + g->dot - 1);
        black(out, g->dot2_y, g->dot2_y + g->dot - 1);
    }
}

#if STORED_SCREEN
// Column x of the stored screen's placeholder: a two-pixel outline, hatched at 45 degrees inside.
static void placeholder(uint8_t *out, int16_t x)
{
    if (x < PH_X0 || x > PH_X1) return;
    if (x < PH_X0 + 2 || x > PH_X1 - 2) { black(out, PH_Y0, PH_Y1); return; }
    black(out, PH_Y0, PH_Y0 + 1);
    black(out, PH_Y1 - 1, PH_Y1);
    for (int16_t y = PH_Y0 + 2; y < PH_Y1 - 1; y++) {
        if (((x + y) & 15) == 0) black(out, y, y);
    }
}
#endif

// How many `unit`s fit in *v, taking them off: digits without pulling in the library's division routines.
static uint8_t take(uint16_t *v, uint16_t unit)
{
    uint8_t q = 0;
    while (*v >= unit) { *v -= unit; q++; }
    return q;
}

static int16_t text_width(const uint8_t *g, uint8_t n)
{
    int16_t w = 0;
    for (uint8_t i = 0; i < n; i++) w += (font_w[g[i]] + 1) * S;
    return w - S;
}

// Column x of the glyphs g[0..n) set from column x0 on the status line.
static void text(uint8_t *out, const uint8_t *g, uint8_t n, int16_t x0, int16_t x)
{
    for (uint8_t i = 0; i < n && x >= x0; i++) {
        int16_t w = font_w[g[i]] * S;
        if (x < x0 + w) {
            uint8_t bits = font[g[i]][(x - x0) / S];
            for (uint8_t b = 0; b < 7; b++) {
                if (bits & (1u << b)) black(out, STATUS_Y + b * S, STATUS_Y + b * S + S - 1);
            }
            return;
        }
        x0 += w + S;
    }
}

// The battery gauge: a 26 x 14 outline two pixels thick, its terminal, and up to four bars inside.
static void battery(uint8_t *out, uint16_t cv, int16_t x)
{
    int16_t u = x - BAT_X;
    if (u < 0 || u > 27) return;
    if (u >= 26) { black(out, STATUS_Y + 4, STATUS_Y + 9); return; }   // terminal
    if (u < 2 || u >= 24) { black(out, STATUS_Y, STATUS_Y + 13); return; }
    black(out, STATUS_Y, STATUS_Y + 1);
    black(out, STATUS_Y + 12, STATUS_Y + 13);
    uint8_t bars = cv >= 290 ? 4 : cv >= 270 ? 3 : cv >= 250 ? 2 : cv >= 230 ? 1 : 0;
    for (int16_t b0 = 3; bars--; b0 += 5) {                             // bars 4 wide, 1 apart, from u = 3
        if (u >= b0 && u < b0 + 4) black(out, STATUS_Y + 3, STATUS_Y + 10);
    }
}

void render_row(const struct screen *s, uint8_t row, uint8_t out[EPD_ROW_BYTES])
{
    memset(out, 0xFF, EPD_ROW_BYTES);
#if FLIP
    int16_t x = row;
#else
    int16_t x = W - 1 - row;
#endif
#if STORED_SCREEN
    if (s->stored) {                             // magnet on: the stopped time small, a graphic under it
        clock_column(out, &small, s->d, x);
        placeholder(out, x);
    } else
#endif
    {
        clock_column(out, &big, s->d, x);
    }
    if (s->vbat_cv) {
        uint16_t cv = s->vbat_cv > 999 ? 999 : s->vbat_cv, v = cv;
        uint8_t g[5];
        g[0] = take(&v, 100);
        g[1] = G_DOT;
        g[2] = take(&v, 10);
        g[3] = (uint8_t)v;
        g[4] = G_V;
        battery(out, cv, x);
        text(out, g, 5, VOLT_X, x);
    }
    if (s->temp_c != TEMP_NONE) {
        uint8_t g[5], n = 0;
        uint16_t t = (uint16_t)(s->temp_c < 0 ? -s->temp_c : s->temp_c);
        if (s->temp_c < 0) g[n++] = G_MINUS;
        if (t >= 10) g[n++] = take(&t, 10);
        g[n++] = (uint8_t)t;
        g[n++] = G_DEG;
        g[n++] = G_C;
        text(out, g, n, TEMP_END - text_width(g, n), x);
    }
}
