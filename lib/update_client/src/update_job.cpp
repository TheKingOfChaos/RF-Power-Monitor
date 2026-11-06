#include "update_job.h"

#include <stdio.h>
#include <string.h>

#include "update_version.h"

bool UpdateJob::begin(const char* wanted, const char* running) {
    _active = false;
    if (wanted == nullptr || running == nullptr || strcmp(wanted, running) == 0) return false;
    snprintf(_version, sizeof(_version), "%s", wanted);
    if (compareVersions(wanted, UPDATE_MIN_VERSION) < 0) {
        refused("older than 3.4.0");
        return false;
    }
    _active = true;
    return true;
}

uint32_t downloadDeadlineMs(uint32_t contentLength) {
    return 120000u + contentLength / 2u;  // at least 2 bytes per millisecond
}

UpdateJob::Step UpdateJob::head(const uint8_t* first, size_t len) {
    char v[UPDATE_VERSION_MAX + 1];
    if (!updHeaderVersion(first, len, v)) {
        refused("not an update file");
        return Step::Refuse;
    }
    if (strcmp(v, _version) != 0) {
        refused("version mismatch");
        return Step::Refuse;
    }
    return Step::Continue;
}

void UpdateJob::failDownload(const char* why) {
    setFail("download", why);
}

void UpdateJob::refused(const char* why) {
    setFail("refused", why);
}

void UpdateJob::reported(uint32_t failSeq) {
    if (failSeq == _failSeq) _fail[0] = '\0';
}

void UpdateJob::setFail(const char* kind, const char* why) {
    snprintf(_fail, sizeof(_fail), "%s %s %s", _version, kind, why != nullptr ? why : "");
    _failSeq++;
    _active = false;
}

void checkinLastText(FwResult result, const char* version, char* out, size_t size) {
    const char* what;
    switch (result) {
    case FwResult::Installed:             what = "installed"; break;
    case FwResult::RolledBackCrash:       what = "rolledback-crash"; break;
    case FwResult::RolledBackUnreachable: what = "rolledback-unreachable"; break;
    case FwResult::FallbackCrash:         what = "fallback-crash"; break;
    default:
        snprintf(out, size, "none");
        return;
    }
    if (version == nullptr || version[0] == '\0') {
        snprintf(out, size, "%s", what);
    } else {
        snprintf(out, size, "%s %s", what, version);
    }
}
