// SPDX-License-Identifier: MIT
// The FR2422's own supply voltage and die temperature, from its ADC and internal 1.5 V reference.
#ifndef SENSE_H
#define SENSE_H

#include <stdint.h>

#define TEMP_NONE (-128)    // no temperature (missing calibration)

// The supply (the cells, through JP1) in hundredths of a volt: the average of eight readings 1 ms apart.
uint16_t sense_vcc_cv(void);

// The die temperature in whole degrees C, from TI's factory calibration at 30 and 85 C.
int8_t sense_temp_c(void);

#endif
