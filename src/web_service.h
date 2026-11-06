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
 * kept in data/web_default.txt (not touched by a factory reset), shown only
 * on the serial status screen.
 */

#include <Arduino.h>

static const uint32_t WEB_CONNECTION_TIMEOUT_MS = 5000;

/// Load or create the default password and start listening. Call after Ethernet started.
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
 * but then a new one is generated at the next boot.
 * @return true if the new password was saved and read back
 */
bool webRegenerateDefaultPassword();

/// true while no own password has been set
bool webUsesDefaultPassword();

/**
 * @brief Set the web password (serial menu), or nullptr to go back to the device default
 *
 * Saves the settings and logs every browser out.
 * @return false if the password is invalid or saving failed (nothing changed then)
 */
bool webSetPassword(const char* password);
