// SPDX-License-Identifier: MIT
// Good Display GDEY0213B74 driver: SSD1680 over 4-wire SPI, write-only.
//
// Wiring (board netlist): P1.4 CS, P1.5 SCLK (UCA0CLK), P1.6 D/C, P1.7 SDA (UCA0SIMO), P2.0 RES, P2.1 BUSY.
// SPI uses eUSCI_A0 on its default pins (SYSCFG3.USCIARMP = 0); the board's serial port shares the block on
// the remapped pins P1.2/P1.3 and isn't used here.
//
// The command sequence follows GxEPD2's driver for this panel (GxEPD2_213_B74) and Good Display's demo code:
// SW reset, 250 gate lines, X and Y incrementing, border waveform, display update control, internal
// temperature sensor. Full refresh is update sequence 0xF7 (display mode 1), partial is 0xFF (display mode 2,
// clock and analog off afterwards). Both RAMs are written every time (0x26 the previous count, 0x24 the new
// one), so a partial update doesn't depend on what the panel kept through deep sleep.
#include <msp430.h>
#include "epd.h"
#include "render.h"

#define CS    BIT4      // P1
#define SCK   BIT5      // P1
#define DC    BIT6      // P1
#define SDA   BIT7      // P1
#define RES   BIT0      // P2
#define BUSY  BIT1      // P2

// Timer_B0 runs from ACLK / 8 = 4096 Hz while the CPU sleeps in LPM3; times are in its ticks (16 s at most).
#define TICKS(ms) ((uint16_t)(((uint32_t)(ms) * 4096u + 999u) / 1000u))

#define RESET_TICKS       TICKS(10)      // RES low, then high, 10 ms each (Good Display's demo)
#define BUSY_SHORT_TICKS  TICKS(200)     // reset and SW reset (typically a few ms)
#define BUSY_PART_TICKS   TICKS(3000)    // partial update (about 0.5 s)
#define BUSY_FULL_TICKS   TICKS(10000)   // full refresh (about 3.5 s; longer in the cold)

static volatile uint8_t timed_out;

static void timer_start(uint16_t ticks)
{
    timed_out = 0;
    TB0CCR0 = ticks;
    TB0CCTL0 = CCIE;
    TB0CTL = TBSSEL__ACLK | ID__8 | MC__UP | TBCLR;
}

static void timer_stop(void)
{
    TB0CTL = 0;
    TB0CCTL0 = 0;
}

static void sleep_ticks(uint16_t ticks)
{
    timer_start(ticks);
    __disable_interrupt();
    while (!timed_out) {
        __bis_SR_register(LPM3_bits | GIE);      // the timer interrupt ends the sleep
        __disable_interrupt();
    }
    __enable_interrupt();
    timer_stop();
}

// Sleeps until BUSY falls (the panel is done) or the timeout runs out. Returns 1 if BUSY is low.
static uint8_t wait_busy(uint16_t ticks)
{
    P2IES |= BUSY;                               // falling edge
    P2IFG &= ~BUSY;
    P2IE |= BUSY;
    timer_start(ticks);
    __disable_interrupt();
    while ((P2IN & BUSY) && !timed_out) {        // checked with interrupts off, so an edge can't slip past
        __bis_SR_register(LPM3_bits | GIE);
        __disable_interrupt();
    }
    __enable_interrupt();
    timer_stop();
    P2IE &= ~BUSY;
    P2IFG &= ~BUSY;
    return !(P2IN & BUSY);
}

static void spi_on(void)
{
    UCA0CTLW0 = UCSWRST;
    UCA0CTLW0 |= UCCKPH | UCMSB | UCMST | UCSYNC | UCMODE_0 | UCSSEL__SMCLK;   // mode 0, MSB first
    UCA0BRW = 2;                                 // SMCLK 8 MHz / 2 = 4 MHz (the SSD1680 takes 20 MHz)
    SYSCFG3 &= ~USCIARMP;                        // SCLK on P1.5, SIMO on P1.7
    P1SEL0 |= SCK | SDA;
    UCA0CTLW0 &= ~UCSWRST;
}

static void spi_off(void)
{
    while (UCA0STATW & UCBUSY) {
    }
    UCA0CTLW0 = UCSWRST;
    P1SEL0 &= ~(SCK | SDA);                      // back to GPIO, driven low
}

static void spi_byte(uint8_t b)
{
    while (!(UCA0IFG & UCTXIFG)) {
    }
    UCA0TXBUF = b;
}

static void spi_drain(void)
{
    while (UCA0STATW & UCBUSY) {
    }
}

// Starts a command: CS low, the command byte with D/C low, then D/C high for its data.
static void begin(uint8_t c)
{
    P1OUT &= ~(DC | CS);
    spi_byte(c);
    spi_drain();
    P1OUT |= DC;
}

static void end(void)
{
    spi_drain();
    P1OUT |= CS;
    P1OUT &= ~DC;
}

static void command(uint8_t c, const uint8_t *data, uint8_t n)
{
    begin(c);
    while (n--) {
        spi_byte(*data++);
    }
    end();
}

#define CMD(c, ...) do { static const uint8_t a_[] = {__VA_ARGS__}; command((c), a_, sizeof a_); } while (0)
#define CMD0(c) command((c), 0, 0)

static void write_ram(uint8_t ram, const uint8_t d[4])
{
    CMD(0x4E, 0x00);                             // RAM X counter: byte 0
    CMD(0x4F, 0x00, 0x00);                       // RAM Y counter: row 0
    begin(ram);
    for (uint8_t row = 0; row < EPD_ROWS; row++) {
        uint8_t buf[EPD_ROW_BYTES];
        render_row(d, row, buf);
        for (uint8_t i = 0; i < EPD_ROW_BYTES; i++) {
            spi_byte(buf[i]);
        }
    }
    end();
}

uint8_t epd_show(const uint8_t old[4], const uint8_t now[4], uint8_t full)
{
    P2OUT &= ~RES;                               // hardware reset; also the way out of deep sleep
    sleep_ticks(RESET_TICKS);
    P2OUT |= RES;
    sleep_ticks(RESET_TICKS);
    spi_on();
    uint8_t ok = wait_busy(BUSY_SHORT_TICKS);
    CMD0(0x12);                                  // SW reset
    sleep_ticks(RESET_TICKS);                    // "wait 10 ms" (Good Display's sequence), then for BUSY
    ok &= wait_busy(BUSY_SHORT_TICKS);
    CMD(0x01, 0xF9, 0x00, 0x00);                 // driver output: 250 gate lines
    CMD(0x11, 0x03);                             // data entry: X then Y, both incrementing
    CMD(0x44, 0x00, EPD_ROW_BYTES - 1);          // RAM X window: bytes 0-15
    CMD(0x45, 0x00, 0x00, EPD_ROWS - 1, 0x00);   // RAM Y window: rows 0-249
    if (full) {
        CMD(0x3C, 0x05);                         // border waveform for a full refresh
    } else {
        CMD(0x3C, 0x80);                         // border left alone during a partial update
    }
    CMD(0x21, 0x00, 0x80);                       // display update control
    CMD(0x18, 0x80);                             // internal temperature sensor
    write_ram(0x26, full ? now : old);           // the previous image, for display mode 2
    write_ram(0x24, now);                        // the new image
    if (full) {
        CMD(0x22, 0xF7);                         // clock + analog on, load temperature + LUT, mode 1, off
    } else {
        CMD(0x22, 0xFF);                         // the same with display mode 2 (partial)
    }
    CMD0(0x20);                                  // go
    ok &= wait_busy(full ? BUSY_FULL_TICKS : BUSY_PART_TICKS);
    CMD(0x10, 0x03);                             // deep sleep mode 2: 1 uA typ, RAM not kept (rewritten every
                                                 // update anyway); the RAM-keeping sleep is 20 uA. BUSY stays high.
    spi_off();
    return ok;
}

void __attribute__((interrupt(TIMER0_B0_VECTOR))) timer_isr(void)
{
    timed_out = 1;
    __bic_SR_register_on_exit(LPM3_bits);
}

void __attribute__((interrupt(PORT2_VECTOR))) port2_isr(void)
{
    (void)P2IV;                                  // clears the BUSY flag
    __bic_SR_register_on_exit(LPM3_bits);
}
