#pragma once
/**
 * @file cli_input.h
 * @brief Serial menu input: the line editor and the rules for what each prompt accepts.
 *
 * Pure C++ (no Arduino.h), so the input handling behind ".clinerules: sanitize all
 * CLI inputs" is tested on the PC. src/cli.cpp reads and writes the serial port and
 * keeps the menu's state machine; the checks of the values themselves are in
 * lib/netconfig, lib/settings_codec and lib/update_client.
 *
 * The cliTake*() rules share one convention: empty input keeps the current value,
 * on success dest holds the new value and true is returned, on failure dest is
 * unchanged and msg receives the line to print (without CR LF).
 */

#include <stddef.h>
#include <stdint.h>

/// What the caller does after LineEditor::feed()
enum class LineEvent : uint8_t {
    None,        ///< Nothing to show (ignored character, the LF of CR LF, backspace on an empty line)
    Echo,        ///< Show the character (echo())
    Erase,       ///< Remove the last character from the screen ("\b \b")
    Line,        ///< Enter: line() holds the input
    Overflow     ///< Enter after more characters than MAX_CHARS: the line is refused, never acted on cut short
};

/**
 * @brief Collects one input line from typed characters
 *
 * Only printable ASCII (0x20..0x7E) is kept; other bytes are dropped. CR, LF and
 * CR LF each end a line. A character past MAX_CHARS is dropped and makes the whole
 * line refused at Enter (Overflow). The buffer is wiped on Overflow and by wipe(),
 * so a typed password or key does not stay in memory.
 */
class LineEditor {
public:
    /// Longest line: room for a 127-character URL after a command word
    static const size_t MAX_CHARS = 143;

    LineEditor() { reset(); }

    /**
     * @brief Take one typed character
     * @param secret Echo '*' instead of the character (passwords, keys)
     */
    LineEvent feed(char c, bool secret);

    /// The character to show for LineEvent::Echo
    char echo() const { return _echo; }

    /// After LineEvent::Line: the input, NUL terminated, valid until the next feed() or wipe()
    const char* line() const { return _buf; }

    /// Clear the buffer's bytes (after a password or key was read)
    void wipe();

    /// Forget everything, as at power-up
    void reset();

private:
    char _buf[MAX_CHARS + 1];
    size_t _len;
    bool _overflow;
    bool _lastWasCR;
    char _echo;
};

/// Room for any message cliTake*() writes
static const size_t CLI_MSG_MAX = 192;

/// "y" or "Y" (or a word starting with them) confirms; anything else cancels
bool cliConfirmed(const char* input);

/**
 * @brief An IPv4 address (static IP, mask, gateway)
 * @param dest 16 bytes
 */
bool cliTakeAddress(const char* input, const char* current, char* dest, char* msg, size_t msgSize);

/**
 * @brief A text setting (community, sysContact, sysLocation): "-" clears it when allowEmpty,
 *        at most maxLength printable ASCII characters
 * @param dest maxLength + 1 bytes
 */
bool cliTakeText(const char* input, const char* current, char* dest, size_t maxLength, bool allowEmpty, char* msg,
                 size_t msgSize);

/**
 * @brief The hostname (sysName): "-" goes back to the default (dest empty), else a valid hostname.
 *        Enter keeps current as it is (it may be free text from older firmware).
 * @param dest SYS_TEXT_MAX_LENGTH + 1 bytes
 */
bool cliTakeHostname(const char* input, const char* current, char* dest, char* msg, size_t msgSize);

/**
 * @brief The trap destination: "dhcp" (either case) takes it from DHCP option 224 and keeps the
 *        fixed address for switching back, "-" turns traps off, else an IPv4 or IPv6 address
 *        (IPv6 stored in RFC 5952 form)
 * @param destTarget IPV6_TEXT_MAX bytes
 */
bool cliTakeTrapTarget(const char* input, const char* currentTarget, bool currentFromDhcp, char* destTarget,
                       bool& destFromDhcp, char* msg, size_t msgSize);

/**
 * @brief The trap version: "1" or "2c" / "2C"
 * @param dest TRAP_VERSION_V1 or TRAP_VERSION_V2C
 */
bool cliTakeTrapVersion(const char* input, uint8_t current, uint8_t& dest, char* msg, size_t msgSize);

/**
 * @brief A static IPv6 address or gateway: "-" clears it, else stored in RFC 5952 form
 *        (whether it is usable is checked by validateStaticIPv6())
 * @param dest IPV6_TEXT_MAX bytes
 */
bool cliTakeIPv6(const char* input, const char* current, char* dest, char* msg, size_t msgSize);

/**
 * @brief The update server: "none" (no check-ins), "dhcp" (DHCP option 225; the stored URL is
 *        kept for later), else an http://host[:port][/path] URL of at most UPDATE_URL_MAX characters
 * @param destUrl UPDATE_URL_MAX + 1 bytes
 */
bool cliTakeUpdateUrl(const char* input, const char* currentUrl, bool currentFromDhcp, char* destUrl,
                      bool& destFromDhcp, char* msg, size_t msgSize);
