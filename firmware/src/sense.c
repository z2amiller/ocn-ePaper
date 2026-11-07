// SPDX-License-Identifier: MIT
// Supply voltage and die temperature from the ADC, as in TI's FR211x examples:
//   - supply: convert the internal 1.5 V reference (A13) against the supply (VR+ = DVCC), so
//     DVCC = 1.5 V x 1023 / reading (msp430fr211x_adc10_05, "Lo_Batt");
//   - temperature: convert the sensor (A12) against the 1.5 V reference and scale between the two factory
//     calibration readings in the TLV at 30 C and 85 C (msp430fr211x_adc10_16).
// The reference is accurate to a few percent, so the voltage is an end-of-life warning, not a fuel gauge.
// Everything is switched off again afterwards; each reading costs well under a microcoulomb.
#include <msp430.h>
#include "sense.h"

#define CAL_15V_30C (*(const uint16_t *)0x1A1A)    // TLV: sensor reading at 30 C, 1.5 V reference
#define CAL_15V_85C (*(const uint16_t *)0x1A1C)    // TLV: sensor reading at 85 C

static void ref_on(uint16_t extra)
{
    PMMCTL0_H = PMMPW_H;
    PMMCTL2 |= INTREFEN | extra;
    PMMCTL0_H = 0;
    for (uint16_t i = 0; i < 2000 && !(PMMCTL2 & REFGENRDY); i++) {
    }
}

static void ref_off(void)
{
    ADCCTL0 &= ~ADCENC;
    ADCCTL0 = 0;
    PMMCTL0_H = PMMPW_H;
    PMMCTL2 &= ~(INTREFEN | TSENSOREN);
    PMMCTL0_H = 0;
}

// a / b, for a quotient that fits in 16 bits: shift and subtract, so the library's 32-bit division routines
// (about 200 bytes of the FR2111's 3.7 KB) stay out of the build.
static uint16_t udiv(uint32_t a, uint16_t b)
{
    uint16_t q = 0;
    uint32_t d = (uint32_t)b << 15;
    for (uint8_t i = 0; i < 16; i++, d >>= 1) {
        q <<= 1;
        if (a >= d) { a -= d; q |= 1; }
    }
    return q;
}

// One conversion with the ADC set up for `mctl` (channel and reference).
static uint16_t convert(uint16_t mctl)
{
    ADCCTL0 &= ~ADCENC;
    ADCCTL0 = ADCSHT_8 | ADCON;                  // 256 ADC clocks of sampling: the sensor needs > 30 us
    ADCCTL1 = ADCSHP;                            // sampling timer, MODOSC
    ADCCTL2 = ADCRES_1;                          // 10 bits
    ADCMCTL0 = mctl;
    ADCCTL0 |= ADCENC | ADCSC;
    while (ADCCTL1 & ADCBUSY) {
    }
    return ADCMEM0;
}

uint16_t sense_vcc_cv(void)
{
    ref_on(0);
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 8; i++) {
        sum += convert(ADCINCH_13 | ADCSREF_0);  // 1.5 V reference, measured against the supply
        __delay_cycles(8000);                    // ~1 ms at 8 MHz, so the readings spread over the load
    }
    ref_off();
    if (sum == 0) return 0;
    return udiv((uint32_t)150 * 1023 * 8, sum);
}

int8_t sense_temp_c(void)
{
    uint16_t c30 = CAL_15V_30C, c85 = CAL_15V_85C;
    if (c30 == 0 || c30 == 0xFFFF || c85 <= c30) return TEMP_NONE;
    ref_on(TSENSOREN);
    int16_t diff = (int16_t)convert(ADCINCH_12 | ADCSREF_1) - (int16_t)c30;
    ref_off();
    uint16_t q = udiv((uint32_t)(uint16_t)(diff < 0 ? -diff : diff) * 110, c85 - c30);   // half-degrees from 30 C
    int16_t half = diff < 0 ? -(int16_t)q : (int16_t)q;
    int16_t t = 30 + (half + (half >= 0 ? 1 : -1)) / 2;
    return (int8_t)(t < -99 ? -99 : t > 99 ? 99 : t);   // two digits on the panel
}
