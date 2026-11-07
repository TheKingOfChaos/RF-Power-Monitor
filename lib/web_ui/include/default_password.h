#pragma once

/**
 * @file default_password.h
 * @brief The device default web password on flash: which of its two copies
 *        to use, which to rewrite, and when it was lost. Pure C++, host tested.
 *
 * The default password is printed on the device label, so it must outlive a
 * corrupt file. It is kept twice (copy A, the original data/web_default.txt,
 * and copy B); every save writes A first, then B, so a valid A is the newer
 * one. The bench tool locks it (a lock file next to the copies); a locked
 * password that is gone from both copies means the label is wrong.
 */

#include <stddef.h>
#include <stdint.h>

/// The characters of a generated default password: no 0/o/1/l to avoid misreading (32 characters)
extern const char WEB_DEFAULT_ALPHABET[];

/**
 * @brief true if text is a default password: exactly WEB_DEFAULT_PASSWORD_LEN
 *        characters, all from WEB_DEFAULT_ALPHABET (false for nullptr)
 */
bool webDefaultPasswordIsValid(const char* text);

/// Where the default password in use comes from
enum class DefaultPasswordSource : uint8_t {
    CopyA,  // copy A is valid
    CopyB,  // copy A is not, copy B is
    None,   // neither: a new password must be made
};

/// What to do with the two copies at boot
struct DefaultPasswordChoice {
    DefaultPasswordSource source;
    bool writeA;           // copy A must be (re)written
    bool writeB;           // copy B must be (re)written
    bool lostWhileLocked;  // locked, and no valid copy now or at an earlier boot: the label is wrong
};

/**
 * @brief Decide which copy to use and which to repair
 *
 * A valid copy A wins; the other copy is rewritten when it is invalid or
 * differs. With no valid copy both are written (with a new password).
 * While locked, two valid copies that differ also count as a loss: firmware
 * before 3.5.0 (after a rollback) knows no lock and replaces copy A only.
 * A loss is reported until the bench locks the password again: the boot that
 * made the new password leaves a marker (lostBefore), because the next boot
 * finds both new copies valid.
 * @param a          Contents of copy A, nullptr if the file is missing
 * @param b          Contents of copy B, nullptr if the file is missing
 * @param locked     The lock file exists
 * @param lostBefore The lost marker exists (an earlier boot lost the locked password)
 */
DefaultPasswordChoice chooseDefaultPassword(const char* a, const char* b, bool locked, bool lostBefore);
