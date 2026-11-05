// fwsign: sign and verify firmware slot blobs on the PC (Ed25519, Monocypher).
//
//   fwsign pubkey <seedfile>                              print the public key (64 hex)
//   fwsign sign <seedfile> <slot> <version> <image.bin> <out.blob>
//                                                         write slot header + image
//   fwsign verify <pubkeyhex> <slot> <blob>               exit 0 if the device would
//                                                         accept the blob, else print why
//
// The header layout is documented in lib/fw_image/include/fw_image.h. verify
// runs the same slotVerify() the boot program uses. Built by tools/fwtool.py.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "fw_image.h"
#include "monocypher-ed25519.h"

namespace {

/// Print `message` and return 2 (usage or I/O error).
int fail(const char* message) {
    fprintf(stderr, "fwsign: %s\n", message);
    return 2;
}

/// Read a whole file. Returns false if it cannot be read.
bool readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    uint8_t buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

/// Load the 32 byte seed and derive the key pair (the seed copy is wiped).
bool loadKey(const char* seedPath, uint8_t secret[64], uint8_t pub[32]) {
    std::vector<uint8_t> seed;
    if (!readFile(seedPath, seed) || seed.size() != 32) return false;
    uint8_t copy[32];
    memcpy(copy, seed.data(), 32);  // crypto_ed25519_key_pair wipes its seed
    crypto_ed25519_key_pair(secret, pub, copy);
    crypto_wipe(seed.data(), seed.size());
    return true;
}

/// Store a 32 bit value little endian.
void putLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

/// Value of a hex digit, or -1.
int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Parse exactly 64 hex characters into 32 bytes.
bool parseHex32(const char* hex, uint8_t out[32]) {
    if (strlen(hex) != 64) return false;
    for (int i = 0; i < 32; i++) {
        int hi = hexNibble(hex[2 * i]), lo = hexNibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi * 16 + lo);
    }
    return true;
}

/// Parse a slot number, "0" or "1".
bool parseSlot(const char* s, uint32_t& slot) {
    if (strcmp(s, "0") == 0) slot = 0;
    else if (strcmp(s, "1") == 0) slot = 1;
    else return false;
    return true;
}

/// fwsign pubkey: print the public key of the seed file.
int cmdPubkey(const char* seedPath) {
    uint8_t secret[64], pub[32];
    if (!loadKey(seedPath, secret, pub)) return fail("cannot read a 32 byte seed file");
    crypto_wipe(secret, sizeof(secret));
    for (int i = 0; i < 32; i++) printf("%02x", pub[i]);
    printf("\n");
    return 0;
}

/// fwsign sign: write the signed slot blob (header + image).
int cmdSign(const char* seedPath, const char* slotArg, const char* version,
            const char* imagePath, const char* outPath) {
    uint32_t slot;
    if (!parseSlot(slotArg, slot)) return fail("slot must be 0 or 1");
    size_t versionLen = strlen(version);
    if (versionLen == 0 || versionLen > 15) return fail("version must be 1 to 15 characters");
    for (size_t i = 0; i < versionLen; i++) {
        if (version[i] < 0x20 || version[i] > 0x7E) return fail("version must be printable ASCII");
    }
    uint8_t secret[64], pub[32];
    if (!loadKey(seedPath, secret, pub)) return fail("cannot read a 32 byte seed file");
    std::vector<uint8_t> image;
    if (!readFile(imagePath, image)) return fail("cannot read the image");
    if (image.size() > FW_IMAGE_MAX) return fail("image does not fit the slot");

    std::vector<uint8_t> blob(FW_SLOT_HEADER_SIZE + image.size(), 0xFF);
    uint8_t* h = blob.data();
    putLe32(h + 64, FW_MAGIC);
    putLe32(h + 68, FW_HEADER_VERSION);
    putLe32(h + 72, slot);
    putLe32(h + 76, (uint32_t)image.size());
    memset(h + 80, 0, 16);
    memcpy(h + 80, version, versionLen);
    fwKeyId(pub, h + 96);
    memcpy(h + FW_SLOT_HEADER_SIZE, image.data(), image.size());
    crypto_ed25519_sign(h, secret, h + 64, blob.size() - 64);
    crypto_wipe(secret, sizeof(secret));

    FILE* f = fopen(outPath, "wb");
    if (!f) return fail("cannot write the output file");
    bool ok = fwrite(blob.data(), 1, blob.size(), f) == blob.size();
    ok = (fclose(f) == 0) && ok;
    return ok ? 0 : fail("write failed");
}

/// fwsign verify: run the device's slotVerify() on the blob.
int cmdVerify(const char* pubHex, const char* slotArg, const char* blobPath) {
    uint8_t pub[32];
    uint32_t slot;
    if (!parseHex32(pubHex, pub)) return fail("public key must be 64 hex characters");
    if (!parseSlot(slotArg, slot)) return fail("slot must be 0 or 1");
    std::vector<uint8_t> blob;
    if (!readFile(blobPath, blob)) return fail("cannot read the blob");
    if (blob.size() > FW_SLOT_SIZE) {
        printf("%s\n", slotCheckName(SlotCheck::TooLong));
        return 1;
    }
    blob.resize(FW_SLOT_SIZE, 0xFF);  // the device reads the whole slot, erased after the image
    SlotHeader header;
    SlotCheck c = slotVerify(blob.data(), slot, pub, header);
    if (c == SlotCheck::Ok) return 0;
    printf("%s\n", slotCheckName(c));
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "pubkey") == 0) return cmdPubkey(argv[2]);
    if (argc == 7 && strcmp(argv[1], "sign") == 0)
        return cmdSign(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (argc == 5 && strcmp(argv[1], "verify") == 0) return cmdVerify(argv[2], argv[3], argv[4]);
    return fail("usage: fwsign pubkey <seed> | sign <seed> <slot> <version> <image> <out> | "
                "verify <pubkeyhex> <slot> <blob>");
}
