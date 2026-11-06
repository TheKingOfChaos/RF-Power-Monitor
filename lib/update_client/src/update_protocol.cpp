#include "update_protocol.h"

#include <stdio.h>
#include <string.h>

#include <monocypher.h>

namespace {

/// Value of one hex digit, or -1
int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Decode exactly 2 * n hex characters into out
bool decodeHex(const char* text, size_t n, uint8_t* out) {
    for (size_t i = 0; i < n; i++) {
        int hi = hexValue(text[2 * i]);
        int lo = hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

/// true if s has no byte below 0x20 or 0x7F (a value must stay on its line)
bool isPrintable(const char* s) {
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || *s == 0x7F) return false;
    }
    return true;
}

/// Appends lines to a buffer, remembering whether everything fitted
struct LineWriter {
    char* out;
    size_t size;
    size_t len;
    bool ok;

    void line(const char* key, const char* value) {
        if (!ok) return;
        if (!isPrintable(value)) {
            ok = false;
            return;
        }
        int n = snprintf(out + len, size - len, "%s=%s\n", key, value);
        if (n < 0 || (size_t)n >= size - len) {
            ok = false;
            return;
        }
        len += (size_t)n;
    }
};

/// The line [start, end) is "<key>=<value>": value set to its start, length in valueLen
bool lineValue(const char* start, const char* end, const char* key, const char*& value, size_t& valueLen) {
    size_t keyLen = strlen(key);
    if ((size_t)(end - start) < keyLen + 1 || memcmp(start, key, keyLen) != 0 || start[keyLen] != '=') return false;
    value = start + keyLen + 1;
    valueLen = (size_t)(end - value);
    return true;
}

/// A version or path value: 1..max printable characters without spaces
bool tokenOk(const char* v, size_t len, size_t max) {
    if (len == 0 || len > max) return false;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)v[i] <= 0x20 || v[i] == 0x7F) return false;
    }
    return true;
}

}  // namespace

size_t buildCheckinBody(const CheckinInfo& in, char* out, size_t size) {
    if (out == nullptr || size == 0) return 0;
    LineWriter w{out, size, 0, true};
    out[0] = '\0';
    char slot[2] = {in.slot, '\0'};
    w.line("serial", in.serial);
    w.line("hostname", in.hostname);
    w.line("ipv4", in.ipv4);
    for (uint8_t i = 0; i < in.ipv6Count && i < UPDATE_IPV6_MAX; i++) w.line("ipv6", in.ipv6[i]);
    w.line("mac", in.mac);
    w.line("version", in.version);
    w.line("slot", slot);
    w.line("trial", in.trial ? "1" : "0");
    w.line("update", in.downloading ? "downloading" : "idle");
    w.line("fail", in.fail);
    w.line("last", in.last);
    w.line("nonce", in.nonce);
    if (!w.ok) {
        out[0] = '\0';
        return 0;
    }
    return w.len;
}

void updateMacHex(const uint8_t key[UPDATE_KEY_LEN], const uint8_t* data, size_t len, char out[UPDATE_MAC_HEX + 1]) {
    uint8_t mac[32];
    crypto_blake2b_keyed(mac, sizeof(mac), key, UPDATE_KEY_LEN, data, len);
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(mac); i++) {
        out[2 * i] = digits[mac[i] >> 4];
        out[2 * i + 1] = digits[mac[i] & 0x0F];
    }
    out[UPDATE_MAC_HEX] = '\0';
}

bool parseKeyHex(const char* text, uint8_t key[UPDATE_KEY_LEN]) {
    if (text == nullptr || strlen(text) != 2 * UPDATE_KEY_LEN) return false;
    return decodeHex(text, UPDATE_KEY_LEN, key);
}

ReplyKind parseCheckinReply(const char* body, size_t len, const char* nonce, const uint8_t key[UPDATE_KEY_LEN],
                            CheckinReply& out) {
    memset(&out, 0, sizeof(out));
    if (body == nullptr || len > UPDATE_REPLY_MAX) return ReplyKind::Malformed;
    if (nonce == nullptr || strlen(nonce) != UPDATE_NONCE_HEX) return ReplyKind::BadAuth;
    uint8_t nonceBytes[UPDATE_NONCE_HEX / 2];
    if (!decodeHex(nonce, sizeof(nonceBytes), nonceBytes)) return ReplyKind::BadAuth;

    // The auth line is the last line: "auth=" + 64 hex, then an optional LF
    size_t end = len;
    if (end > 0 && body[end - 1] == '\n') end--;
    const size_t authLineLen = 5 + UPDATE_MAC_HEX;
    if (end < authLineLen + 1) return ReplyKind::BadAuth;  // at least one line before it
    size_t authStart = end - authLineLen;
    if (body[authStart - 1] != '\n' || memcmp(body + authStart, "auth=", 5) != 0) return ReplyKind::BadAuth;
    uint8_t got[32];
    if (!decodeHex(body + authStart + 5, sizeof(got), got)) return ReplyKind::BadAuth;

    crypto_blake2b_ctx ctx;
    crypto_blake2b_keyed_init(&ctx, sizeof(got), key, UPDATE_KEY_LEN);
    crypto_blake2b_update(&ctx, (const uint8_t*)nonce, UPDATE_NONCE_HEX);
    crypto_blake2b_update(&ctx, (const uint8_t*)"\n", 1);
    crypto_blake2b_update(&ctx, (const uint8_t*)body, authStart);
    uint8_t want[32];
    crypto_blake2b_final(&ctx, want);
    if (crypto_verify32(got, want) != 0) return ReplyKind::BadAuth;

    // Authenticated: now read the lines before the auth line
    for (size_t i = 0; i < authStart; i++) {
        if (body[i] == '\r' || body[i] == '\0') return ReplyKind::Malformed;
    }
    const char* lines[4];
    const char* ends[4];
    size_t count = 0;
    const char* p = body;
    const char* stop = body + authStart;
    while (p < stop) {
        const char* nl = (const char*)memchr(p, '\n', (size_t)(stop - p));  // always found: stop follows an LF
        if (count == 4) return ReplyKind::Malformed;
        lines[count] = p;
        ends[count] = nl;
        count++;
        p = nl + 1;
    }

    const char* v;
    size_t vLen;
    if (count == 0 || !lineValue(lines[0], ends[0], "status", v, vLen)) return ReplyKind::Malformed;
    if (vLen == 2 && memcmp(v, "ok", 2) == 0) return count == 1 ? ReplyKind::Ok : ReplyKind::Malformed;
    if (vLen != 6 || memcmp(v, "update", 6) != 0 || count != 3) return ReplyKind::Malformed;

    if (!lineValue(lines[1], ends[1], "version", v, vLen) || !tokenOk(v, vLen, UPDATE_VERSION_MAX)) {
        return ReplyKind::Malformed;
    }
    memcpy(out.version, v, vLen);
    if (!lineValue(lines[2], ends[2], "path", v, vLen) || !tokenOk(v, vLen, UPDATE_PATH_MAX) || v[0] != '/') {
        memset(&out, 0, sizeof(out));
        return ReplyKind::Malformed;
    }
    memcpy(out.path, v, vLen);
    return ReplyKind::Update;
}
