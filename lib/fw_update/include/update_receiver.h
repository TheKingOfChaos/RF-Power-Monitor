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
//   and valid: erase sector 0. The blob is then programmed one 256-byte flash
//   page at a time as each page fills (the last one padded with 0xFF); each
//   sector after sector 0 is erased just before its first page. Page 0 (the
//   slot header with the signature) is held in RAM and programmed last, by
//   finish(), only when exactly contentLength bytes have arrived. Until then
//   the slot header reads erased (Empty), and the boot program never starts it.
// The Ed25519 signature is checked later by the caller (slotVerify).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fw_layout.h"

static const uint32_t FW_PAGE_SIZE = 256u;  // flash programming unit
static_assert(FW_SLOT_HEADER_SIZE == FW_PAGE_SIZE, "the slot header is exactly page 0");
static_assert(FW_SECTOR_SIZE % FW_PAGE_SIZE == 0, "pages tile a sector");

/// Flash access for one slot. Offsets are from the start of the slot.
class SlotWriter {
public:
    virtual ~SlotWriter() {}
    /// Erase the 4 KB sector at offsetInSlot (a multiple of FW_SECTOR_SIZE). False on failure.
    virtual bool eraseSector(uint32_t offsetInSlot) = 0;
    /// Program the (erased) page at offsetInSlot (a multiple of FW_PAGE_SIZE) with FW_PAGE_SIZE bytes of `data`.
    /// False on failure.
    virtual bool writePage(uint32_t offsetInSlot, const uint8_t* data) = 0;
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

/// About 600 bytes (two flash pages): keep one static instance, never on the stack.
class UpdateReceiver {
public:
    /// Start receiving `contentLength` bytes for `targetSlot` (0 or 1),
    /// accepting only images signed with `keyId`. Does no flash I/O.
    void begin(uint8_t targetSlot, uint32_t contentLength, const uint8_t keyId[8], SlotWriter& w);
    /// Consume the next bytes. Failed is sticky.
    UpdateStatus feed(const uint8_t* data, size_t len);
    /// End of data: checks the length and writes page 0 (the slot header). Done only on success.
    UpdateStatus finish();
    UpdateError error() const { return _error; }
    /// Firmware version from the target slot header; "" until it is validated.
    const char* version() const { return _version; }

private:
    bool parseFileHeader();
    bool checkSlotHeader();
    bool flushPage(uint32_t offsetInBlob);
    bool fail(UpdateError e);
    bool consume(uint8_t b);

    uint8_t _page[FW_PAGE_SIZE];    // the page being filled (pages 1..n of the blob)
    uint8_t _header[FW_PAGE_SIZE];  // page 0: the slot header, written last
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
