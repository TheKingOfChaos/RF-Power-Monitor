#include "fw_image.h"

#include <string.h>

#include "monocypher-ed25519.h"

namespace {

const size_t OFF_SIGNATURE = 0;
const size_t OFF_MAGIC = 64;
const size_t OFF_HEADER_VERSION = 68;
const size_t OFF_SLOT = 72;
const size_t OFF_LENGTH = 76;
const size_t OFF_VERSION = 80;
const size_t VERSION_FIELD = 16;
const size_t OFF_KEY_ID = 96;

/// Read a little endian 32 bit value.
uint32_t readLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/// True if every byte is 0xFF (erased flash).
bool allErased(const uint8_t* p, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (p[i] != 0xFF) return false;
    }
    return true;
}

}  // namespace

SlotCheck slotHeaderParse(const uint8_t* bytes, size_t len, uint32_t expectedSlot,
                          const uint8_t keyId[8], SlotHeader& out) {
    if (bytes == nullptr || len < FW_SLOT_HEADER_SIZE) {
        return (bytes != nullptr && allErased(bytes, len)) ? SlotCheck::Empty
                                                           : SlotCheck::BadMagic;
    }
    if (allErased(bytes, FW_SLOT_HEADER_SIZE)) return SlotCheck::Empty;
    if (readLe32(bytes + OFF_MAGIC) != FW_MAGIC) return SlotCheck::BadMagic;
    if (readLe32(bytes + OFF_HEADER_VERSION) != FW_HEADER_VERSION) return SlotCheck::BadMagic;

    uint32_t slot = readLe32(bytes + OFF_SLOT);
    if (slot != expectedSlot) return SlotCheck::WrongSlot;

    uint32_t length = readLe32(bytes + OFF_LENGTH);
    if (length > FW_IMAGE_MAX) return SlotCheck::TooLong;

    // Version: printable ASCII, then NUL padding to the end of the field.
    size_t n = 0;
    while (n < VERSION_FIELD && bytes[OFF_VERSION + n] >= 0x20 && bytes[OFF_VERSION + n] <= 0x7E) {
        n++;
    }
    if (n == 0 || n == VERSION_FIELD) return SlotCheck::BadVersion;  // empty or unterminated
    for (size_t i = n; i < VERSION_FIELD; i++) {
        if (bytes[OFF_VERSION + i] != 0) return SlotCheck::BadVersion;
    }

    if (memcmp(bytes + OFF_KEY_ID, keyId, 8) != 0) return SlotCheck::WrongKey;

    out.slot = slot;
    out.imageLength = length;
    memcpy(out.version, bytes + OFF_VERSION, n);
    memset(out.version + n, 0, sizeof(out.version) - n);
    memcpy(out.keyId, bytes + OFF_KEY_ID, 8);
    return SlotCheck::Ok;
}

SlotCheck slotVerify(const uint8_t* slot, uint32_t expectedSlot,
                     const uint8_t publicKey[32], SlotHeader& out) {
    uint8_t keyId[8];
    fwKeyId(publicKey, keyId);
    SlotHeader h;
    memset(&h, 0, sizeof(h));
    SlotCheck c = slotHeaderParse(slot, FW_SLOT_SIZE, expectedSlot, keyId, h);
    if (c != SlotCheck::Ok) return c;

    const uint8_t* message = slot + OFF_MAGIC;
    size_t messageLen = FW_SLOT_HEADER_SIZE - OFF_MAGIC + h.imageLength;
    if (crypto_ed25519_check(slot + OFF_SIGNATURE, publicKey, message, messageLen) != 0) {
        return SlotCheck::BadSignature;
    }
    out = h;
    return SlotCheck::Ok;
}

void fwKeyId(const uint8_t publicKey[32], uint8_t out[8]) {
    uint8_t hash[64];
    crypto_sha512(hash, publicKey, 32);
    memcpy(out, hash, 8);
}

void fwKeyFingerprint(const uint8_t publicKey[32], char out[17]) {
    static const char HEX[] = "0123456789abcdef";
    uint8_t id[8];
    fwKeyId(publicKey, id);
    for (size_t i = 0; i < 8; i++) {
        out[2 * i] = HEX[id[i] >> 4];
        out[2 * i + 1] = HEX[id[i] & 15];
    }
    out[16] = '\0';
}

const char* slotCheckName(SlotCheck c) {
    switch (c) {
        case SlotCheck::Ok: return "ok";
        case SlotCheck::Empty: return "empty";
        case SlotCheck::BadMagic: return "bad magic";
        case SlotCheck::WrongSlot: return "wrong slot";
        case SlotCheck::TooLong: return "too long";
        case SlotCheck::BadVersion: return "bad version";
        case SlotCheck::WrongKey: return "wrong key";
        case SlotCheck::BadSignature: return "bad signature";
    }
    return "unknown";
}
