#include "default_password.h"

#include <string.h>
#include "web_auth.h"

const char WEB_DEFAULT_ALPHABET[] = "abcdefghijkmnpqrstuvwxyz23456789";

static_assert(sizeof(WEB_DEFAULT_ALPHABET) - 1 == 32, "webMakeDefaultPassword picks with 5 random bits");

bool webDefaultPasswordIsValid(const char* text) {
    if (text == nullptr) return false;
    size_t len = 0;
    for (const char* c = text; *c; c++, len++) {
        if (len >= WEB_DEFAULT_PASSWORD_LEN || strchr(WEB_DEFAULT_ALPHABET, *c) == nullptr) return false;
    }
    return len == WEB_DEFAULT_PASSWORD_LEN;
}

DefaultPasswordChoice chooseDefaultPassword(const char* a, const char* b, bool locked, bool lostBefore) {
    bool aValid = webDefaultPasswordIsValid(a);
    bool bValid = webDefaultPasswordIsValid(b);
    DefaultPasswordChoice c{DefaultPasswordSource::None, true, true, locked && lostBefore};
    if (aValid) {
        c.source = DefaultPasswordSource::CopyA;
        c.writeA = false;
        c.writeB = !bValid || strcmp(a, b) != 0;
    } else if (bValid) {
        c.source = DefaultPasswordSource::CopyB;
        c.writeA = true;
        c.writeB = false;
    } else {
        c.lostWhileLocked = locked;
    }
    return c;
}
