"""Write RP2040 UF2 files from (flash address, bytes) chunks.

A UF2 file is a list of 512-byte blocks, each carrying 256 bytes for one
flash address. The RP2040 boot ROM (BOOTSEL drive) and picotool both take it.
Used by scripts/slot_build.py for firmware_full.uf2 (boot program, boot
record sectors and the slot A image in one file).

    python scripts/uf2.py out.uf2 0x10000000 boot.bin [0x10010000 slot.bin ...]
"""

import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
PAYLOAD_SIZE = 256
DATA_FIELD_SIZE = 476
FLASH_START = 0x10000000
FLASH_END = 0x10200000  # 2 MB


def _pages(chunks):
    """Split the chunks into (address, 256 bytes) pages, sorted by address.

    Each chunk must start on a 256-byte boundary inside flash; its last page
    is padded with 0xFF (the erased value). Raises ValueError for a chunk
    outside flash, a misaligned chunk or chunks that overlap.
    """
    pages = []
    for address, data in chunks:
        if address % PAYLOAD_SIZE:
            raise ValueError("chunk at 0x%08x is not 256-byte aligned" % address)
        if address < FLASH_START or address + len(data) > FLASH_END:
            raise ValueError("chunk at 0x%08x (%d bytes) is outside flash" % (address, len(data)))
        for off in range(0, len(data), PAYLOAD_SIZE):
            page = data[off:off + PAYLOAD_SIZE]
            pages.append((address + off, page + b"\xff" * (PAYLOAD_SIZE - len(page))))
    pages.sort(key=lambda p: p[0])
    for (a, _), (b, _) in zip(pages, pages[1:]):
        if a == b:
            raise ValueError("chunks overlap at 0x%08x" % a)
    return pages


def write_uf2(path: str, chunks: list[tuple[int, bytes]]) -> None:
    """Write `chunks` (flash address, bytes) to `path` as an RP2040 UF2 file.

    Family ID 0xe48bff56 (RP2040). Raises ValueError for bad chunks (see
    _pages) and when there is nothing to write.
    """
    pages = _pages(chunks)
    if not pages:
        raise ValueError("no data for %s" % path)
    blocks = []
    for number, (address, page) in enumerate(pages):
        header = struct.pack("<IIIIIIII", UF2_MAGIC_START0, UF2_MAGIC_START1,
                             UF2_FLAG_FAMILY_ID_PRESENT, address, PAYLOAD_SIZE,
                             number, len(pages), RP2040_FAMILY_ID)
        data = page + b"\x00" * (DATA_FIELD_SIZE - PAYLOAD_SIZE)
        blocks.append(header + data + struct.pack("<I", UF2_MAGIC_END))
    with open(path, "wb") as f:
        f.write(b"".join(blocks))


def main():
    """Command line: write_uf2 for address/file pairs."""
    args = sys.argv[1:]
    if len(args) < 3 or len(args) % 2 == 0:
        sys.exit(__doc__)
    chunks = []
    for i in range(1, len(args), 2):
        with open(args[i + 1], "rb") as f:
            chunks.append((int(args[i], 0), f.read()))
    write_uf2(args[0], chunks)


if __name__ == "__main__":
    main()
