// Streaming receiver for the signed update file (.upd), pure C++ (no heap,
// no exceptions). Bytes arrive in any chunk size; only the target slot's
// blob is written, through a SlotWriter, and a header that lies is rejected
// before the first flash erase.
//
// .upd layout (little endian, see tools/fw_release.py): 64 byte header
//   [0,4) magic 0x50554652 "RFUP"   [4,8) format 1   [8,24) version
//   [24,32) slotOffset[2]  [32,40) slotLength[2]   rest 0xFF
// then the slot A and slot B blobs, each = 256 byte slot header + image.
//
// Order of flash writes (an aborted upload leaves the slot reading Empty):
//   begin(): none. When the file header and the target slot header are in
//   and valid: erase sector 0. Sectors 1..n are erased and written as they
//   fill (the last padded with 0xFF). Sector 0 is held in RAM and written
//   last, by finish(), only when exactly contentLength bytes have arrived.
// The Ed25519 signature is checked later by the caller (slotVerify).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fw_layout.h"

/// Flash access for one slot. Offsets are from the start of the slot.
class SlotWriter {
public:
    virtual ~SlotWriter() {}
    /// Erase the 4 KB sector at offsetInSlot (a multiple of FW_SECTOR_SIZE). False on failure.
    virtual bool eraseSector(uint32_t offsetInSlot) = 0;
    /// Program the (erased) sector at offsetInSlot with `data`, FW_SECTOR_SIZE bytes. False on failure.
    virtual bool writeSector(uint32_t offsetInSlot, const uint8_t* data) = 0;
};

enum class UpdateError : uint8_t {
    None,
    BadMagic,     // not an update file
    BadFormat,    // unknown file format number
    SlotMissing,  // target blob shorter than a slot header
    TooLarge,     // a blob is larger than a slot
    WrongSlot,    // target blob was signed for the other slot
    BadHeader,    // file header inconsistent, or slot header rejected
    WrongKey,     // target slot header is signed with another key than the trusted one
    Truncated,    // fewer bytes than announced
    TooLong,      // more bytes than announced
    WriteFailed,  // the SlotWriter reported failure
};

enum class UpdateStatus : uint8_t { InProgress, Done, Failed };

/// Large (two 4 KB buffers): keep one static instance, never on the stack.
class UpdateReceiver {
public:
    /// Start receiving `contentLength` bytes for `targetSlot` (0 or 1),
    /// accepting only images signed with `keyId`. Does no flash I/O.
    void begin(uint8_t targetSlot, uint32_t contentLength, const uint8_t keyId[8], SlotWriter& w);
    /// Consume the next bytes. Failed is sticky.
    UpdateStatus feed(const uint8_t* data, size_t len);
    /// End of data: checks the length and writes sector 0. Done only on success.
    UpdateStatus finish();
    UpdateError error() const { return _error; }
    /// Firmware version from the target slot header; "" until it is validated.
    const char* version() const { return _version; }

private:
    bool parseFileHeader();
    bool checkSlotHeader();
    bool flushSector(uint32_t sectorIndex);
    bool fail(UpdateError e);
    bool consume(uint8_t b);

    uint8_t _sector[FW_SECTOR_SIZE];  // sectors 1..n of the blob
    uint8_t _first[FW_SECTOR_SIZE];   // sector 0, written last
    uint8_t _fileHeader[64];
    char _version[17];
    uint8_t _keyId[8];
    SlotWriter* _w = nullptr;
    uint32_t _length = 0;       // announced file length
    uint32_t _pos = 0;          // bytes consumed
    uint32_t _blobStart = 0;    // file offset of the target blob
    uint32_t _blobLen = 0;
    uint8_t _slot = 0;
    bool _headerOk = false;      // file header validated
    bool _slotHeaderOk = false;  // slot header validated, sector 0 erased
    UpdateError _error = UpdateError::None;
    bool _finished = false;
};

/// Short lowercase description, e.g. "bad magic".
const char* updateErrorName(UpdateError e);
