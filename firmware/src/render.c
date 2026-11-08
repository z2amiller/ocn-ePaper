// SPDX-License-Identifier: MIT
// Two screens.
//
// The counting screen, read landscape: 250 pixels across, 122 down, the MAGNET DISABLE end on the right.
// HH:MM fills the top as four bevelled seven-segment digits and a colon; a status line along the bottom shows a
// battery gauge and its voltage on the left and the temperature on the right, in a 5 x 7 font drawn at twice
// size. The status line also says which way up the panel is, which 00:00 alone doesn't.
//
// The stored screen, read portrait: 122 across, 250 down, the MAGNET DISABLE end at the bottom, for a unit
// stored upright. The stopped time small across the top, a placeholder for a graphic, and the battery gauge
// and voltage at the bottom.
//
// The panel's RAM is portrait: rows of 122 pixels, 250 rows. So each RAM row is one column of the landscape
// picture, and one row of the portrait one. With FLIP=0, RAM row 0 is the landscape picture's right-hand
// column (the portrait picture's bottom row) and a row's first pixel is the landscape top (the portrait right);
// FLIP=1 turns both pictures through 180 degrees. Neither mirrors them: the RAM order is the one GxEPD2 uses
// for this panel.
#include <string.h>
#include "render.h"

#ifndef FLIP
#define FLIP 0
#endif

#define W   250             // landscape width (columns), portrait height (rows)
#define H   EPD_VISIBLE     // landscape height (rows), portrait width (columns)

// HH:MM. A digit is dw x dh pixels with strokes ht * 2 thick, from row `top`. The segments' centre lines run
// between the skeleton points (ht or dw - ht across; ht, dh / 2 or dh - ht down); each segment stops GAP short
// of a corner and is bevelled at 45 degrees. The colon is two dot x dot squares from column colon_x.
#define GAP  1
struct geom {
    uint8_t top, dh, dw, ht;
    uint8_t x[4];           // left edges of the four digits
    uint8_t colon_x, dot, dot1_y, dot2_y;
};
static const struct geom big = {6, 80, 40, 5, {27, 73, 137, 183}, 121, 8, 26, 58};    // landscape

// The status line: 5 x 7 glyphs at S times size.
#define S         2
#define STATUS_Y  98        // landscape: the status line's top row
#define BAT_X     27        // battery gauge: 26 x 14, then its terminal
#define VOLT_X    60        // the voltage, left-aligned
#define TEMP_END  223       // the temperature, right-aligned to here

// The stored screen, in portrait coordinates.
static const struct geom small = {10, 40, 22, 3, {7, 33, 67, 93}, 59, 4, 22, 38};
#define PH_X0   6           // the placeholder for a graphic: an outlined, lightly hatched block
#define PH_X1   115
#define PH_Y0   64
#define PH_Y1   200
#define P_STAT_Y  218       // battery gauge and voltage, centred
#define P_BAT_X   18
#define P_VOLT_X  52

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

// Blackens pixels a..b (inclusive) of the current RAM row: landscape rows of the current column, or portrait
// columns of the current row.
static void black(uint8_t *out, int16_t a, int16_t b)
{
    if (a < 0) a = 0;
    if (b > H - 1) b = H - 1;
#if FLIP
    int16_t p0 = H - 1 - b, p1 = H - 1 - a;
#else
    int16_t p0 = a, p1 = b;
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

// ---------- the counting screen, a landscape column at a time ----------

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

// How many `unit`s fit in *v, taking them off: digits without pulling in the library's division routines.
static uint8_t take(uint16_t *v, uint16_t unit)
{
    uint8_t q = 0;
    while (*v >= unit) { *v -= unit; q++; }
    return q;
}

// The voltage as glyphs, e.g. 2.98V.
static void volts(uint16_t cv, uint8_t g[5])
{
    g[0] = take(&cv, 100);
    g[1] = G_DOT;
    g[2] = take(&cv, 10);
    g[3] = (uint8_t)cv;
    g[4] = G_V;
}

// Two CR2032s in parallel, read under the booster's load: fresh cells read about 2.8-2.9 V there, and their
// resistance, which climbs with age and cold, pulls it down from there. Below about 2.4 V an update comes
// close to the panel's 2.2 V minimum, so no bars means replace the cells.
static uint8_t bars(uint16_t cv)
{
    return cv >= 280 ? 4 : cv >= 265 ? 3 : cv >= 250 ? 2 : cv >= 240 ? 1 : 0;
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
    uint8_t n = bars(cv);
    for (int16_t b0 = 3; n--; b0 += 5) {                                // bars 4 wide, 1 apart, from u = 3
        if (u >= b0 && u < b0 + 4) black(out, STATUS_Y + 3, STATUS_Y + 10);
    }
}

static void counting_column(const struct screen *s, int16_t x, uint8_t *out)
{
    for (uint8_t i = 0; i < 4; i++) {
        int16_t u = x - big.x[i];
        if (u >= 0 && u < big.dw && s->d[i] < 10) digit_column(out, &big, segments[s->d[i]], u);
    }
    if (x >= big.colon_x && x < big.colon_x + big.dot) {
        black(out, big.dot1_y, big.dot1_y + big.dot - 1);
        black(out, big.dot2_y, big.dot2_y + big.dot - 1);
    }
    if (s->vbat_cv) {
        uint8_t g[5];
        volts(s->vbat_cv > 999 ? 999 : s->vbat_cv, g);
        battery(out, s->vbat_cv, x);
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

// ---------- the stored screen, a portrait row at a time ----------
// The same strokes as above, transposed: a horizontal stroke across one row, a vertical stroke's slice of it.
// Read with the MAGNET DISABLE end at the bottom, portrait row y is landscape column y and portrait column c is
// landscape row H - 1 - c: the counting screen's picture turned 90 degrees clockwise.
static void sblack(uint8_t *out, int16_t a, int16_t b)
{
    black(out, H - 1 - b, H - 1 - a);
}


// Row v (0..dh-1) of a digit whose left edge is column x0.
static void digit_row(uint8_t *out, const struct geom *g, uint8_t s, int16_t x0, int16_t v)
{
    int16_t ht = g->ht, xr = g->dw - ht, ym = g->dh >> 1, yb = g->dh - ht;
    const int16_t yc[3] = {ht, ym, yb};
    const uint8_t seg[3] = {SA, SG, SD};
    for (uint8_t i = 0; i < 3; i++) {                           // horizontal strokes: d rows off the centre line
        if ((s & seg[i]) && v >= yc[i] - ht && v < yc[i] + ht) {
            int16_t d = v < yc[i] ? yc[i] - 1 - v : v - yc[i];
            sblack(out, x0 + ht + GAP + d + 1, x0 + xr - GAP - d - 1);
        }
    }
    int16_t up = min3(ht, v - (ht + GAP), (ym - GAP) - v);     // vertical strokes: their half-width on this row
    int16_t lo = min3(ht, v - (ym + GAP), (yb - GAP) - v);
    if (up > 0) {
        if (s & SF) sblack(out, x0 + ht - up, x0 + ht + up - 1);
        if (s & SB) sblack(out, x0 + xr - up, x0 + xr + up - 1);
    }
    if (lo > 0) {
        if (s & SE) sblack(out, x0 + ht - lo, x0 + ht + lo - 1);
        if (s & SC) sblack(out, x0 + xr - lo, x0 + xr + lo - 1);
    }
}

// Row v (0..13) of the glyphs g[0..n) set from column x0.
static void text_row(uint8_t *out, const uint8_t *g, uint8_t n, int16_t x0, int16_t v)
{
    uint8_t b = (uint8_t)(v / S);
    for (uint8_t i = 0; i < n; i++) {
        for (uint8_t c = 0; c < font_w[g[i]]; c++) {
            if (font[g[i]][c] & (1u << b)) sblack(out, x0 + c * S, x0 + c * S + S - 1);
        }
        x0 += (font_w[g[i]] + 1) * S;
    }
}

// Row v (0..13) of the battery gauge with its left edge at column x0.
static void battery_row(uint8_t *out, uint16_t cv, int16_t x0, int16_t v)
{
    if (v >= 4 && v <= 9) sblack(out, x0 + 26, x0 + 27);         // terminal
    if (v < 2 || v >= 12) { sblack(out, x0, x0 + 25); return; }
    sblack(out, x0, x0 + 1);
    sblack(out, x0 + 24, x0 + 25);
    if (v >= 3 && v <= 10) {
        uint8_t n = bars(cv);
        for (int16_t b0 = 3; n--; b0 += 5) sblack(out, x0 + b0, x0 + b0 + 3);
    }
}

static void stored_row(const struct screen *s, int16_t y, uint8_t *out)
{
    int16_t v = y - small.top;
    if (v >= 0 && v < small.dh) {
        for (uint8_t i = 0; i < 4; i++) {
            if (s->d[i] < 10) digit_row(out, &small, segments[s->d[i]], small.x[i], v);
        }
    }
    if ((y >= small.dot1_y && y < small.dot1_y + small.dot) || (y >= small.dot2_y && y < small.dot2_y + small.dot)) {
        sblack(out, small.colon_x, small.colon_x + small.dot - 1);
    }
    if (y >= PH_Y0 && y <= PH_Y1) {                             // placeholder: two-pixel outline, 45-degree hatching
        if (y < PH_Y0 + 2 || y > PH_Y1 - 2) {
            sblack(out, PH_X0, PH_X1);
        } else {
            sblack(out, PH_X0, PH_X0 + 1);
            sblack(out, PH_X1 - 1, PH_X1);
            for (int16_t x = PH_X0 + 2; x < PH_X1 - 1; x++) {
                if (((x + y) & 15) == 0) sblack(out, x, x);
            }
        }
    }
    v = y - P_STAT_Y;
    if (s->vbat_cv && v >= 0 && v < 7 * S) {
        uint8_t g[5];
        volts(s->vbat_cv > 999 ? 999 : s->vbat_cv, g);
        battery_row(out, s->vbat_cv, P_BAT_X, v);
        text_row(out, g, 5, P_VOLT_X, v);
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
    if (s->stored) {
        stored_row(s, x, out);                                  // portrait row = landscape column
    } else {
        counting_column(s, x, out);
    }
}
