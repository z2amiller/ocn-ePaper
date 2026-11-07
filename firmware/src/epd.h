// SPDX-License-Identifier: MIT
// Good Display GDEY0213B74 (SSD1680 controller) on the ocn-ePaper board.
#ifndef EPD_H
#define EPD_H

#include <stdint.h>

// Wakes the panel with a hardware reset, sets it up, draws `now` and puts it back into deep sleep. A partial
// update (full = 0) changes only what differs from `old` and takes about half a second; a full refresh
// (full = 1) flashes the whole panel and takes about 3.5 s, and clears any ghosting. Sleeps in LPM3 while the
// panel works, so interrupts must be enabled. Returns 0 if the panel never reported done (BUSY stuck high,
// as when no panel is connected).
uint8_t epd_show(const uint8_t old[4], const uint8_t now[4], uint8_t full);

#endif
