// SPDX-License-Identifier: MIT
// Elapsed-time counter: counts minutes from when the magnet comes off and shows HH:MM on the e-paper panel,
// freezing at 99:59. Putting the magnet back stops the count and shows the stored screen.
//
// The chip spends its time in LPM3.5, where only the RTC (from the 32.768 kHz crystal), the 32-byte backup
// memory and the port interrupts keep running. Waking from LPM3.5 is a reset: the program starts again from
// main(), and SYSRSTIV says it was a wake rather than a power-up. RAM doesn't survive, so the state lives in
// backup memory. The panel holds its picture with no power at all, so it sleeps too between updates.
//
// The magnet works the reed switch on P1.0 (WAKE), which reads high while it's closed. The chip stays powered
// either way, so it sees the magnet come and go:
//   - Magnet off: counting. The RTC wakes the chip once a minute. Each minute is a partial update (about
//     0.8 s, 1 mC); after every FULL_EVERY of those comes a full refresh (about 2.3 s), which clears the
//     ghosting partial updates leave. 99:59 is drawn with a full refresh, then the RTC stops.
//   - Magnet on: stored. The count stops where it is and the stored screen shows it, drawn with a full
//     refresh. The RTC then wakes the chip every 34 minutes, the longest it can, and after STORE_WAKES of those
//     (about a week) the stored screen is drawn again: Good Display asks for a full refresh about weekly on a
//     panel that keeps showing the same picture.
//   - Taking the magnet off again starts a new count from 00:00, with a full refresh.
// Port 1 wakes the chip on the WAKE edge that would change the mode. Every wake, whatever caused it, compares
// the pin with the mode, so a missed or bouncing edge can't leave the two out of step; a change counts only
// once the pin reads the same twice, 20 ms apart.
//
// Under the count, a status line shows the battery voltage and the chip's temperature. The temperature is
// read at each wake, just before drawing. The battery is read in the middle of each update, while the panel's
// booster loads it, and shown at the next update; at power-up it's read before the first drawing.
//
// Power-up (fitting the cells, or the programmer's supply coming back) starts afresh from 00:00: counting if
// the magnet is off, stored if it's on.
//
// Build options (see the Makefile): FAST=1 counts once a second and redraws the stored screen every 30 s;
// START=hhmm starts from that count; FLIP=1 turns the picture through 180 degrees; FULL_EVERY=n sets the
// partial updates between full refreshes.
#include <stdint.h>
#include "board.h"
#include "epd.h"
#include "sense.h"
#include "timer.h"

#ifndef FAST
#define FAST 0
#endif
#ifndef START
#define START 0
#endif
#ifndef LOOP
#define LOOP 0
#endif
#ifndef FULL_EVERY
#define FULL_EVERY 5        // Good Display recommends a full refresh after every five partial updates
#endif

// RTC tick = 1024 / 32768 s = 31.25 ms; the interrupt fires every RTCMOD + 1 ticks.
#define COUNT_MOD   (FAST ? 32 - 1 : 60 * 32 - 1)       // a minute (a second in FAST builds)
#define STORE_MOD   (FAST ? 10 * 32 - 1 : 0xFFFF)       // 2,048 s, the longest the RTC can (10 s in FAST builds)
#define STORE_WAKES (FAST ? 3 : 295)                     // stored screen redrawn every 295 x 2,048 s = 6.99 days

// Backup memory (kept in LPM3.5): bytes 0-3 the digits as BCD, tens of hours first; byte 4 the partial
// updates since the last full refresh; byte 5 the mode; 6-7 the latest battery reading; 16-18 the battery and
// temperature the panel shows now, which the next partial update needs as its "old" picture; 20-21 the RTC
// wakes since the stored screen was last drawn. (DIAG builds use 8-15.)
#define BK             ((volatile uint8_t *)BKMEM_BASE)
#define BK_PARTIALS    (BK[4])
#define BK_MODE        (BK[5])
#define BK_VBAT        (*(volatile uint16_t *)(BKMEM_BASE + 6))
#define BK_SHOWN_V     (*(volatile uint16_t *)(BKMEM_BASE + 16))
#define BK_SHOWN_T     (*(volatile int8_t *)(BKMEM_BASE + 18))
#define BK_STORE_WAKES (*(volatile uint16_t *)(BKMEM_BASE + 20))

enum { COUNTING = 0, STORED = 1 };

static volatile uint8_t ticks;          // RTC interrupts not yet handled

// SYSRSTIV holds every reset cause since it was last emptied; each read returns the highest-priority one and
// clears only that. The debugger leaves extras behind (attaching wakes the chip, then resets it through the
// RST pin), so it's emptied at every start-up, and a start-up is a wake only when the LPM3.5 wake is its one
// cause. Anything else starts afresh from 00:00.
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
//   P1.0 WAKE   input, no pull: R3 pulls it down, and the internal pull-down (about 35k) would draw about
//               90 uA whenever the magnet holds it high
//   P1.1        not connected: driven low
//   P1.2 D/C, P1.4 SDA, P1.6 SCLK   low
//   P1.3 CS     high (panel not selected)
//   P1.5 BUSY   input, no pull: the panel drives it, and holds it high in deep sleep, so a pull-down would
//               draw current all the time
//   P1.7 RES    high: low would hold the panel in reset
//   P2.0, P2.1  the crystal (P2SEL = 10)
//   P2.2        not connected, and P2.3-P2.7 not bonded out on this package: driven low
static void gpio_init(void)
{
    P1OUT = CS | RES;
    P1REN = 0;
    P1DIR = (uint8_t)~(WAKE | BUSY);
    P2OUT = 0;
    P2DIR = (uint8_t)~(BIT0 | BIT1);
    P2SEL1 |= BIT0 | BIT1;                       // XOUT/XIN
}

static void clock_init(void)
{
    // The crystal: already running after a wake, typically 1 s to start from power-up. Give up after about
    // 3 s (at the ~1 MHz reset clock) rather than spin on a bad joint and flatten the cells: ACLK then falls
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

// ---------- the count ----------

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

// Runs in the middle of each update: the battery under the booster's load, for the next screen.
static void read_battery(void)
{
    BK_VBAT = sense_vcc_cv();
}

// Remembers the status line the panel now shows.
static void show_status(const struct screen *s)
{
    BK_SHOWN_V = s->vbat_cv;
    BK_SHOWN_T = s->temp_c;
}

// ---------- the RTC and the magnet ----------

// Stops the RTC and drops its interrupts not yet handled, returning how many, including one that has fired but
// hasn't been taken yet. Call with interrupts off.
static uint8_t rtc_halt(void)
{
    uint8_t dropped = ticks;
    if (RTCIV == RTCIV__RTCIFG) dropped++;
    RTCCTL = 0;
    if (RTCIV == RTCIV__RTCIFG) dropped++;       // one that fired as it stopped
    ticks = 0;
    return dropped;
}

static void rtc_stop(void)
{
    __disable_interrupt();
    (void)rtc_halt();
    __enable_interrupt();
}

// Restarts the RTC to interrupt every mod + 1 ticks from now. Returns the interrupts it dropped, as rtc_halt().
static uint8_t rtc_every(uint16_t mod)
{
    __disable_interrupt();
    uint8_t dropped = rtc_halt();
    RTCMOD = mod;
    RTCCTL = RTCSS__XT1CLK | RTCSR | RTCPS__1024 | RTCIE;
    __enable_interrupt();
    return dropped;
}

static uint8_t reed(void)
{
    return (P1IN & WAKE) ? 1 : 0;
}

// Whether the magnet is on, once the reed has settled: the same reading twice, 20 ms apart. Gives up after
// about half a second of chatter and goes with the last reading.
static uint8_t magnet(void)
{
    uint8_t a = reed();
    for (uint8_t i = 0; i < 25; i++) {
        timer_sleep(TICKS(20));
        uint8_t b = reed();
        if (b == a) return a;
        a = b;
    }
    return a;
}

// ---------- the two modes ----------

// Starts a count from START (00:00 unless a test build says otherwise), drawn with a full refresh.
static void start_count(void)
{
    const uint8_t start[4] = {START / 1000 % 10, START / 100 % 10, START / 10 % 10, START % 10};
    save_digits(start);
    BK_PARTIALS = 0;
    BK_MODE = COUNTING;
    if (at_end(start)) {
        rtc_stop();
    } else {
        (void)rtc_every(COUNT_MOD);              // counting starts now, before the refresh
    }
    struct screen now;
    load_digits(now.d);
    now.vbat_cv = BK_VBAT;
    now.temp_c = sense_temp_c();
    now.stored = 0;
    epd_show(&now, &now, 1, read_battery);
    show_status(&now);
}

// One RTC interrupt while counting: the next minute, drawn.
static void count_minute(void)
{
    struct screen now, old;
    load_digits(now.d);
    for (uint8_t i = 0; i < 4; i++) old.d[i] = now.d[i];
    if (!advance(now.d)) {
        rtc_stop();                              // frozen: no more wakes
        return;
    }
    save_digits(now.d);
    old.vbat_cv = BK_SHOWN_V;                    // what the panel shows now, for the partial update
    old.temp_c = BK_SHOWN_T;
    now.vbat_cv = BK_VBAT;
    now.temp_c = sense_temp_c();
    old.stored = now.stored = 0;
    uint8_t full = at_end(now.d) || BK_PARTIALS >= FULL_EVERY;
    BK_PARTIALS = full ? 0 : BK_PARTIALS + 1;
    epd_show(&old, &now, full, read_battery);
    show_status(&now);
    if (at_end(now.d)) rtc_stop();               // 99:59 is on the panel, cleanly: no more wakes
}

static void draw_stored(void)
{
    struct screen s;
    load_digits(s.d);
    s.vbat_cv = BK_VBAT;
    s.temp_c = TEMP_NONE;
    s.stored = 1;
    epd_show(&s, &s, 1, read_battery);
}

// Stops the count where it is and draws the stored screen. Minutes the RTC has already counted but the panel
// hasn't shown yet still count.
static void store(void)
{
    uint8_t pending = rtc_every(STORE_MOD);
    if (BK_MODE != COUNTING) pending = 0;
    uint8_t d[4];
    load_digits(d);
    for (; pending && advance(d); pending--) {
    }
    save_digits(d);
    BK_MODE = STORED;
    BK_STORE_WAKES = 0;
    draw_stored();
}

// One RTC interrupt while stored.
static void stored_wake(void)
{
    if (++BK_STORE_WAKES >= STORE_WAKES) {
        BK_STORE_WAKES = 0;
        draw_stored();
    }
}

// Arms Port 1 for the WAKE edge that would change the mode (falling while stored, rising while counting), then
// sleeps in LPM3.5; the next wake restarts main(). Returns instead if the reed has already moved, or if an
// interrupt arrives as the chip goes to sleep.
//
// The supply supervisor (SVSH) stays on through LPM3.5, at about 0.24 uA. It resets the chip cleanly if the
// cells ever sag below ~1.8 V, rather than leaving the RTC and backup memory running below their minimum
// voltage: without it, the brownout reset is only guaranteed once the supply falls below 0.1 V.
static void sleep_lpm35(uint8_t stored)
{
    __disable_interrupt();
    if (stored) P1IES |= WAKE; else P1IES &= ~WAKE;
    P1IFG &= ~WAKE;                              // after P1IES: changing it can set the flag
    P1IE |= WAKE;
    if (reed() != stored) {                      // it moved before the edge was armed
        P1IE &= ~WAKE;
        __enable_interrupt();
        return;
    }
    PMMCTL0_H = PMMPW_H;
    PMMCTL0_L |= SVSHE | PMMREGOFF;              // regulator off = LPM3.5, supervisor kept on
    PMMCTL0_H = 0;
    __bis_SR_register(LPM3_bits | GIE);
    // Only an interrupt that was already pending (the port's or the RTC's) gets here. Back to the main loop
    // with the regulator on, or the next LPM3 wait would be LPM3.5.
    PMMCTL0_H = PMMPW_H;
    PMMCTL0_L &= ~PMMREGOFF;
    PMMCTL0_H = 0;
    P1IE &= ~WAKE;
}

int main(void)
{
    WDTCTL = WDTPW | WDTHOLD;
    SFRRPCR = SYSRSTRE | SYSRSTUP;               // RST is a reset with its internal pull-up: R1 and C11 can be left off
    uint8_t woke = woke_from_lpm35();

    gpio_init();
    PM5CTL0 &= ~LOCKLPM5;                        // pins take the settings above
    clock_init();

    if (LOOP) {
        // LOOP=1 bench build: full refreshes back to back, 88:88 then 00:00, so a meter can catch the booster's
        // voltages. Never counts; don't leave it on the cells.
        static const struct screen eights = {{8, 8, 8, 8}, 888, 88, 0}, zeros = {{0, 0, 0, 0}, 0, TEMP_NONE, 0};
        __enable_interrupt();
        for (;;) {
            epd_show(&eights, &eights, 1, 0);
            epd_show(&zeros, &zeros, 1, 0);
        }
    }

    if (!woke) {
        const uint8_t start[4] = {START / 1000 % 10, START / 100 % 10, START / 10 % 10, START % 10};
        save_digits(start);                      // what the stored screen shows if the magnet is on already
        BK_MODE = COUNTING;
        BK_VBAT = sense_vcc_cv();                // unloaded, so the first screen has a reading
        __enable_interrupt();
        if (magnet()) {
            store();
        } else {
            start_count();
        }
    } else {
        __enable_interrupt();                    // the RTC interrupt that woke us, if it did, runs now
        __no_operation();
    }

    // Each pass handles one thing: a change of mode, else one RTC interrupt (more than one can be waiting, in
    // FAST builds where a full refresh takes longer than a count), else it goes back to sleep.
    for (;;) {
        uint8_t stored = BK_MODE == STORED;
        if (reed() != stored && magnet() != stored) {
            if (stored) {
                start_count();
            } else {
                store();
            }
            continue;
        }
        __disable_interrupt();
        uint8_t pending = ticks;
        if (pending) ticks = pending - 1;
        __enable_interrupt();
        if (pending) {
            if (stored) {
                stored_wake();
            } else {
                count_minute();
            }
            continue;
        }
        sleep_lpm35(stored);
    }
}

void __attribute__((interrupt(RTC_VECTOR))) rtc_isr(void)
{
    if (RTCIV == RTCIV__RTCIFG) {
        ticks++;
    }
    __bic_SR_register_on_exit(LPM3_bits);        // back to the main loop, even from the way into LPM3.5
}

// Port 1: BUSY falling, which ends epd.c's waits, or WAKE, which is only armed on the way into LPM3.5.
void __attribute__((interrupt(PORT1_VECTOR))) port1_isr(void)
{
    if (P1IV == P1IV__P1IFG0) P1IE &= ~WAKE;
    __bic_SR_register_on_exit(LPM3_bits);
}
