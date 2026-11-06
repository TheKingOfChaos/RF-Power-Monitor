#include "update_receiver.h"

#include <string.h>

#include "fw_image.h"

namespace {

const uint32_t UPD_MAGIC = 0x50554652u;
const uint32_t UPD_FORMAT = 1u;
const uint32_t UPD_HEADER_SIZE = 64u;

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
        case UpdateError::Truncated: return "truncated";
        case UpdateError::TooLong: return "too long";
        case UpdateError::WriteFailed: return "write failed";
    }
    return "unknown";
}

void UpdateReceiver::begin(uint8_t targetSlot, uint32_t contentLength, const uint8_t keyId[8],
                           SlotWriter& w) {
    memset(_first, 0xFF, sizeof(_first));
    memset(_sector, 0xFF, sizeof(_sector));
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

/// Validate the target slot header (first 256 bytes in _first), then erase sector 0.
bool UpdateReceiver::checkSlotHeader() {
    SlotHeader h;
    SlotCheck c = slotHeaderParse(_first, FW_SLOT_HEADER_SIZE, _slot, _keyId, h);
    if (c == SlotCheck::WrongSlot) return fail(UpdateError::WrongSlot);
    if (c != SlotCheck::Ok) return fail(UpdateError::BadHeader);
    if ((uint64_t)FW_SLOT_HEADER_SIZE + h.imageLength != _blobLen) return fail(UpdateError::BadHeader);
    memcpy(_version, h.version, sizeof(_version));
    _slotHeaderOk = true;
    if (!_w->eraseSector(0)) return fail(UpdateError::WriteFailed);
    return true;
}

/// Erase and write one of sectors 1..n from _sector (sector 0 is held back).
bool UpdateReceiver::flushSector(uint32_t sectorIndex) {
    uint32_t off = sectorIndex * FW_SECTOR_SIZE;
    if (!_w->eraseSector(off) || !_w->writeSector(off, _sector)) return fail(UpdateError::WriteFailed);
    memset(_sector, 0xFF, sizeof(_sector));
    return true;
}

bool UpdateReceiver::consume(uint8_t b) {
    uint32_t pos = _pos++;
    if (pos < UPD_HEADER_SIZE) {
        _fileHeader[pos] = b;
        return pos + 1 < UPD_HEADER_SIZE || parseFileHeader();
    }
    if (!_headerOk || pos < _blobStart || pos - _blobStart >= _blobLen) return true;  // not ours
    uint32_t r = pos - _blobStart;
    uint32_t sector = r / FW_SECTOR_SIZE;
    uint32_t inSector = r % FW_SECTOR_SIZE;
    (sector == 0 ? _first : _sector)[inSector] = b;
    if (r + 1 == FW_SLOT_HEADER_SIZE && !checkSlotHeader()) return false;
    // a sector is complete when full or at the blob's last byte
    if (sector > 0 && (inSector + 1 == FW_SECTOR_SIZE || r + 1 == _blobLen)) return flushSector(sector);
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
    if (!_w->writeSector(0, _first)) {
        fail(UpdateError::WriteFailed);
        return UpdateStatus::Failed;
    }
    return UpdateStatus::Done;
}
