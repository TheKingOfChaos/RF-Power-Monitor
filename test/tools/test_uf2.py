"""Tests for scripts/uf2.py (the UF2 writer behind firmware_full.uf2). No hardware."""

import os
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import uf2  # noqa: E402

FLASH = 0x10000000


def blocks_of(chunks):
    """Write the chunks to a temp file; return its 512-byte blocks as dicts."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "out.uf2")
        uf2.write_uf2(path, chunks)
        with open(path, "rb") as f:
            data = f.read()
    out = []
    for i in range(0, len(data), 512):
        b = data[i:i + 512]
        m0, m1, flags, addr, size, num, total, fam = struct.unpack_from("<8I", b)
        out.append(dict(m0=m0, m1=m1, flags=flags, addr=addr, size=size, num=num, total=total,
                        family=fam, payload=b[32:32 + 256], end=struct.unpack_from("<I", b, 508)[0],
                        length=len(b)))
    return out


class Uf2Tests(unittest.TestCase):
    def test_misaligned_chunk_is_rejected(self):
        with self.assertRaises(ValueError):
            blocks_of([(FLASH + 0x10, b"x")])

    def test_chunk_below_flash_is_rejected(self):
        with self.assertRaises(ValueError):
            blocks_of([(0x0, b"x")])

    def test_chunk_past_2_mb_is_rejected(self):
        with self.assertRaises(ValueError):
            blocks_of([(FLASH + 0x1FFF00, b"x" * 257)])

    def test_chunk_ending_exactly_at_2_mb_is_accepted(self):
        blocks = blocks_of([(FLASH + 0x1FFF00, b"x" * 256)])
        self.assertEqual(blocks[0]["addr"], FLASH + 0x1FFF00)

    def test_overlapping_chunks_are_rejected(self):
        with self.assertRaises(ValueError):
            blocks_of([(FLASH, b"a" * 512), (FLASH + 0x100, b"b" * 256)])

    def test_empty_input_is_rejected(self):
        with self.assertRaises(ValueError):
            blocks_of([])

    def test_last_page_is_padded_with_ff(self):
        blocks = blocks_of([(FLASH, b"\x01" * 300)])
        self.assertEqual(len(blocks), 2)
        self.assertEqual(blocks[1]["payload"], b"\x01" * 44 + b"\xff" * 212)

    def test_blocks_are_numbered_in_address_order(self):
        blocks = blocks_of([(FLASH + 0x1000, b"b" * 256), (FLASH, b"a" * 512)])
        self.assertEqual([b["addr"] for b in blocks], [FLASH, FLASH + 0x100, FLASH + 0x1000])
        self.assertEqual([b["num"] for b in blocks], [0, 1, 2])
        self.assertTrue(all(b["total"] == 3 for b in blocks))

    def test_block_framing_and_family(self):
        (b,) = blocks_of([(FLASH, b"z")])
        self.assertEqual((b["m0"], b["m1"], b["end"]), (0x0A324655, 0x9E5D5157, 0x0AB16F30))
        self.assertEqual(b["length"], 512)
        self.assertEqual(b["size"], 256)
        self.assertEqual(b["family"], 0xE48BFF56)
        self.assertEqual(b["flags"], 0x2000)


if __name__ == "__main__":
    unittest.main()
