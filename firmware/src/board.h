// SPDX-License-Identifier: MIT
// The MSP430FR2422's pins on the ocn-ePaper board (revision 0.3 on). Everything but the crystal is on Port 1.
//   P1.0 WAKE   the reed switch to the supply, with R3 (10M) to ground: high while the magnet is on
//   P1.1        not connected
//   P1.2 D/C    panel data/command
//   P1.3 CS     panel chip select
//   P1.4 SDA    panel data (UCA0SIMO)
//   P1.5 BUSY   panel busy: high while it works, and in deep sleep
//   P1.6 SCLK   panel clock (UCA0CLK)
//   P1.7 RES    panel reset, active low
//   P2.0, P2.1  the 32.768 kHz crystal (XOUT, XIN)
//   P2.2        not connected
#ifndef BOARD_H
#define BOARD_H

#include <msp430.h>

#define WAKE  BIT0
#define DC    BIT2
#define CS    BIT3
#define SDA   BIT4
#define BUSY  BIT5
#define SCK   BIT6
#define RES   BIT7

#endif
