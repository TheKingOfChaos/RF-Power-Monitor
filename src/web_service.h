#pragma once

/**
 * @file web_service.h
 * @brief The configuration web page on TCP port 80 (lib/web_ui does the parsing,
 *        login and pages; this file connects it to the network and the device).
 *
 * One connection at a time, fully non-blocking: the request is read as it
 * arrives, the response is written only as fast as the TCP send buffer takes
 * it, and a connection that does not finish within WEB_CONNECTION_TIMEOUT_MS
 * is closed. SNMP is served in between, so a slow or hostile browser cannot
 * stall the agent.
 *
 * A firmware upload (POST /firmware/upload) is the one request whose body is
 * not buffered: after the session check it is read 512 bytes at a time into
 * the update service (fw_update_service.h), which writes it to the other
 * slot; 30 s without data ends it. After a verified upload the device
 * restarts into the new firmware, on trial.
 *
 * Login: user "admin". Until a password is set with serial menu option 6 the
 * device default password is used: 10 random characters generated once and
 * kept twice, in data/web_default.txt and data/web_default_b.txt (not touched
 * by a factory reset), shown only on the serial status screen and to the
 * bench tool (benchinfo).
 */

#include <Arduino.h>

static const uint32_t WEB_CONNECTION_TIMEOUT_MS = 5000;

/**
 * @brief Load the default password and its lock (repairing a copy, or making a new
 *        password); call once LittleFS is mounted, whether or not Ethernet starts,
 *        so the lock and the menu's password are right without a network
 */
void webLoadDefaultPassword();

/// Start listening (after webLoadDefaultPassword). Call after Ethernet started.
void webBegin();

/// Serve the web page; call from loop() on core 0
void webPoll();

/// true while the web interface is switched on (setting webEnabled)
bool webEnabled();

/// true while TCP port 80 is open (on, and not still waiting for the port)
bool webListening();

/**
 * @brief Switch the web interface on or off (serial menu option 8) and save it
 *
 * Off closes TCP port 80 (after a response in progress), ends every session
 * and stops announcing the page over mDNS. A factory reset switches it on.
 * @return false if saving failed (nothing changed then)
 */
bool webSetEnabled(bool on);

/// The device default password (shown on the serial status screen)
const char* webDefaultPassword();

/**
 * @brief Replace the device default password with a new random one (serial menu option 7)
 *
 * For when the default password became known. Logs browsers out if the
 * default is in use. The new password is active even if saving it failed,
 * but then a new one is generated at the next boot. Refused while the
 * default password is locked (webDefaultPasswordLocked).
 * @return true if the new password was saved (both copies) and read back;
 *         false and nothing changed while locked
 */
bool webRegenerateDefaultPassword();

/// Shown at boot and on the status screen when webDefaultPasswordLost()
static const char WEB_PASSWORD_LOST_TEXT[] = "Default web password LOST: label is wrong, relabel at the bench";

/**
 * @brief true while the default password is locked: it is printed on the
 *        device label (bench tool, hidden menu commands pwlock/pwunlock)
 *
 * The lock is the file data/web_default.lock, kept like the password itself
 * (data/web_default.txt and data/web_default_b.txt) through factory resets
 * and firmware updates.
 */
bool webDefaultPasswordLocked();

/**
 * @brief Lock or unlock the default password (hidden menu commands pwlock/pwunlock)
 * @return true if the lock file now matches `locked`
 */
bool webSetDefaultPasswordLocked(bool locked);

/// true when boot found no valid copy of a locked default password: a new one was made, the label is wrong
bool webDefaultPasswordLost();

/// true while no own password has been set
bool webUsesDefaultPassword();

/**
 * @brief Set the web password (serial menu), or nullptr to go back to the device default
 *
 * Saves the settings and logs every browser out.
 * @return false if the password is invalid or saving failed (nothing changed then)
 */
bool webSetPassword(const char* password);
