// SPDX-License-Identifier: MIT
// Elapsed-time counter: counts minutes from power-up and shows HH:MM on the e-paper panel, freezing at 99:59.
//
// The chip spends its time in LPM3.5, where only the RTC (from the 32.768 kHz crystal) and the 32-byte backup
// memory keep running. The RTC wakes it once a minute. Waking from LPM3.5 is a reset: the program starts
// again from main(), and SYSRSTIV says it was a wake rather than a power-up. RAM doesn't survive, so the count
// lives in backup memory. The panel holds its picture with no power at all, so it sleeps too between updates.
//
// Each minute is a partial update (about half a second at ~3.5 mA); after every FULL_EVERY of those comes a
// full refresh (about 3.5 s), which clears the ghosting partial updates leave. 99:59 is drawn with a full
// refresh, then the RTC stops and nothing wakes the chip again.
//
// Power-up (the magnet coming off the board, or the programmer's supply coming back) starts from 00:00 with a
// full refresh. When the magnet goes back on, the board stops a few seconds later and the panel keeps the last
// count.
//
// Build options (see the Makefile): FAST=1 counts once a second; START=hhmm starts from that count; FLIP=1
// turns the picture through 180 degrees; FULL_EVERY=n sets the partial updates between full refreshes.
#include <msp430.h>
#include <stdint.h>
#include "epd.h"

#ifndef FAST
#define FAST 0
#endif
#ifndef START
#define START 0
#endif
#ifndef FULL_EVERY
#define FULL_EVERY 5        // Good Display recommends a full refresh after every five partial updates
#endif

// RTC tick = 1024 / 32768 s = 31.25 ms; the interrupt fires every RTCMOD + 1 ticks.
#define TICKS_PER_COUNT (FAST ? 32 : 60 * 32)

// Backup memory (kept in LPM3.5): bytes 0-3 the digits as BCD, tens of hours first; byte 4 the partial
// updates since the last full refresh.
#define BK          ((volatile uint8_t *)BKMEM_BASE)
#define BK_PARTIALS (BK[4])

static volatile uint8_t ticks;          // RTC interrupts not yet counted

// SYSRSTIV holds every reset cause since it was last emptied; each read returns the highest-priority one and
// clears only that. The debugger leaves extras behind (attaching wakes the chip, then resets it through the
// RST pin), so it's emptied at every start-up, and a start-up is a wake only when the LPM3.5 wake is its one
// cause. Anything else starts over from 00:00.
static uint8_t woke_from_lpm35(void)
{
    uint8_t wake = 0, other = 0;
    for (uint8_t i = 0; i < 32; i++) {
        uint16_t iv = SYSRSTIV;
        if (iv == SYSRSTIV__NONE) break;
        if (iv == SYSRSTIV__LPM5WU) wake = 1; else other = 1;
    }
    return wake && !other;
}

// Every pin gets a defined state; these are also the states held through LPM3.5.
//   P1.0, P1.1  not connected: driven low
//   P1.2 RX     input with pull-up: J1's serial line idles high when a programmer is wired to it
//   P1.3 TX     driven low
//   P1.4 CS     high (panel not selected)
//   P1.5 SCLK, P1.6 D/C, P1.7 SDA   low
//   P2.0 RES    high: low would hold the panel in reset
//   P2.1 BUSY   input, no pull: the panel drives it, and holds it high in deep sleep, so a pull-down would
//               draw current all the time
//   P2.6, P2.7  the crystal
//   P2.2-P2.5   not bonded out on this package: driven low
static void gpio_init(void)
{
    P1OUT = BIT4 | BIT2;
    P1REN = BIT2;
    P1DIR = (uint8_t)~BIT2;
    P2OUT = BIT0;
    P2DIR = (uint8_t)~(BIT1 | BIT6 | BIT7);
    P2SEL1 |= BIT6 | BIT7;                       // XIN/XOUT
}

static void clock_init(void)
{
    // The crystal: already running after a wake, typically 1 s to start from power-up. Give up after about
    // 3 s (at the ~1 MHz reset clock) rather than spin on a bad joint and flatten the cell: ACLK then falls
    // back to REFO by itself, the panel still shows 00:00, and the RTC never ticks, so it never counts.
    for (uint16_t i = 0; i < 3000; i++) {
        CSCTL7 &= ~(XT1OFFG | DCOFFG);
        SFRIFG1 &= ~OFIFG;
        if (!(SFRIFG1 & OFIFG)) break;
        __delay_cycles(1000);
    }

    FRCTL0 = FRCTLPW | NWAITS_1;                 // one FRAM wait state: safe even if the DCO overshoots 8 MHz
    __bis_SR_register(SCG0);                     // FLL off while it's set up
    CSCTL3 = SELREF__XT1CLK;                     // locked to the crystal, so REFO stays off
    CSCTL0 = 0x100;                              // DCO tap mid-range, as TI's trim routine starts it
    CSCTL1 = DCOFTRIMEN_1 | DCOFTRIM0 | DCOFTRIM1 | DCORSEL_3;   // DCOFTRIM = 3 as in TI's examples, 8 MHz range
    CSCTL2 = FLLD_0 + 243;                       // 244 x 32768 Hz = 8.0 MHz
    __delay_cycles(3);
    __bic_SR_register(SCG0);                     // no waiting for lock (~250 ms): nothing here needs an exact clock
    CSCTL4 = SELMS__DCOCLKDIV | SELA__XT1CLK;    // MCLK = SMCLK = ~8 MHz, ACLK = 32.768 kHz
}

static void load_digits(uint8_t d[4])
{
    for (uint8_t i = 0; i < 4; i++) d[i] = BK[i];
}

static void save_digits(const uint8_t d[4])
{
    for (uint8_t i = 0; i < 4; i++) BK[i] = d[i];
}

static uint8_t at_end(const uint8_t d[4])
{
    return d[0] == 9 && d[1] == 9 && d[2] == 5 && d[3] == 9;
}

// Advances one minute. Returns 0, leaving the digits alone, once they read 99:59 (or anything invalid).
static uint8_t advance(uint8_t d[4])
{
    if (at_end(d) || d[0] > 9 || d[1] > 9 || d[2] > 5 || d[3] > 9) return 0;
    if (++d[3] < 10) return 1;
    d[3] = 0;
    if (++d[2] < 6) return 1;
    d[2] = 0;
    if (++d[1] < 10) return 1;
    d[1] = 0;
    ++d[0];
    return 1;
}

// The supply supervisor (SVSH) stays on through LPM3.5, at about 0.24 uA. It is what makes the magnet a clean
// reset: once the hold-up capacitors fall below ~1.8 V it resets the chip, so taking the magnet off powers up
// from 00:00. Without it the brownout reset is only guaranteed if the supply falls below 0.1 V first (FR2111
// datasheet, safe BOR power-down level), and a magnet held on for tens of seconds could leave the chip running
// on from a backup memory that went below its minimum voltage.
static void sleep_lpm35(void)
{
    __disable_interrupt();
    PMMCTL0_H = PMMPW_H;
    PMMCTL0_L |= SVSHE | PMMREGOFF;              // regulator off = LPM3.5, supervisor kept on
    PMMCTL0_H = 0;
    __bis_SR_register(LPM3_bits | GIE);
}

int main(void)
{
    WDTCTL = WDTPW | WDTHOLD;
    uint8_t woke = woke_from_lpm35();

    gpio_init();
    PM5CTL0 &= ~LOCKLPM5;                        // pins take the settings above
    clock_init();

    uint8_t d[4], old[4];
    if (!woke) {
        const uint8_t start[4] = {START / 1000 % 10, START / 100 % 10, START / 10 % 10, START % 10};
        save_digits(start);
        BK_PARTIALS = 0;
        if (!at_end(start)) {
            RTCMOD = TICKS_PER_COUNT - 1;        // counting starts now, before the 3.5 s first refresh
            RTCCTL = RTCSS__XT1CLK | RTCSR | RTCPS__1024 | RTCIE;
        }
        __enable_interrupt();
        epd_show(start, start, 1);
    } else {
        __enable_interrupt();                    // the RTC interrupt that woke us runs now
        __no_operation();
    }

    // One count per RTC interrupt, including any that arrived during an update (only in FAST builds, where
    // a full refresh takes longer than a count).
    for (;;) {
        __disable_interrupt();
        uint8_t pending = ticks;
        if (pending) ticks = pending - 1;
        __enable_interrupt();
        if (!pending) break;

        load_digits(d);
        for (uint8_t i = 0; i < 4; i++) old[i] = d[i];
        if (!advance(d)) {
            RTCCTL = 0;                          // frozen: no more wakes
            break;
        }
        save_digits(d);
        uint8_t full = at_end(d) || BK_PARTIALS >= FULL_EVERY;
        BK_PARTIALS = full ? 0 : BK_PARTIALS + 1;
        epd_show(old, d, full);
        if (at_end(d)) {
            RTCCTL = 0;                          // 99:59 is on the panel, cleanly: no more wakes
            break;
        }
    }
    sleep_lpm35();                               // doesn't return: the next wake restarts main()
    for (;;) {
    }
}

void __attribute__((interrupt(RTC_VECTOR))) rtc_isr(void)
{
    if (RTCIV == RTCIV__RTCIFG) {
        ticks++;
    }
}
