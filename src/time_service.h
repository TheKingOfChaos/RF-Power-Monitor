#pragma once

/**
 * @file time_service.h
 * @brief Real time from NTP: lwIP's SNTP client with the server from DHCP
 *        (option 42, DHCPv6 option 56), typed (address or name) or none.
 *
 * The core keeps the clock (gettimeofday); lwIP sets it through
 * SNTP_SET_SYSTEM_TIME_US, which include/lwipopts.h points at
 * timeServiceSetTime() so the service knows when it was last synced. Until the
 * first answer the device runs on uptime as before. Core 0 only.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/// Called by lwIP's SNTP client with a new time: sets the core's clock, notes the sync
void timeServiceSetTime(unsigned long sec, unsigned long us);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/// Apply currentLogSettings at boot (before the network is up)
void timeServiceBegin();

/// Apply currentLogSettings after a change: DHCP, a typed server, or off
void timeServiceApply();

/// Keep SNTP off while the setting is none (the core restarts SNTP on every link change)
void timeServicePoll();

/// NTP has set the clock (and it reads 2026 or later)
bool timeServiceSynced();

/// Milliseconds since 1970 (UTC); meaningful only when timeServiceSynced()
uint64_t timeServiceEpochMs();

/// Seconds since the last NTP answer; 0 when never synced
uint32_t timeServiceLastSyncAgeS();

/// Danish local time "2026-11-07 19:42:10 CET", or "" when not synced
void timeServiceLocalText(char* out, size_t size);

/// Status line text, e.g. "2026-11-07 19:42:10 CET (NTP pool.ntp.org, synced 3 min ago)"
void timeServiceStatusText(char* out, size_t size);

/// hrSystemDate bytes (DateAndTime, local time); false when not synced
bool timeServiceDateAndTime(uint8_t out[11]);

#endif
