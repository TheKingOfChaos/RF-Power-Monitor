"""Tests for the check-in API and the assignment rules (rfupdate.api).
Run: python -m unittest discover -s update-server/tests -v
"""

import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from helpers import KEY, SERIAL, FakeDevice, fake_upd, reply_lines  # noqa: E402
from rfupdate.api import handle_checkin  # noqa: E402
from rfupdate.store import Store  # noqa: E402

T0 = 1_800_000_000.0


class ApiTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(self.tmp.name)
        self.store.set_site_key(KEY)
        self.dev = FakeDevice()

    def tearDown(self):
        self.store.close()
        self.tmp.cleanup()

    def checkin(self, now=T0, dev=None):
        dev = dev or self.dev
        body, auth, nonce = dev.signed()
        code, reply = handle_checkin(self.store, body, auth, now)
        lines = reply_lines(reply, nonce=nonce) if code == 200 else reply.decode().strip().split("\n")
        return code, lines

    def row(self, now=T0, serial=SERIAL):
        return {r["serial"]: r for r in self.store.device_rows(now)}[serial]

    def assign(self, version, now=T0, serials=(SERIAL,)):
        if version not in [r["version"] for r in self.store.releases()]:
            self.store.add_release(fake_upd(version), now=now)
        self.store.assign(list(serials), version, now=now)

    # --- check-ins ---

    def test_checkin_with_valid_mac_adds_device(self):
        self.assertEqual((200, ["status=ok"]), self.checkin())
        r = self.row()
        self.assertEqual("rfpm-2d2f", r["hostname"])
        self.assertEqual("10.132.0.58", r["ipv4"])
        self.assertEqual(["2001:db8::5"], r["ipv6"])
        self.assertEqual("3.4.0", r["version"])
        self.assertTrue(r["online"])

    def test_checkin_same_serial_updates_row_not_new(self):
        self.checkin()
        self.dev.ipv4 = "10.132.0.77"
        self.dev.hostname = "renamed"
        self.checkin(now=T0 + 60)
        rows = self.store.device_rows(T0 + 60)
        self.assertEqual(1, len(rows))
        self.assertEqual("10.132.0.77", rows[0]["ipv4"])
        self.assertEqual("renamed", rows[0]["hostname"])

    def test_checkin_bad_mac_is_401(self):
        body, auth, _ = self.dev.signed()
        code, reply = handle_checkin(self.store, body, "0" * 64, T0)
        self.assertEqual((401, b"status=badauth\n"), (code, reply))
        code, _ = handle_checkin(self.store, body, None, T0)
        self.assertEqual(401, code)
        other = FakeDevice(key=b"\x01" * 32)
        body, auth, _ = other.signed()
        self.assertEqual(401, handle_checkin(self.store, body, auth, T0)[0])
        self.assertEqual([], self.store.device_rows(T0))

    def test_checkin_replayed_nonce_is_409(self):
        body, auth, _ = self.dev.signed()
        self.assertEqual(200, handle_checkin(self.store, body, auth, T0)[0])
        code, reply = handle_checkin(self.store, body, auth, T0 + 5)
        self.assertEqual((409, b"status=replay\n"), (code, reply))

    def test_replayed_nonce_is_refused_for_400_days(self):
        # A captured check-in replayed later could mark an update done or end one
        body, auth, _ = self.dev.signed()
        handle_checkin(self.store, body, auth, T0)
        self.assertEqual(409, handle_checkin(self.store, body, auth, T0 + 86400 + 1)[0])
        self.assertEqual(409, handle_checkin(self.store, body, auth, T0 + 399 * 86400)[0])
        self.assertEqual(200, handle_checkin(self.store, body, auth, T0 + 400 * 86400 + 1)[0])

    def test_checkin_without_site_key_is_401(self):
        self.store.set_site_key(None)
        body, auth, _ = self.dev.signed()
        self.assertEqual(401, handle_checkin(self.store, body, auth, T0)[0])

    def test_checkin_malformed_is_400(self):
        body = b"serial=X\n"
        from rfupdate import protocol
        code, reply = handle_checkin(self.store, body, protocol.mac_hex(KEY, body), T0)
        self.assertEqual((400, b"status=bad\n"), (code, reply))

    # --- device list ---

    def test_device_offline_after_65_min(self):
        self.checkin()
        self.assertTrue(self.row(now=T0 + 3900)["online"])
        self.assertFalse(self.row(now=T0 + 3901)["online"])

    # --- assignments ---

    def test_unassigned_device_gets_ok(self):
        self.assign("3.4.1", serials=())  # a release, but not for this device
        self.assertEqual((200, ["status=ok"]), self.checkin())

    def test_assigned_device_gets_update_with_path(self):
        self.checkin()
        self.assign("3.4.1")
        self.assertEqual("waiting", self.row()["update_status"])
        code, lines = self.checkin(now=T0 + 10)
        self.assertEqual(["status=update", "version=3.4.1", "path=/releases/3.4.1.upd"], lines)
        self.assertEqual("told", self.row(now=T0 + 10)["update_status"])

    def test_assignment_of_running_version_is_done_at_once(self):
        self.checkin()
        self.assign("3.4.0")
        self.assertEqual("done", self.row()["update_status"])
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 10))

    def test_trial_then_kept_marks_done(self):
        self.checkin()
        self.assign("3.4.1")
        self.checkin(now=T0 + 10)  # told
        self.dev.version, self.dev.trial, self.dev.slot = "3.4.1", True, "B"
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 100))  # ok confirms the trial
        self.assertEqual("on trial", self.row(now=T0 + 100)["update_status"])
        self.dev.trial = False
        self.dev.last = "installed 3.4.1"
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 160))
        self.assertEqual("done", self.row(now=T0 + 160)["update_status"])

    def test_rollback_marks_rolled_back_and_stops_sending(self):
        self.dev.last = "rolledback-crash 3.4.1"  # an old rollback of the same version: not this one
        self.checkin()
        self.assign("3.4.1")
        self.assertEqual("status=update", self.checkin(now=T0 + 10)[1][0])
        self.dev.last = "rolledback-unreachable 3.4.1"
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 900))
        r = self.row(now=T0 + 900)
        self.assertEqual("rolled back", r["update_status"])
        self.assertIn("unreachable", r["update_detail"])
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 2700))

    def test_old_rollback_of_same_version_does_not_end_new_assignment(self):
        self.dev.last = "rolledback-crash 3.4.1"  # from an earlier attempt, still in the boot record
        self.checkin()
        self.assign("3.4.1")
        self.assertEqual("status=update", self.checkin(now=T0 + 10)[1][0])  # told
        self.dev.fail = "3.4.1 download connection closed"  # the next check-in still reports the old rollback
        self.assertEqual("status=update", self.checkin(now=T0 + 70)[1][0])
        self.assertEqual("told", self.row(now=T0 + 70)["update_status"])

    def test_refused_ends_assignment(self):
        self.checkin()
        self.assign("3.4.1")
        self.checkin(now=T0 + 10)
        self.dev.fail = "3.4.1 refused bad signature"
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 70))
        r = self.row(now=T0 + 70)
        self.assertEqual("refused", r["update_status"])
        self.assertEqual("bad signature", r["update_detail"])

    def test_site_ends_assignment_after_three_download_failures(self):
        self.checkin()
        self.assign("3.4.1")
        self.checkin(now=T0 + 10)
        for i in range(2):
            self.dev.fail = "3.4.1 download connection closed"
            self.assertEqual("status=update", self.checkin(now=T0 + 70 + i)[1][0])  # tried again
        self.dev.fail = "3.4.1 download no answer"
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 80))
        r = self.row(now=T0 + 80)
        self.assertEqual("failed", r["update_status"])
        self.assertEqual("no answer", r["update_detail"])

    def test_failure_of_another_version_is_ignored(self):
        self.checkin()
        self.assign("3.4.1")
        self.checkin(now=T0 + 10)
        self.dev.fail = "3.4.2 refused bad signature"
        self.assertEqual("status=update", self.checkin(now=T0 + 70)[1][0])

    def test_assignment_lost_after_2_hours(self):
        self.checkin()
        self.assign("3.4.1")
        self.checkin(now=T0 + 10)  # told at T0 + 10
        self.assertEqual("told", self.row(now=T0 + 10 + 7200)["update_status"])
        self.assertEqual("lost", self.row(now=T0 + 10 + 7201)["update_status"])
        self.assertEqual((200, ["status=ok"]), self.checkin(now=T0 + 7300))

    def test_new_assignment_replaces_old(self):
        self.checkin()
        self.assign("3.4.1")
        self.assign("3.4.2", now=T0 + 5)
        self.assertEqual(["status=update", "version=3.4.2", "path=/releases/3.4.2.upd"], self.checkin(now=T0 + 10)[1])

    def test_assign_unknown_release_is_refused(self):
        self.checkin()
        with self.assertRaises(ValueError):
            self.store.assign([SERIAL], "9.9.9", now=T0)

    def test_two_devices_are_independent(self):
        other = FakeDevice(serial="AAAAAAAAAAAAAAAA")
        self.checkin()
        self.checkin(dev=other)
        self.assign("3.4.1", serials=(SERIAL,))
        self.assertEqual("status=update", self.checkin(now=T0 + 10)[1][0])
        self.assertEqual(["status=ok"], self.checkin(now=T0 + 10, dev=other)[1])


if __name__ == "__main__":
    unittest.main()
