// Check-in history with the update site for SNMP (updateSite group, 63050.8) and syslog:
// last result, minutes since the last good check-in, failures in a row. Pure, host tested.
#pragma once
#include <stdint.h>

/// What the updateSite group reports
struct CheckinSnapshot {
    uint8_t result;             ///< 0 off, 1 never (not yet), 2 ok, 3 failed
    uint32_t minutesSinceGood;  ///< since the last good check-in, or since check-ins became due when
                                ///< none yet; 0 while off
    uint32_t failedInRow;
};

/// Whether a check-in changed the story: worth one syslog line
enum class CheckinChange : uint8_t { None, StartedFailing, WorksAgain };

class CheckinStats {
public:
    /// Each poll: check-ins are due (configured and the network is up); the first true starts the clock
    void active(bool due, uint32_t nowMs);
    /// A check-in ended
    CheckinChange done(bool ok, uint32_t nowMs);
    /// New update server settings: forget the old server's history; minutes count from now
    void restart(uint32_t nowMs);
    /// The updateSite values now; `configured`: a URL and a key are set
    CheckinSnapshot snapshot(bool configured, uint32_t nowMs) const;

private:
    bool _active = false;
    uint32_t _sinceMs = 0;    // when check-ins became due, or the last restart()
    bool _good = false;
    uint32_t _lastGoodMs = 0;
    uint32_t _failedInRow = 0;
};
