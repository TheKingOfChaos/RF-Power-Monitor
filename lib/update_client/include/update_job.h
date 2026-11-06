#pragma once

/**
 * @file update_job.h
 * @brief A firmware download the update server asked for, and what the next
 *        check-in reports about it.
 *
 * The device downloads only the version an authenticated reply named, refuses a
 * file whose header names another version, and remembers why a download failed
 * ("<v> download <why>") or was refused ("<v> refused <why>") until a check-in
 * has reported it; the site then ends the assignment, so a failing release is
 * never retried in a loop. Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>

#include <boot_record.h>

#include "update_protocol.h"

/// One download from the update server
class UpdateJob {
public:
    /// What to do with a download after its first bytes
    enum class Step : uint8_t { Continue, Refuse };

    /**
     * @brief Start a download of wanted (the version an authenticated reply named)
     * @return false when it is the running version: nothing to do
     */
    bool begin(const char* wanted, const char* running);

    /// A download is in progress
    bool active() const { return _active; }

    /// The version being downloaded (or last downloaded)
    const char* version() const { return _version; }

    /**
     * @brief Check the file's header (its first UPD_HEAD_SIZE bytes)
     * @return Refuse (and record why) when it is not a .upd file or names another version
     */
    Step head(const uint8_t* first, size_t len);

    /// The download broke off (network): record "<v> download <why>" and end the job
    void failDownload(const char* why);

    /// The device refused the file: record "<v> refused <why>" and end the job
    void refused(const char* why);

    /// The failure to report at the next check-in, "" when none
    const char* failLine() const { return _fail; }

    /// Counts failures; a check-in remembers it so reported() clears only what it carried
    uint32_t failSequence() const { return _failSeq; }

    /// A check-in that carried failure failSeq was accepted by the site
    void reported(uint32_t failSeq);

private:
    void setFail(const char* kind, const char* why);

    bool _active = false;
    char _version[UPDATE_VERSION_MAX + 1] = "";
    char _fail[sizeof(CheckinInfo::fail)] = "";
    uint32_t _failSeq = 0;
};

/**
 * @brief The check-in's "last" value for the boot record's last result
 * @return In out: "none", or "installed", "rolledback-crash", "rolledback-unreachable",
 *         "fallback-crash", followed by " <version>" when a version is known
 */
void checkinLastText(FwResult result, const char* version, char* out, size_t size);
