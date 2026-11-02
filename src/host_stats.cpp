#include "host_stats.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <cpu_load.h>
#include <pico/unique_id.h>
#include "version.h"

/// Main SRAM, as the build reports it ("RAM: ... from 262144 bytes")
static const uint32_t RAM_TOTAL_BYTES = 262144;
static const uint32_t FS_REFRESH_MS = 10000;

static LoopLoad core0Load;                 // core 0 only
static CpuLoad core1Load;                  // core 1 only
static volatile uint8_t core1Percent = 0;  // written by core 1, read by core 0
static uint32_t core1PublishedUs = 0;      // core 1 only

static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static SnmpHostState snapshot;             // core 0 only
static uint32_t lastFsMs = 0;

/// LittleFS size and use into snapshot
static void readFs() {
    FSInfo info;
    snapshot.fsValid = LittleFS.info(info);
    snapshot.fsTotalBytes = snapshot.fsValid ? (uint32_t)info.totalBytes : 0;
    snapshot.fsUsedBytes = snapshot.fsValid ? (uint32_t)info.usedBytes : 0;
    lastFsMs = millis();
}

void hostStatsBegin() {
    pico_get_unique_board_id_string(serial, sizeof(serial));
    snapshot.serialNumber = serial;
    snapshot.firmwareRevision = FIRMWARE_VERSION;
    snapshot.hardwareRevision = HARDWARE_REVISION;
    snapshot.ramTotalBytes = RAM_TOTAL_BYTES;
    readFs();
}

void hostStatsLoopPass() {
    core0Load.pass(time_us_32());
}

void hostStatsCore1Busy(uint32_t nowUs, uint32_t busyUs) {
    core1Load.add(nowUs, busyUs);
    // Publish once a second: percent() sums the minute, too much to do every millisecond
    if (nowUs - core1PublishedUs >= 1000000) {
        core1PublishedUs = nowUs;
        core1Percent = core1Load.percent();  // one byte: a single store, no tearing
        __dmb();
    }
}

void hostStatsPoll() {
    if (millis() - lastFsMs >= FS_REFRESH_MS) readFs();
}

const SnmpHostState& hostStatsSnapshot() {
    uint32_t freeHeap = (uint32_t)rp2040.getFreeHeap();
    snapshot.ramUsedBytes = freeHeap < RAM_TOTAL_BYTES ? RAM_TOTAL_BYTES - freeHeap : 0;
    snapshot.cpuLoad[0] = core0Load.percent();
    __dmb();
    snapshot.cpuLoad[1] = core1Percent;
    return snapshot;
}
