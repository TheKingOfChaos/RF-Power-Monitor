#pragma once

/**
 * @file cpu_temp.h
 * @brief RP2040 on-chip temperature sensor (ADC channel 4).
 *
 * Measures the die, not the air: it reads a few degrees above ambient and is
 * accurate to a few degrees. Good for "the box is overheating", not as a
 * room thermometer. Averaged over the last 8 samples, one every 2 seconds
 * (16 seconds).
 */

#include <Arduino.h>

/**
 * @brief Take the first reading. Call once from setup() on core 0.
 */
void cpuTempBegin();

/**
 * @brief Sample every 2 s. Call from loop() on core 0.
 */
void cpuTempPoll();

/**
 * @brief Averaged die temperature in tenths of a degree Celsius
 */
int32_t cpuTempDeciC();
