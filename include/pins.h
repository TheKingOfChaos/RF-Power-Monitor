#pragma once

/**
 * @file pins.h
 * @brief GPIO assignments (see README "Pin Configuration" and .clinerules)
 */

#include <stdint.h>
// The boot program (boot/, scripts/build_boot.py) is built without the Arduino core
#ifndef RFPOWER_BOOT_PROGRAM
#include <Arduino.h>
#endif

const uint8_t LED_PIN = 25;               ///< Onboard LED (LED_BUILTIN), status indication
#ifdef LED_BUILTIN
static_assert(LED_PIN == LED_BUILTIN, "LED_PIN must be the board's LED_BUILTIN");
#endif
const uint8_t POWER_PIN = 27;             ///< Mains power detection, HIGH = present
const uint8_t FACTORY_RESET_PIN = 22;     ///< Factory reset button to GND, hold 10 s

// TMP1075 ambient temperature sensor on I2C0 (carrier board, address 0x48)
const uint8_t TEMP_SENSOR_SDA_PIN = 4;
const uint8_t TEMP_SENSOR_SCL_PIN = 5;

// W5500 Ethernet on SPI0
const uint8_t W5500_MISO_PIN = 16;
const uint8_t W5500_CS_PIN = 17;
const uint8_t W5500_SCK_PIN = 18;
const uint8_t W5500_MOSI_PIN = 19;
const uint8_t W5500_RST_PIN = 20;
const uint8_t W5500_INT_PIN = 21;
