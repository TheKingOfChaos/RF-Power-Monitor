#pragma once

/**
 * @file ab_files.h
 * @brief File reads and writes shared by the A/B stores (SettingsStore,
 *        UpdateSettingsStore). LittleFS must be mounted. Internal to lib/settings.
 */

#include <stddef.h>

/**
 * @brief Read a whole file into buf, NUL terminated
 * @param tooBig Set when the file is longer than size - 1 (buf then holds its start)
 * @return false if the file cannot be opened
 */
bool abReadFile(const char* path, char* buf, size_t size, bool& tooBig);

/**
 * @brief Replace a file's contents
 * @param what Name for log messages ("Settings", "Update settings")
 * @return false (logged) when the file cannot be opened or the write is short
 */
bool abWriteFile(const char* what, const char* path, const char* data, size_t len);
