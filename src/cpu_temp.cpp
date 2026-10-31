#include "cpu_temp.h"

static const uint32_t SAMPLE_INTERVAL_MS = 2000;
static const uint8_t SAMPLES = 8;

static int32_t samples[SAMPLES];
static uint8_t next = 0;
static int32_t average = 0;

/**
 * @brief One reading in tenths of a degree
 */
static int32_t readDeciC() {
    float c = analogReadTemp();
    return (int32_t)(c * 10.0f + (c >= 0 ? 0.5f : -0.5f));
}

void cpuTempBegin() {
    int32_t first = readDeciC();
    for (uint8_t i = 0; i < SAMPLES; i++) samples[i] = first;
    average = first;
}

void cpuTempPoll() {
    static uint32_t lastSample = 0;
    if (millis() - lastSample < SAMPLE_INTERVAL_MS) return;
    lastSample = millis();

    samples[next] = readDeciC();
    next = (next + 1) % SAMPLES;
    int32_t sum = 0;
    for (uint8_t i = 0; i < SAMPLES; i++) sum += samples[i];
    average = sum / SAMPLES;
}

int32_t cpuTempDeciC() {
    return average;
}
