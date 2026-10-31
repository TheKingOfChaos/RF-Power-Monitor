#pragma once

/**
 * @file ambient_temp.h
 * @brief TMP1075 ambient temperature sensor on the carrier board (I2C0, GP4/GP5, 0x48).
 *
 * Read every 2 seconds on core 0. Accurate to +-1 C and away from the
 * RP2040, so it shows the air in the enclosure rather than the chip. Without
 * the carrier board (or with the sensor missing) ambientTempValid() is false
 * and the SNMP object is absent.
 */

#include <Arduino.h>

/**
 * @brief Start I2C0 and take the first reading. Call once from setup() on core 0.
 */
void ambientTempBegin();

/**
 * @brief Read the sensor every 2 s. Call from loop() on core 0.
 */
void ambientTempPoll();

/**
 * @brief true while the sensor answers (false after 3 failed reads in a row)
 */
bool ambientTempValid();

/**
 * @brief Last reading in tenths of a degree Celsius (only meaningful when valid)
 */
int32_t ambientTempDeciC();

/**
 * @brief Failed reads since boot
 */
uint32_t ambientTempReadErrors();
