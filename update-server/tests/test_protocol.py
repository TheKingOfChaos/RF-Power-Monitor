"""Tests for the check-in wire format (rfupdate.protocol) against vectors.json,
the same vectors the firmware's native test (test/native/test_update_protocol) uses.
Run: python -m unittest discover -s update-server/tests -v
"""

import json
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from rfupdate import protocol  # noqa: E402

with open(os.path.join(HERE, "vectors.json"), encoding="utf-8") as f:
    VECTORS = json.load(f)
KEY = bytes.fromhex(VECTORS["key"])
NONCE = VECTORS["nonce"]
CASES = {c["name"]: c for c in VECTORS["cases"]}


class ProtocolTest(unittest.TestCase):
    def test_mac_matches_vectors(self):
        for name in ("checkin0", "checkin4"):
            c = CASES[name]
            self.assertEqual(c["mac"], protocol.mac_hex(KEY, c["body"].encode()))

    def test_parse_checkin_round_trips_vector(self):
        f = protocol.parse_checkin(CASES["checkin4"]["body"].encode())
        self.assertEqual("E6614103E7452D2F", f["serial"])
        self.assertEqual("rfpm-lab", f["hostname"])
        self.assertEqual("", f["ipv4"])
        self.assertEqual(["2001:db8::5", "2001:db8::6", "fd16:d833:4f0e:3ece::1", "fe80::2acd:c1ff:fe00:1"], f["ipv6"])
        self.assertEqual("3.4.1-hil", f["version"])
        self.assertEqual("B", f["slot"])
        self.assertTrue(f["trial"])
        self.assertEqual("downloading", f["update"])
        self.assertEqual("3.4.1 download connection closed", f["fail"])
        self.assertEqual("installed 3.4.0", f["last"])
        self.assertEqual("ffeeddccbbaa99887766554433221100", f["nonce"])

    def test_parse_checkin_without_ipv6(self):
        f = protocol.parse_checkin(CASES["checkin0"]["body"].encode())
        self.assertEqual([], f["ipv6"])
        self.assertFalse(f["trial"])
        self.assertEqual("10.132.0.58", f["ipv4"])

    def test_parse_checkin_rejects_missing_nonce(self):
        body = CASES["checkin0"]["body"].replace("nonce=" + NONCE + "\n", "")
        with self.assertRaises(ValueError):
            protocol.parse_checkin(body.encode())

    def test_parse_checkin_rejects_bad_nonce(self):
        body = CASES["checkin0"]["body"].replace(NONCE, "xyz")
        with self.assertRaises(ValueError):
            protocol.parse_checkin(body.encode())

    def test_parse_checkin_rejects_lines_out_of_order(self):
        body = CASES["checkin0"]["body"].replace("slot=A\ntrial=0\n", "trial=0\nslot=A\n")
        with self.assertRaises(ValueError):
            protocol.parse_checkin(body.encode())

    def test_parse_checkin_rejects_five_ipv6(self):
        body = CASES["checkin4"]["body"].replace("ipv6=2001:db8::5\n", "ipv6=2001:db8::5\nipv6=2001:db8::7\n")
        with self.assertRaises(ValueError):
            protocol.parse_checkin(body.encode())

    def test_parse_checkin_rejects_body_over_1024_bytes(self):
        body = CASES["checkin0"]["body"].replace("rf-power-monitor", "x" * 1024)
        with self.assertRaises(ValueError):
            protocol.parse_checkin(body.encode())

    def test_parse_checkin_rejects_every_truncation(self):
        body = CASES["checkin0"]["body"].encode()
        for cut in range(len(body) - 1):
            with self.assertRaises(ValueError, msg=str(cut)):
                protocol.parse_checkin(body[:cut])

    def test_parse_checkin_rejects_non_ascii_and_cr(self):
        for bad in (b"\xff", b"\r"):
            body = CASES["checkin0"]["body"].encode().replace(b"rf-power", b"rf" + bad + b"power")
            with self.assertRaises(ValueError):
                protocol.parse_checkin(body)

    def test_build_reply_matches_vector(self):
        self.assertEqual(CASES["reply_ok"]["reply"].encode(), protocol.build_reply(KEY, NONCE, ["status=ok"]))
        self.assertEqual(CASES["reply_update"]["reply"].encode(),
                         protocol.build_reply(KEY, NONCE, ["status=update", "version=3.4.1",
                                                           "path=/releases/3.4.1.upd"]))

    def test_verify_body_mac(self):
        c = CASES["checkin0"]
        self.assertTrue(protocol.verify_mac(KEY, c["body"].encode(), c["mac"]))
        self.assertTrue(protocol.verify_mac(KEY, c["body"].encode(), c["mac"].upper()))
        self.assertFalse(protocol.verify_mac(KEY, c["body"].encode(), "0" * 64))
        self.assertFalse(protocol.verify_mac(KEY, c["body"].encode(), "zz"))
        self.assertFalse(protocol.verify_mac(KEY, c["body"].encode(), None))


if __name__ == "__main__":
    unittest.main()
