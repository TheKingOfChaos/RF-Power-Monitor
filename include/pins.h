#pragma once

/**
 * @file pins.h
 * @brief GPIO assignments (see README "Pin Configuration" and .clinerules)
 */

#include <Arduino.h>

const uint8_t LED_PIN = LED_BUILTIN;      ///< Onboard LED, status indication
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
