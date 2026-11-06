#include "web_auth.h"

#include <string.h>
#include "default_password.h"
#include "sha256.h"
#include <settings_codec.h>

static_assert(WEB_PASSWORD_HASH_LEN == SETTINGS_WEB_PASSWORD_LEN, "settings must hold the stored web password");

static const char HEX[] = "0123456789abcdef";

static void toHex(const uint8_t* in, size_t len, char* out) {
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = HEX[in[i] >> 4];
        out[2 * i + 1] = HEX[in[i] & 15];
    }
    out[2 * len] = '\0';
}

static bool isLowerHex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

static int hexValue(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

bool webPasswordIsValid(const char* password) {
    if (password == nullptr) return false;
    size_t len = 0;
    for (const char* c = password; *c; c++, len++) {
        if (len >= WEB_PASSWORD_MAX) return false;
        if (*c < 0x21 || *c > 0x7E) return false;
    }
    return len >= WEB_PASSWORD_MIN;
}

void webHashPassword(const char* password, const uint8_t salt[WEB_SALT_LEN], char* out) {
    uint8_t digest[SHA256_DIGEST_LEN];
    size_t pwLen = strlen(password);
    Sha256 first;
    first.update(salt, WEB_SALT_LEN);
    first.update(password, pwLen);
    first.finish(digest);
    for (uint32_t i = 1; i < WEB_HASH_ITERATIONS; i++) {
        Sha256 round;
        round.update(digest, sizeof(digest));
        round.update(salt, WEB_SALT_LEN);
        round.update(password, pwLen);
        round.finish(digest);
    }
    toHex(salt, WEB_SALT_LEN, out);
    out[2 * WEB_SALT_LEN] = '$';
    toHex(digest, sizeof(digest), out + 2 * WEB_SALT_LEN + 1);
}

bool webPasswordHashIsWellFormed(const char* stored) {
    if (stored == nullptr || strlen(stored) != WEB_PASSWORD_HASH_LEN) return false;
    for (size_t i = 0; i < WEB_PASSWORD_HASH_LEN; i++) {
        if (i == 2 * WEB_SALT_LEN) {
            if (stored[i] != '$') return false;
        } else if (!isLowerHex(stored[i])) {
            return false;
        }
    }
    return true;
}

bool webCheckPassword(const char* password, const char* stored) {
    if (password == nullptr || !webPasswordHashIsWellFormed(stored)) return false;
    uint8_t salt[WEB_SALT_LEN];
    for (size_t i = 0; i < WEB_SALT_LEN; i++) {
        salt[i] = (uint8_t)(hexValue(stored[2 * i]) << 4 | hexValue(stored[2 * i + 1]));
    }
    char computed[WEB_PASSWORD_HASH_LEN + 1];
    webHashPassword(password, salt, computed);
    return webConstantTimeEquals(computed, stored);
}

void WebPasswordCheck::begin(const char* password, const char* stored) {
    clear();
    _active = true;
    _valid = password != nullptr && strlen(password) <= WEB_PASSWORD_MAX && webPasswordHashIsWellFormed(stored);
    // Even an invalid input runs all rounds, so the answer time does not reveal why it failed
    if (password != nullptr) strncpy(_password, password, WEB_PASSWORD_MAX);
    if (_valid) {
        memcpy(_stored, stored, WEB_PASSWORD_HASH_LEN + 1);
        for (size_t i = 0; i < WEB_SALT_LEN; i++) {
            _salt[i] = (uint8_t)(hexValue(stored[2 * i]) << 4 | hexValue(stored[2 * i + 1]));
        }
    }
}

bool WebPasswordCheck::step(uint32_t maxRounds) {
    if (!_active) return false;
    if (_done) return true;
    size_t pwLen = strlen(_password);
    for (uint32_t n = 0; n < maxRounds && _round < WEB_HASH_ITERATIONS; n++, _round++) {
        Sha256 h;
        if (_round > 0) h.update(_digest, sizeof(_digest));
        h.update(_salt, WEB_SALT_LEN);
        h.update(_password, pwLen);
        h.finish(_digest);
    }
    if (_round < WEB_HASH_ITERATIONS) return false;
    char computed[WEB_PASSWORD_HASH_LEN + 1];
    toHex(_salt, WEB_SALT_LEN, computed);
    computed[2 * WEB_SALT_LEN] = '$';
    toHex(_digest, sizeof(_digest), computed + 2 * WEB_SALT_LEN + 1);
    _matches = webConstantTimeEquals(computed, _stored) && _valid;
    _done = true;
    memset(_password, 0, sizeof(_password));
    return true;
}

void WebPasswordCheck::clear() {
    memset(_password, 0, sizeof(_password));
    memset(_stored, 0, sizeof(_stored));
    memset(_salt, 0, sizeof(_salt));
    memset(_digest, 0, sizeof(_digest));
    _round = 0;
    _valid = _active = _done = _matches = false;
}

void webMakeDefaultPassword(const uint8_t* random, char* out) {
    for (size_t i = 0; i < WEB_DEFAULT_PASSWORD_LEN; i++) out[i] = WEB_DEFAULT_ALPHABET[random[i] & 31];
    out[WEB_DEFAULT_PASSWORD_LEN] = '\0';
}

bool webConstantTimeEquals(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    size_t la = strlen(a), lb = strlen(b);
    // Walk the longer string completely; a length difference already makes them unequal
    size_t n = la > lb ? la : lb;
    uint8_t diff = (uint8_t)(la != lb);
    for (size_t i = 0; i < n; i++) {
        uint8_t ca = i < la ? (uint8_t)a[i] : 0;
        uint8_t cb = i < lb ? (uint8_t)b[i] : 0;
        diff |= (uint8_t)(ca ^ cb);
    }
    return diff == 0;
}

// ---- sessions ----

WebSessions::WebSessions() { clear(); }

size_t WebSessions::create(const uint8_t random[WEB_SESSION_RANDOM], uint32_t nowMs) {
    size_t slot = 0;
    for (size_t i = 0; i < WEB_MAX_SESSIONS; i++) {
        if (!_s[i].used) {
            slot = i;
            break;
        }
        // All in use: replace the one idle longest
        if (nowMs - _s[i].lastMs > nowMs - _s[slot].lastMs) slot = i;
    }
    Session& s = _s[slot];
    s.used = true;
    s.lastMs = nowMs;
    toHex(random, WEB_TOKEN_HEX / 2, s.token);
    toHex(random + WEB_TOKEN_HEX / 2, WEB_TOKEN_HEX / 2, s.csrf);
    return slot;
}

int WebSessions::find(const char* token, uint32_t nowMs) {
    if (token == nullptr || strlen(token) != WEB_TOKEN_HEX) return -1;
    int found = -1;
    for (size_t i = 0; i < WEB_MAX_SESSIONS; i++) {
        Session& s = _s[i];
        if (!s.used) continue;
        if (nowMs - s.lastMs > WEB_SESSION_IDLE_MS) {
            remove(i);
            continue;
        }
        if (webConstantTimeEquals(s.token, token)) found = (int)i;
    }
    if (found >= 0) _s[found].lastMs = nowMs;
    return found;
}

void WebSessions::remove(size_t slot) {
    if (slot >= WEB_MAX_SESSIONS) return;
    memset(&_s[slot], 0, sizeof(_s[slot]));
}

void WebSessions::clear() {
    memset(_s, 0, sizeof(_s));
}

size_t WebSessions::count() const {
    size_t n = 0;
    for (size_t i = 0; i < WEB_MAX_SESSIONS; i++) n += _s[i].used ? 1 : 0;
    return n;
}

// ---- throttle ----

bool LoginThrottle::allowed(uint32_t nowMs) {
    if (_locked && nowMs - _lockedAtMs >= WEB_LOGIN_LOCKOUT_MS) _locked = false;
    return !_locked;
}

uint32_t LoginThrottle::secondsLeft(uint32_t nowMs) {
    if (!allowed(nowMs)) return (WEB_LOGIN_LOCKOUT_MS - (nowMs - _lockedAtMs) + 999) / 1000;
    return 0;
}

void LoginThrottle::failed(uint32_t nowMs) {
    if (++_failures >= WEB_LOGIN_MAX_FAILURES) {
        _failures = 0;
        _locked = true;
        _lockedAtMs = nowMs;
    }
}
