#include "ambient_temp.h"

#include <Wire.h>
#include <pins.h>
#include <tmp1075.h>

static const uint32_t SAMPLE_INTERVAL_MS = 2000;
static const uint32_t I2C_CLOCK_HZ = 100000;
/// A stuck bus must not hold up core 0's network loop
static const uint32_t I2C_TIMEOUT_MS = 5;

static TempSensorState state;

/**
 * @brief Read the temperature register
 * @return false if the sensor did not answer
 */
static bool readRaw(uint16_t& raw) {
    Wire.beginTransmission(TMP1075_ADDRESS);
    Wire.write(TMP1075_REG_TEMP);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom(TMP1075_ADDRESS, (uint8_t)2) != 2) return false;
    raw = (uint16_t)(Wire.read() << 8);
    raw |= (uint16_t)Wire.read();
    return true;
}

/**
 * @brief One read attempt, recorded in the sensor state
 */
static void sample() {
    uint16_t raw = 0;
    bool ok = readRaw(raw);
    state.reading(ok, ok ? tmp1075ToDeciC(raw) : 0);
}

void ambientTempBegin() {
    Wire.setSDA(TEMP_SENSOR_SDA_PIN);
    Wire.setSCL(TEMP_SENSOR_SCL_PIN);
    Wire.begin();
    Wire.setClock(I2C_CLOCK_HZ);
    Wire.setTimeout(I2C_TIMEOUT_MS);
    sample();
}

void ambientTempPoll() {
    static uint32_t lastSample = 0;
    if (millis() - lastSample < SAMPLE_INTERVAL_MS) return;
    lastSample = millis();
    sample();
}

bool ambientTempValid() {
    return state.valid();
}

int32_t ambientTempDeciC() {
    return state.deciC();
}

uint32_t ambientTempReadErrors() {
    return state.readErrors();
}
