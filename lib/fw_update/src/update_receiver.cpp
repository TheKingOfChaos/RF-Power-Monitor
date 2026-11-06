#include "update_receiver.h"

#include <string.h>

#include "fw_image.h"

namespace {

const uint32_t UPD_MAGIC = 0x50554652u;
const uint32_t UPD_FORMAT = 1u;
const uint32_t UPD_HEADER_SIZE = 64u;

/// Little endian u32 at p.
uint32_t getLe32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

}  // namespace

const char* updateErrorName(UpdateError e) {
    switch (e) {
        case UpdateError::None: return "none";
        case UpdateError::BadMagic: return "bad magic";
        case UpdateError::BadFormat: return "unknown format";
        case UpdateError::SlotMissing: return "slot image missing";
        case UpdateError::TooLarge: return "image too large";
        case UpdateError::WrongSlot: return "wrong slot";
        case UpdateError::BadHeader: return "bad header";
        case UpdateError::WrongKey: return "different key";
        case UpdateError::Truncated: return "truncated";
        case UpdateError::TooLong: return "too long";
        case UpdateError::WriteFailed: return "write failed";
    }
    return "unknown";
}

void UpdateReceiver::begin(uint8_t targetSlot, uint32_t contentLength, const uint8_t keyId[8],
                           SlotWriter& w) {
    memset(_header, 0xFF, sizeof(_header));
    memset(_page, 0xFF, sizeof(_page));
    memset(_fileHeader, 0, sizeof(_fileHeader));
    _version[0] = 0;
    memcpy(_keyId, keyId, 8);
    _w = &w;
    _length = contentLength;
    _pos = 0;
    _blobStart = 0;
    _blobLen = 0;
    _slot = targetSlot;
    _headerOk = false;
    _slotHeaderOk = false;
    _error = UpdateError::None;
    _finished = false;
}

/// Record the first error (later ones keep it) and return false.
bool UpdateReceiver::fail(UpdateError e) {
    if (_error == UpdateError::None) _error = e;
    return false;
}

/// Validate the 64 byte file header; sets _blobStart/_blobLen. No flash I/O.
bool UpdateReceiver::parseFileHeader() {
    if (getLe32(&_fileHeader[0]) != UPD_MAGIC) return fail(UpdateError::BadMagic);
    if (getLe32(&_fileHeader[4]) != UPD_FORMAT) return fail(UpdateError::BadFormat);
    if (_slot > 1) return fail(UpdateError::BadHeader);
    uint32_t off[2] = {getLe32(&_fileHeader[24]), getLe32(&_fileHeader[28])};
    uint32_t len[2] = {getLe32(&_fileHeader[32]), getLe32(&_fileHeader[36])};
    for (int i = 0; i < 2; i++) {
        if (len[i] < FW_SLOT_HEADER_SIZE) {
            return fail(i == _slot ? UpdateError::SlotMissing : UpdateError::BadHeader);
        }
        if (len[i] > FW_SLOT_SIZE) return fail(UpdateError::TooLarge);
        // 64 bit sums: these fields come from the network
        if (off[i] < UPD_HEADER_SIZE || (uint64_t)off[i] + len[i] > _length) {
            return fail(UpdateError::BadHeader);
        }
    }
    if ((uint64_t)off[0] + len[0] > off[1] && (uint64_t)off[1] + len[1] > off[0]) {
        return fail(UpdateError::BadHeader);  // blobs overlap
    }
    _blobStart = off[_slot];
    _blobLen = len[_slot];
    _headerOk = true;
    return true;
}

/// Validate the target slot header (page 0, in _header), then erase sector 0.
bool UpdateReceiver::checkSlotHeader() {
    SlotHeader h;
    SlotCheck c = slotHeaderParse(_header, FW_SLOT_HEADER_SIZE, _slot, _keyId, h);
    if (c == SlotCheck::WrongSlot) return fail(UpdateError::WrongSlot);
    if (c == SlotCheck::WrongKey) return fail(UpdateError::WrongKey);
    if (c != SlotCheck::Ok) return fail(UpdateError::BadHeader);
    if ((uint64_t)FW_SLOT_HEADER_SIZE + h.imageLength != _blobLen) return fail(UpdateError::BadHeader);
    memcpy(_version, h.version, sizeof(_version));
    _slotHeaderOk = true;
    if (!_w->eraseSector(0)) return fail(UpdateError::WriteFailed);
    return true;
}

/// Program one of pages 1..n from _page (page 0 is held back); erase its sector first when the
/// page opens a sector after sector 0 (sector 0 was erased with the header check).
bool UpdateReceiver::flushPage(uint32_t offsetInBlob) {
    if (offsetInBlob % FW_SECTOR_SIZE == 0 && !_w->eraseSector(offsetInBlob)) return fail(UpdateError::WriteFailed);
    if (!_w->writePage(offsetInBlob, _page)) return fail(UpdateError::WriteFailed);
    memset(_page, 0xFF, sizeof(_page));
    return true;
}

/// Take one byte of the file: collect the file header, then route the target blob's bytes
/// into the page buffers, programming each page as it completes. False once failed.
bool UpdateReceiver::consume(uint8_t b) {
    uint32_t pos = _pos++;
    if (pos < UPD_HEADER_SIZE) {
        _fileHeader[pos] = b;
        return pos + 1 < UPD_HEADER_SIZE || parseFileHeader();
    }
    if (!_headerOk || pos < _blobStart || pos - _blobStart >= _blobLen) return true;  // not ours
    uint32_t r = pos - _blobStart;
    uint32_t inPage = r % FW_PAGE_SIZE;
    if (r < FW_PAGE_SIZE) {
        _header[inPage] = b;
        return r + 1 < FW_SLOT_HEADER_SIZE || checkSlotHeader();
    }
    _page[inPage] = b;
    // a page is complete when full or at the blob's last byte
    if (inPage + 1 == FW_PAGE_SIZE || r + 1 == _blobLen) return flushPage(r - inPage);
    return true;
}

UpdateStatus UpdateReceiver::feed(const uint8_t* data, size_t len) {
    if (_error != UpdateError::None || _finished) return UpdateStatus::Failed;
    for (size_t i = 0; i < len; i++) {
        if (_pos >= _length) {
            fail(UpdateError::TooLong);
            return UpdateStatus::Failed;
        }
        if (!consume(data[i])) return UpdateStatus::Failed;
    }
    return UpdateStatus::InProgress;
}

UpdateStatus UpdateReceiver::finish() {
    if (_error != UpdateError::None || _finished) return UpdateStatus::Failed;
    _finished = true;
    if (_pos != _length || !_slotHeaderOk) {
        fail(UpdateError::Truncated);
        return UpdateStatus::Failed;
    }
    if (!_w->writePage(0, _header)) {
        fail(UpdateError::WriteFailed);
        return UpdateStatus::Failed;
    }
    return UpdateStatus::Done;
}
