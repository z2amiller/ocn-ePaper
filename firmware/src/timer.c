// SPDX-License-Identifier: MIT
// Timer_A0 for waits and timeouts (see timer.h). The FR2422 has no Timer_B; Timer_A0's own pins (P1.4-P1.6)
// stay with the SPI and BUSY, since nothing here selects their timer function.
#include <msp430.h>
#include "timer.h"

volatile uint8_t timer_expired;

void timer_start(uint16_t ticks)
{
    timer_expired = 0;
    TA0CCR0 = ticks;
    TA0CCTL0 = CCIE;
    TA0CTL = TASSEL__ACLK | ID__8 | MC__UP | TACLR;
}

void timer_stop(void)
{
    TA0CTL = 0;
    TA0CCTL0 = 0;
}

uint16_t timer_now(void)
{
    return TA0R;
}

void timer_sleep(uint16_t ticks)
{
    timer_start(ticks);
    __disable_interrupt();
    while (!timer_expired) {
        __bis_SR_register(LPM3_bits | GIE);      // the timer interrupt ends the sleep (others may too)
        __disable_interrupt();
    }
    __enable_interrupt();
    timer_stop();
}

void __attribute__((interrupt(TIMER0_A0_VECTOR))) timer_isr(void)
{
    timer_expired = 1;
    __bic_SR_register_on_exit(LPM3_bits);
}
