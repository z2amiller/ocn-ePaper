// SPDX-License-Identifier: MIT
// Host preview of src/render.c: `make preview DIGITS=1234 VBAT=298 TEMP=17 [STORED=1]` writes build/preview.pbm, the
// panel read landscape, rebuilt from the RAM rows exactly as the firmware sends them. It undoes the same
// mapping the build used, so FLIP=1 previews the same upright picture: the check is that the rows decode
// cleanly and the screen is drawn right, not which way up the real panel is.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/render.h"

#ifndef FLIP
#define FLIP 0
#endif

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "1234";
    if (strlen(s) != 4) {
        fprintf(stderr, "usage: preview HHMM [battery centivolts, 0 for none] [temperature C, none for none] [stored]\n");
        return 1;
    }
    struct screen sc = { .vbat_cv = 298, .temp_c = 17 };
    for (int i = 0; i < 4; i++) sc.d[i] = (uint8_t)(s[i] - '0');
    if (argc > 2) sc.vbat_cv = (uint16_t)atoi(argv[2]);
    if (argc > 3) sc.temp_c = strcmp(argv[3], "none") ? (int8_t)atoi(argv[3]) : TEMP_NONE;
    if (argc > 4) sc.stored = strcmp(argv[4], "stored") == 0;

    static uint8_t img[EPD_VISIBLE][EPD_ROWS];      // landscape: [y][x], 1 = black
    for (int row = 0; row < EPD_ROWS; row++) {
        uint8_t buf[EPD_ROW_BYTES];
        render_row(&sc, (uint8_t)row, buf);
        for (int p = 0; p < EPD_ROW_BYTES * 8; p++) {
            int white = (buf[p >> 3] >> (7 - (p & 7))) & 1;
            if (p >= EPD_VISIBLE) {
                if (!white) { fprintf(stderr, "row %d: black bit past the visible edge (%d)\n", row, p); return 1; }
                continue;
            }
            // The mapping render.c assumes, undone: FLIP=0 puts row 0 at the right and pixel 0 at the top.
            int x = FLIP ? row : EPD_ROWS - 1 - row;
            int y = FLIP ? EPD_VISIBLE - 1 - p : p;
            img[y][x] = (uint8_t)!white;
        }
    }
    if (sc.stored) {                                // the stored screen reads portrait: landscape turned 90 degrees CW
        printf("P1\n%d %d\n", EPD_VISIBLE, EPD_ROWS);
        for (int py = 0; py < EPD_ROWS; py++) {
            for (int px = 0; px < EPD_VISIBLE; px++) putchar(img[EPD_VISIBLE - 1 - px][py] ? '1' : '0');
            putchar('\n');
        }
        return 0;
    }
    printf("P1\n%d %d\n", EPD_ROWS, EPD_VISIBLE);
    for (int y = 0; y < EPD_VISIBLE; y++) {
        for (int x = 0; x < EPD_ROWS; x++) putchar(img[y][x] ? '1' : '0');
        putchar('\n');
    }
    return 0;
}
