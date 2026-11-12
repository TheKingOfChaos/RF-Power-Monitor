/**
 * @file fuzz_cli_input.cpp
 * @brief Fuzz the serial menu's input (lib/cli_input): typed bytes through the line editor,
 *        each finished line through every prompt's rule
 *
 * Input: the bytes typed; a byte 0xFF switches secret echo on or off. Checks that a
 * line is only printable ASCII and at most MAX_CHARS long, a secret is never echoed,
 * an overflowed line leaves nothing in the buffer, and every accepted value is
 * terminated inside its field.
 */
#include <cli_input.h>
#include <netconfig.h>
#include <settings_codec.h>
#include <update_url.h>

#include "fuzz_common.h"

/// NUL within size bytes, and only printable ASCII before it
static void checkText(const char* s, size_t size) {
    size_t n = strnlen(s, size);
    if (n == size) __builtin_trap();
    for (size_t i = 0; i < n; i++) {
        if (s[i] < 0x20 || s[i] > 0x7E) __builtin_trap();
    }
}

static void takeAll(const char* line) {
    static char msg[CLI_MSG_MAX];
    char dest[128];
    bool fromDhcp = false;
    uint8_t version = 0;
    if (cliTakeAddress(line, "192.168.1.100", dest, msg, sizeof(msg))) checkText(dest, 16);
    if (cliTakeText(line, "public", dest, COMMUNITY_MAX_LENGTH, false, msg, sizeof(msg))) {
        checkText(dest, COMMUNITY_MAX_LENGTH + 1);
        if (dest[0] == '\0') __builtin_trap();  // empty not allowed
    }
    if (cliTakeText(line, "Rack 4", dest, SYS_TEXT_MAX_LENGTH, true, msg, sizeof(msg))) {
        checkText(dest, SYS_TEXT_MAX_LENGTH + 1);
    }
    if (cliTakeHostname(line, "rack-4", dest, msg, sizeof(msg))) {
        checkText(dest, SYS_TEXT_MAX_LENGTH + 1);
        if (dest[0] != '\0' && !hostnameIsValid(dest)) __builtin_trap();
    }
    if (cliTakeTrapTarget(line, "10.132.0.45", false, dest, fromDhcp, msg, sizeof(msg))) {
        checkText(dest, IPV6_TEXT_MAX);
    }
    if (cliTakeTrapVersion(line, TRAP_VERSION_V2C, version, msg, sizeof(msg))) {
        if (version != TRAP_VERSION_V1 && version != TRAP_VERSION_V2C) __builtin_trap();
    }
    if (cliTakeIPv6(line, "fd00::5", dest, msg, sizeof(msg))) checkText(dest, IPV6_TEXT_MAX);
    if (cliTakeUpdateUrl(line, "http://10.132.0.45:8074", false, dest, fromDhcp, msg, sizeof(msg))) {
        checkText(dest, UPDATE_URL_MAX + 1);
        UpdateUrl url;
        if (dest[0] != '\0' && !parseUpdateUrl(dest, url)) __builtin_trap();
    }
    cliConfirmed(line);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static LineEditor* ed = new LineEditor();
    ed->reset();
    bool secret = false;
    for (size_t i = 0; i < size; i++) {
        if (data[i] == 0xFF) {
            secret = !secret;
            continue;
        }
        switch (ed->feed((char)data[i], secret)) {
        case LineEvent::Echo:
            if (secret ? ed->echo() != '*' : (ed->echo() < 0x20 || ed->echo() > 0x7E)) __builtin_trap();
            break;
        case LineEvent::Line: {
            const char* line = ed->line();
            checkText(line, LineEditor::MAX_CHARS + 1);
            takeAll(line);
            break;
        }
        case LineEvent::Overflow:
            for (size_t j = 0; j <= LineEditor::MAX_CHARS; j++) {
                if (ed->line()[j] != 0) __builtin_trap();
            }
            break;
        default: break;
        }
    }
    return 0;
}
