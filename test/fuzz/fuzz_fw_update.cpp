/**
 * @file fuzz_fw_update.cpp
 * @brief Fuzz the firmware update receiver, slot header and boot record (lib/fw_update, lib/fw_image)
 *
 * Input:
 *   byte 0  bit 0 target slot, bit 1 expect version "3.6.2", bits 2-3 announced length:
 *           0 exact, 1 one byte more, 2 one byte less, 3 FW_SLOT_SIZE * 2
 *   byte 1  piece size for feed() (0 = 1 byte at a time is too slow: taken as 255)
 *   rest    the update file as uploaded; also read as a slot header and a boot record
 *           (with its CRC made valid, so the fields behind it are reached)
 *
 * The flash fake traps on any erase or write outside the slot or not aligned:
 * an upload from the network must never reach flash it does not own.
 */
#include <boot_decision.h>
#include <boot_record.h>
#include <fw_image.h>
#include <update_receiver.h>

#include "fuzz_common.h"

/// Key id the harness trusts; the seeds sign nothing, they only carry this id
static const uint8_t KEY_ID[8] = {1, 2, 3, 4, 5, 6, 7, 8};

class CheckedFlash : public SlotWriter {
public:
    bool eraseSector(uint32_t offset) override {
        if (offset % FW_SECTOR_SIZE != 0 || offset >= FW_SLOT_SIZE) __builtin_trap();
        return true;
    }
    bool writePage(uint32_t offset, const uint8_t* data) override {
        if (offset % FW_PAGE_SIZE != 0 || offset + FW_PAGE_SIZE > FW_SLOT_SIZE) __builtin_trap();
        volatile uint8_t sink = 0;
        for (uint32_t i = 0; i < FW_PAGE_SIZE; i++) sink ^= data[i];
        return true;
    }
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static UpdateReceiver* rx = new UpdateReceiver();  // ~600 bytes, as the firmware keeps it static
    static CheckedFlash flash;

    FuzzInput in(data, size);
    uint8_t mode = in.byte();
    size_t piece = in.byte();
    if (piece == 0) piece = 255;
    size_t len;
    const uint8_t* file = in.rest(len);

    uint32_t announced = (uint32_t)len;
    switch ((mode >> 2) & 3) {
    case 1: announced = (uint32_t)len + 1; break;
    case 2: announced = len > 0 ? (uint32_t)len - 1 : 0; break;
    case 3: announced = FW_SLOT_SIZE * 2; break;
    }
    rx->begin(mode & 1, announced, KEY_ID, flash, (mode & 2) ? "3.6.2" : nullptr);
    UpdateStatus st = UpdateStatus::InProgress;
    for (size_t off = 0; off < len && st == UpdateStatus::InProgress; off += piece) {
        size_t n = len - off < piece ? len - off : piece;
        std::vector<uint8_t> copy(file + off, file + off + n);  // exact size for ASan
        st = rx->feed(copy.data(), copy.size());
    }
    if (st == UpdateStatus::InProgress) st = rx->finish();
    if (st == UpdateStatus::Done && rx->version()[0] == '\0') __builtin_trap();
    updateErrorName(rx->error());

    // The same bytes as a slot header (as the boot program and the web page read it)
    std::vector<uint8_t> copy(file, file + len);
    SlotHeader h;
    if (slotHeaderParse(copy.data(), copy.size(), mode & 1, KEY_ID, h) == SlotCheck::Ok) {
        if (h.imageLength > FW_IMAGE_MAX || strlen(h.version) == 0 || strlen(h.version) > 16) __builtin_trap();
    }

    // ... and as a boot record, once as is and once with a valid CRC
    if (len >= BOOT_RECORD_SIZE) {
        BootRecord r;
        bootRecordDecode(copy.data(), len, r);
        uint32_t crc = fwCrc32(copy.data(), BOOT_RECORD_SIZE - 4);
        for (int i = 0; i < 4; i++) copy[BOOT_RECORD_SIZE - 4 + i] = (uint8_t)(crc >> (8 * i));
        if (bootRecordDecode(copy.data(), BOOT_RECORD_SIZE, r)) {
            if (r.active > 1 || r.trial == r.active || strlen(r.lastVersion) > 16) __builtin_trap();
        }
    }
    if (len >= 4) {
        uint8_t slot, failed;
        BootReason reason;
        ResetKind reset;
        uint32_t v;
        memcpy(&v, file, 4);
        bootInfoDecode(v, slot, reason, reset, failed);
    }
    return 0;
}
