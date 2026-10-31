#pragma once

/**
 * @file tmp1075.h
 * @brief TMP1075 temperature sensor: register map, conversion and read state.
 *
 * The carrier board has a TMP1075 (LM75 compatible) on I2C0, GP4 = SDA,
 * GP5 = SCL, address 0x48 (see hardware/pico-poe-mains-carrier/README.md).
 * Pure C++, unit tested on the host; src/ambient_temp.cpp does the I2C reads.
 */

#include <stdint.h>

/// I2C address with A0..A2 to GND
constexpr uint8_t TMP1075_ADDRESS = 0x48;
/// Temperature register, 16 bits, MSB first
constexpr uint8_t TMP1075_REG_TEMP = 0x00;
/// Device ID register (TMP1075 only, not on other LM75 parts)
constexpr uint8_t TMP1075_REG_DEVICE_ID = 0x0F;
/// Value of the device ID register
constexpr uint16_t TMP1075_DEVICE_ID = 0x7500;

/**
 * @brief Convert the temperature register to tenths of a degree Celsius
 *
 * The register is a 12-bit two's complement value, left-aligned, 0.0625 C per
 * bit (1/256 C per register unit). Rounded half away from zero.
 * @param raw Register value, MSB first as read from the bus
 * @return -1280 .. 1279
 */
int32_t tmp1075ToDeciC(uint16_t raw);

/**
 * @brief Last good reading and whether the sensor counts as present
 *
 * A single failed read (a glitch on the bus) keeps the last value; after
 * MAX_FAILURES failed reads in a row the sensor counts as missing until it
 * answers again.
 */
class TempSensorState {
public:
    /// Failed reads in a row before the sensor counts as missing
    static constexpr uint8_t MAX_FAILURES = 3;

    /**
     * @brief Record one read attempt
     * @param ok true if the sensor answered
     * @param deciC The reading, used only when ok
     */
    void reading(bool ok, int32_t deciC);

    /// true while the reading is current
    bool valid() const { return _valid; }

    /// Last good reading in tenths of a degree Celsius (only meaningful when valid())
    int32_t deciC() const { return _deciC; }

    /// Failed reads since boot
    uint32_t readErrors() const { return _readErrors; }

private:
    bool _valid = false;
    int32_t _deciC = 0;
    uint8_t _failuresInRow = 0;
    uint32_t _readErrors = 0;
};
