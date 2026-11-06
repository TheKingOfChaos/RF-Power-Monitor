"""Tests for the site's web pages, login and HTTP routes (rfupdate.web), against a real
server on a free local port.
Run: python -m unittest discover -s update-server/tests -v
"""

import http.client
import os
import re
import sys
import tempfile
import threading
import unittest
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
from helpers import KEY, SERIAL, FakeDevice, fake_upd, reply_lines  # noqa: E402
from rfupdate import upd  # noqa: E402
from rfupdate.store import Store  # noqa: E402
from rfupdate.web import make_server  # noqa: E402

PASSWORD = "correct horse"


class WebTest(unittest.TestCase):
    password = PASSWORD

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.store = Store(self.tmp.name)
        self.store.set_site_key(KEY)
        self.server = make_server(self.store, "127.0.0.1", 0, self.password)
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.store.close()
        self.tmp.cleanup()

    def request(self, method, path, body=None, headers=None, cookie=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        h = dict(headers or {})
        if cookie:
            h["Cookie"] = "sid=" + cookie
        conn.request(method, path, body=body, headers=h)
        r = conn.getresponse()
        data = r.read()
        result = (r.status, dict((k.lower(), v) for k, v in r.getheaders()), data)
        conn.close()
        return result

    def form(self, path, fields, cookie=None):
        from urllib.parse import urlencode
        return self.request("POST", path, urlencode(fields, doseq=True).encode(),
                            {"Content-Type": "application/x-www-form-urlencoded"}, cookie)

    def login(self):
        status, headers, _ = self.form("/login", {"password": self.password})
        self.assertEqual(303, status)
        sid = re.search(r"sid=([^;]+)", headers["set-cookie"]).group(1)
        _, _, page = self.request("GET", "/admin", cookie=sid)
        csrf = re.search(rb'name="csrf" value="([^"]+)"', page).group(1).decode()
        return sid, csrf

    def upload(self, data, sid, csrf, filename="fw.upd"):
        boundary = uuid.uuid4().hex
        body = (("--%s\r\nContent-Disposition: form-data; name=\"csrf\"\r\n\r\n%s\r\n" % (boundary, csrf)).encode()
                + ("--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"%s\"\r\n"
                   "Content-Type: application/octet-stream\r\n\r\n" % (boundary, filename)).encode()
                + data + ("\r\n--%s--\r\n" % boundary).encode())
        return self.request("POST", "/admin/upload", body,
                            {"Content-Type": "multipart/form-data; boundary=" + boundary}, sid)

    def checkin(self, dev):
        body, auth, nonce = dev.signed()
        status, _, reply = self.request("POST", "/api/checkin", body,
                                        {"Content-Type": "text/plain", "X-RFPower-Auth": auth})
        return status, reply, nonce

    # --- access ---

    def test_device_list_needs_no_login(self):
        self.checkin(FakeDevice())
        status, _, page = self.request("GET", "/")
        self.assertEqual(200, status)
        self.assertIn(SERIAL.encode(), page)
        self.assertIn(b"10.132.0.58", page)
        self.assertIn(b"online", page)
        self.assertNotIn(b"/admin/upload", page)

    def test_device_list_escapes_device_fields(self):
        dev = FakeDevice()
        dev.hostname = "<script>alert(1)</script>"
        dev.fail = "3.4.1 refused <b>x</b>"
        self.checkin(dev)
        _, _, page = self.request("GET", "/")
        self.assertNotIn(b"<script>alert", page)
        self.assertIn(b"&lt;script&gt;", page)

    def test_admin_needs_login(self):
        status, headers, _ = self.request("GET", "/admin")
        self.assertEqual(303, status)
        self.assertEqual("/login", headers["location"])
        status, _, _ = self.request("GET", "/admin", cookie="forged")
        self.assertEqual(303, status)

    def test_wrong_password_is_refused(self):
        status, headers, page = self.form("/login", {"password": "wrong"})
        self.assertEqual(401, status)
        self.assertNotIn("set-cookie", headers)

    def test_logout_ends_session(self):
        sid, csrf = self.login()
        self.assertEqual(303, self.form("/logout", {"csrf": csrf}, sid)[0])
        self.assertEqual(303, self.request("GET", "/admin", cookie=sid)[0])

    def test_session_cookie_is_httponly_and_strict(self):
        _, headers, _ = self.form("/login", {"password": self.password})
        self.assertIn("HttpOnly", headers["set-cookie"])
        self.assertIn("SameSite=Strict", headers["set-cookie"])

    def test_upload_needs_login_and_csrf(self):
        data = fake_upd("3.4.1")
        self.assertEqual(403, self.upload(data, None, "x")[0])
        sid, csrf = self.login()
        self.assertEqual(403, self.upload(data, sid, "wrong")[0])
        self.assertEqual([], self.store.releases())
        self.assertEqual(200, self.upload(data, sid, csrf)[0])
        self.assertEqual(["3.4.1"], [r["version"] for r in self.store.releases()])

    def test_assign_needs_login_and_csrf(self):
        self.checkin(FakeDevice())
        self.store.add_release(fake_upd("3.4.1"), now=0)
        self.assertEqual(403, self.form("/admin/assign", {"serial": SERIAL, "version": "3.4.1", "csrf": "x"})[0])
        sid, csrf = self.login()
        self.assertEqual(403, self.form("/admin/assign", {"serial": SERIAL, "version": "3.4.1", "csrf": "no"}, sid)[0])
        self.assertIsNone(self.store.device_rows(1e12)[0]["update_status"])
        self.assertEqual(200, self.form("/admin/assign", {"serial": SERIAL, "version": "3.4.1", "csrf": csrf}, sid)[0])
        self.assertEqual("waiting", self.store.device_rows(1e10)[0]["update_status"])

    def test_login_rate_limited_after_5_failures(self):
        for _ in range(5):
            self.assertEqual(401, self.form("/login", {"password": "wrong"})[0])
        status, headers, _ = self.form("/login", {"password": self.password})
        self.assertEqual(429, status)
        self.assertNotIn("set-cookie", headers)

    def test_upload_errors_are_shown(self):
        sid, csrf = self.login()
        status, _, page = self.upload(fake_upd("3.3.0"), sid, csrf)
        self.assertEqual(400, status)
        self.assertIn(b"older than 3.4.0", page)
        status, _, page = self.upload(b"not a release" * 10, sid, csrf)
        self.assertEqual(400, status)
        self.assertIn(b"not a .upd file", page)

    def test_upload_then_assign_then_checkin_gets_update(self):
        dev = FakeDevice()
        self.checkin(dev)
        sid, csrf = self.login()
        self.assertEqual(200, self.upload(fake_upd("3.4.1"), sid, csrf)[0])
        _, _, admin = self.request("GET", "/admin", cookie=sid)
        self.assertIn(('name="serial" value="%s"' % SERIAL).encode(), admin)
        self.assertIn(b'<option value="3.4.1">', admin)
        self.form("/admin/assign", {"serial": SERIAL, "version": "3.4.1", "csrf": csrf}, sid)
        status, reply, nonce = self.checkin(dev)
        self.assertEqual(200, status)
        self.assertEqual(["status=update", "version=3.4.1", "path=/releases/3.4.1.upd"],
                         reply_lines(reply, nonce=nonce))
        _, _, page = self.request("GET", "/")
        self.assertIn(b"told", page)

    def test_checkin_errors_over_http(self):
        dev = FakeDevice()
        body, auth, _ = dev.signed()
        status, _, reply = self.request("POST", "/api/checkin", body, {"X-RFPower-Auth": "0" * 64})
        self.assertEqual((401, b"status=badauth\n"), (status, reply))
        status, _, _ = self.request("POST", "/api/checkin", b"x" * 2000, {"X-RFPower-Auth": auth})
        self.assertEqual(400, status)
        self.assertEqual(405, self.request("GET", "/api/checkin")[0])

    def test_release_download_serves_exact_bytes(self):
        data = fake_upd("3.4.1", blob_a=bytes(range(256)) * 10)
        self.store.add_release(data, now=0)
        status, headers, got = self.request("GET", "/releases/3.4.1.upd")
        self.assertEqual(200, status)
        self.assertEqual(data, got)
        self.assertEqual(str(len(data)), headers["content-length"])
        self.assertEqual(404, self.request("GET", "/releases/3.4.9.upd")[0])
        self.assertEqual(404, self.request("GET", "/releases/..%2Frfupdate.db")[0])
        self.assertEqual(404, self.request("GET", "/releases/../rfupdate.db")[0])

    def test_upload_over_max_is_413(self):
        sid, csrf = self.login()
        status, _, _ = self.request("POST", "/admin/upload", b"",
                                    {"Content-Type": "multipart/form-data; boundary=x",
                                     "Content-Length": str(upd.MAX_UPLOAD + 70000)}, sid)
        self.assertEqual(413, status)

    def test_key_page_shows_new_key_once(self):
        self.store.set_site_key(None)
        sid, csrf = self.login()
        _, _, admin = self.request("GET", "/admin", cookie=sid)
        self.assertIn(b"Shared key: not set", admin)
        status, _, page = self.form("/admin/key/new", {"csrf": csrf}, sid)
        self.assertEqual(200, status)
        key = self.store.site_key().hex().encode()
        self.assertIn(key, page)
        _, _, admin = self.request("GET", "/admin", cookie=sid)
        self.assertNotIn(key, admin)
        self.assertIn(b"Shared key: set", admin)
        self.assertEqual(403, self.form("/admin/key/new", {"csrf": "x"}, sid)[0])

    def test_delete_release(self):
        self.store.add_release(fake_upd("3.4.1"), now=0)
        sid, csrf = self.login()
        self.assertEqual(200, self.form("/admin/release/delete", {"version": "3.4.1", "csrf": csrf}, sid)[0])
        self.assertEqual([], self.store.releases())


class NoPasswordTest(WebTest):
    """Without RFUPDATE_ADMIN_PASSWORD nobody can log in; the device list still works."""
    password = None

    def test_no_login_without_admin_password_env(self):
        status, headers, page = self.form("/login", {"password": ""})
        self.assertEqual(403, status)
        self.assertNotIn("set-cookie", headers)
        self.assertIn(b"RFUPDATE_ADMIN_PASSWORD", self.request("GET", "/login")[2])
        self.assertEqual(200, self.request("GET", "/")[0])


# The inherited tests need a login; NoPasswordTest runs only its own
for _name in [n for n in dir(WebTest) if n.startswith("test_")]:
    if _name not in ("test_device_list_needs_no_login", "test_device_list_escapes_device_fields",
                     "test_admin_needs_login", "test_checkin_errors_over_http"):
        setattr(NoPasswordTest, _name, None)


if __name__ == "__main__":
    unittest.main()
