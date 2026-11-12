/**
 * @file fuzz_settings.cpp
 * @brief Fuzz the settings files as read from flash (lib/settings_codec, lib/update_client, lib/syslog)
 *
 * Input: byte 0 picks the file (0 settings, 1 update site, 2 NTP/syslog), the
 * rest is its JSON. Anything a loader accepts must save within the documented
 * buffer size and load back the same: the A/B store depends on that.
 */
#include <settings_codec.h>
#include <log_settings.h>
#include <update_settings.h>

#include "fuzz_common.h"

static void settingsRoundTrip(const char* json) {
    Settings s;
    if (settingsFromJson(json, s) != SettingsParseError::None) return;
    static char out[SETTINGS_JSON_MAX];
    size_t n = settingsToJson(s, out, sizeof(out));
    if (n == 0 || n >= sizeof(out)) __builtin_trap();
    Settings again;
    if (settingsFromJson(out, again) != SettingsParseError::None || !settingsEqual(s, again)) __builtin_trap();
}

static void updateRoundTrip(const char* json) {
    UpdateSettings s;
    if (!updateSettingsFromJson(json, s)) return;
    char out[UPDATE_SETTINGS_JSON_MAX], out2[UPDATE_SETTINGS_JSON_MAX];
    size_t n = updateSettingsToJson(s, out, sizeof(out));
    if (n == 0 || n >= sizeof(out)) __builtin_trap();
    UpdateSettings again;
    if (!updateSettingsFromJson(out, again)) __builtin_trap();
    size_t n2 = updateSettingsToJson(again, out2, sizeof(out2));
    if (n2 != n || memcmp(out, out2, n) != 0) __builtin_trap();
}

static void logRoundTrip(const char* json) {
    LogSettings s;
    if (!logSettingsFromJson(json, s)) return;
    char out[LOG_SETTINGS_JSON_MAX], out2[LOG_SETTINGS_JSON_MAX];
    size_t n = logSettingsToJson(s, out, sizeof(out));
    if (n == 0 || n >= sizeof(out)) __builtin_trap();
    LogSettings again;
    if (!logSettingsFromJson(out, again)) __builtin_trap();
    size_t n2 = logSettingsToJson(again, out2, sizeof(out2));
    if (n2 != n || memcmp(out, out2, n) != 0) __builtin_trap();
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzInput in(data, size);
    uint8_t which = in.byte() % 3;
    size_t len;
    const uint8_t* json = in.rest(len);
    std::vector<char> s = fuzzString(json, len);
    if (which == 0) settingsRoundTrip(s.data());
    if (which == 1) updateRoundTrip(s.data());
    if (which == 2) logRoundTrip(s.data());
    return 0;
}
