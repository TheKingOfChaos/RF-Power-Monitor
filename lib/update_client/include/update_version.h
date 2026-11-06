#pragma once

/**
 * @file update_version.h
 * @brief The version in a .upd file's header, and version order.
 * Pure C++, host tested.
 */

#include <stddef.h>
#include <stdint.h>

#include "update_protocol.h"

/// Bytes of the .upd header updHeaderVersion() needs
static const size_t UPD_HEAD_SIZE = 64;

/**
 * @brief The version in a .upd header (layout: tools/fw_release.py build_upd)
 *
 * Magic 0x50554652 ("RFUP"), format 1, then 16 bytes of version text with at
 * least one NUL (so at most 15 characters).
 * @param head The file's first bytes, at least UPD_HEAD_SIZE
 * @return false for a short head, another magic or format, or an empty or unterminated version
 */
bool updHeaderVersion(const uint8_t* head, size_t len, char out[UPDATE_VERSION_MAX + 1]);

/**
 * @brief Order two versions: <0 when a is older, 0 when equal, >0 when newer
 *
 * major.minor.patch compare as numbers; a version with a pre-release part
 * ("3.4.0-hil") ranks below the same version without one, and two pre-release
 * parts compare as text. Text that is not a version compares as text.
 */
int compareVersions(const char* a, const char* b);
