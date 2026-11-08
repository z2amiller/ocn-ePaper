// SPDX-License-Identifier: MIT
// Timer_A0 from ACLK / 8 = 4096 Hz, for waits and timeouts while the CPU sleeps in LPM3. Times are in its
// ticks, 16 s at most.
#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TICKS(ms) ((uint16_t)(((uint32_t)(ms) * 4096u + 999u) / 1000u))

// Set when the time given to timer_start() has run out; its interrupt also ends any LPM3 sleep.
extern volatile uint8_t timer_expired;

void timer_start(uint16_t ticks);
void timer_stop(void);

// Ticks since timer_start().
uint16_t timer_now(void);

// Sleeps in LPM3 for `ticks`. Interrupts must be enabled.
void timer_sleep(uint16_t ticks);

#endif
