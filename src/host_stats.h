#pragma once

/**
 * @file host_stats.h
 * @brief Values for HOST-RESOURCES-MIB and ENTITY-MIB: CPU load of both cores, RAM and LittleFS use,
 *        the serial number (flash unique ID).
 *
 * Core 0's load comes from its loop passes (lib/cpu_load LoopLoad), core 1's from the time outside
 * its delay(1). Core 1 publishes its percentage in one byte once a second, so core 0 never reads
 * core 1's ring while it is written.
 */

#include <stdint.h>
#include <snmp_mib.h>

/// Read the serial number and the first LittleFS figures. Call once from setup() after LittleFS.begin().
void hostStatsBegin();

/// Call at the start of every loop() pass on core 0
void hostStatsLoopPass();

/**
 * @brief Account core 1's work. Call from loop1() before its delay
 * @param nowUs  time_us_32() after the work
 * @param busyUs How long the work took
 */
void hostStatsCore1Busy(uint32_t nowUs, uint32_t busyUs);

/// Refresh the LittleFS figures every 10 s (LittleFS.info walks the filesystem). Call from loop() on core 0.
void hostStatsPoll();

/// The current values (static, core 0 only), for the SNMP agent and the status screen
const SnmpHostState& hostStatsSnapshot();
