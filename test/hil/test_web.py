"""Hardware-in-the-loop tests for the configuration web page (TCP port 80).

The device must be in DHCP mode, use community "public" and the device
default web password (status screen: 'Web login: user admin, default
password "..."'). Tests restore whatever they change.

Run:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe -m unittest discover -s test/hil -p "test_web.py" -v

Environment variables:
    RFPOWER_STATIC_IP / RFPOWER_STATIC_MASK / RFPOWER_STATIC_GW  Also test switching to a static
        address from the web page and back (as for test_serial_menu.py)
"""

import http.client
import os
import re
import socket
import statistics
import threading
import time
import unittest
from urllib.parse import quote_plus

from device import Device
from snmp_tools import local_ip_towards, net_snmp, raw_get

LOCKOUT_S = 30


class Web:
    """Minimal browser: one request per connection, like the device expects."""

    def __init__(self, host):
        self.host = host
        self.sid = None

    def request(self, method, path, form=None, sid=None, timeout=5):
        conn = http.client.HTTPConnection(self.host, 80, timeout=timeout)
        headers = {}
        body = None
        if form is not None:
            body = "&".join("%s=%s" % (k, v) for k, v in form.items())
            headers["Content-Type"] = "application/x-www-form-urlencoded"
        cookie = sid if sid is not None else self.sid
        if cookie:
            headers["Cookie"] = "sid=" + cookie
        conn.request(method, path, body=body, headers=headers)
        r = conn.getresponse()
        page = r.read().decode("utf-8")
        conn.close()
        return r.status, dict(r.getheaders()), page

    def login(self, password, user="admin"):
        status, headers, page = self.request("POST", "/login", {"user": user, "password": password}, sid="")
        m = re.search(r"sid=([0-9a-f]{32});", headers.get("Set-Cookie", ""))
        self.sid = m.group(1) if m else None
        return status, page

    def csrf(self):
        status, _, page = self.request("GET", "/")
        m = re.search(r'name="csrf" value="([0-9a-f]{32})"', page)
        return m.group(1) if m else None

    def post(self, path, **fields):
        fields.setdefault("csrf", self.csrf())
        return self.request("POST", path, fields)


class WebPageTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.dev = Device()
        cls.dev.open()
        cls.dev.wait_for_menu()
        st = cls.dev.wait_connected()
        if st["mode"] != "DHCP" or st["community"] != "public" or not st["webDefaultPassword"]:
            cls.dev.close()
            raise unittest.SkipTest("device must be in DHCP mode with community public and the default web password")
        cls.dev.set_web_enabled(True)  # a failed earlier run may have left it off
        cls.ip = st["ip"]
        cls.password = st["webDefaultPassword"]
        cls.pc = local_ip_towards(cls.ip)

    @classmethod
    def tearDownClass(cls):
        cls.dev.close()

    def setUp(self):
        self.dev.wait_for_menu(timeout=10)
        self.web = Web(self.ip)

    def logged_in(self):
        status, page = self.web.login(self.password)
        self.assertEqual(303, status, page)
        self.assertIsNotNone(self.web.sid)
        return self.web

    def wait_lockout_over(self):
        # A previous test may have triggered the lockout; a right password then gets 429
        deadline = time.time() + LOCKOUT_S + 5
        while time.time() < deadline:
            status, _ = Web(self.ip).login(self.password)
            if status == 303:
                return
            time.sleep(2)
        self.fail("web login still locked out")

    # ---- login ----

    def test_start_page_is_the_login_form_without_device_data(self):
        status, headers, page = self.web.request("GET", "/")
        self.assertEqual(200, status)
        self.assertIn('action="/login"', page)
        self.assertNotIn(self.ip, page)
        self.assertNotIn("Restart device", page)
        self.assertIn("default-src 'none'", headers["Content-Security-Policy"])
        self.assertEqual("DENY", headers["X-Frame-Options"])
        self.assertEqual("no-store", headers["Cache-Control"])

    def test_wrong_password_is_refused_and_logged(self):
        self.wait_lockout_over()
        status, page = self.web.login("wrong-" + self.password)
        self.assertEqual(401, status)
        self.assertIsNone(self.web.sid)
        self.assertIn("Wrong user name or password.", page)
        self.dev.wait_for_menu(timeout=5)
        entries = [e for e in self.dev.access_log() if e[0] == self.pc and e[1] == "web login failed"]
        self.assertTrue(entries, "web login failure not in the access log")
        status, _ = self.web.login(self.password, user="root")
        self.assertEqual(401, status)

    def test_login_shows_the_device_status(self):
        self.wait_lockout_over()
        web = self.logged_in()
        status, _, page = web.request("GET", "/")
        self.assertEqual(200, status)
        st = self.dev.status()
        self.assertIn("<td>%s</td>" % self.ip, page)
        self.assertIn(st["mac"], page)
        self.assertRegex(page, r"&middot; v\d+\.\d+\.\d+</span>")
        self.assertIn('">%s</span></td></tr>' % st["mains"], page)  # mains badge
        self.assertIn('value="public"', page)
        self.assertIn('value="dhcp" checked', page)
        # Network card: with DHCP the boxes hold the addresses in use, as on the status screen
        self.assertEqual("DHCP", st["mode"])
        for name, key in (("ip", "ip"), ("mask", "subnet"), ("gateway", "gateway")):
            self.assertRegex(page, r'id="f-%s"[^>]*value="%s"' % (name, re.escape(st[key])))
        self.assertIn("From DHCP, DNS server ", page)

    def test_five_failed_logins_lock_out_for_30_s(self):
        self.wait_lockout_over()
        for i in range(5):
            self.assertEqual(401, self.web.login("wrong-%d" % i)[0])
        status, page = self.web.login(self.password)
        self.assertEqual(429, status, "the right password must also be refused during the lockout")
        self.assertRegex(page, r"Try again in \d+ s")
        self.dev.wait_for_menu(timeout=5)
        self.assertTrue([e for e in self.dev.access_log() if e[0] == self.pc and e[1] == "web locked out"])
        time.sleep(LOCKOUT_S + 1)
        self.assertEqual(303, self.web.login(self.password)[0])

    # ---- forms ----

    def test_actions_need_session_and_csrf_token(self):
        self.wait_lockout_over()
        status, _, _ = self.web.request("POST", "/community", {"community": "hacked1", "csrf": "0" * 32}, sid="")
        self.assertEqual(403, status)
        web = self.logged_in()
        status, _, _ = web.request("POST", "/community", {"community": "hacked2"})
        self.assertEqual(403, status)
        status, _, _ = web.request("POST", "/community", {"community": "hacked3", "csrf": "f" * 32})
        self.assertEqual(403, status)
        self.assertEqual("public", self.dev.status()["community"])

    def test_change_community_applies_immediately(self):
        self.wait_lockout_over()
        web = self.logged_in()
        try:
            status, _, page = web.post("/community", community="web-test%21")
            self.assertEqual(200, status, page)
            self.assertIn("SNMP community saved and active.", page)
            self.assertEqual("web-test!", self.dev.status()["community"])
            rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.5.0", community="web-test!")
            self.assertIn("STRING:", out)
            rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.5.0", community="public")
            self.assertIn("Timeout", out)
        finally:
            status, _, _ = web.post("/community", community="public")
            self.assertEqual(200, status)
        self.assertEqual("public", self.dev.status()["community"])

    def resolve_mdns(self, name, timeout=10):
        """Address of name.local via the PC's mDNS resolver, or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                return socket.getaddrinfo(name + ".local", 80, socket.AF_INET)[0][4][0]
            except socket.gaierror:
                time.sleep(1)
        return None

    def test_hostname_from_web_applies_to_snmp_mdns_and_status(self):
        self.wait_lockout_over()
        web = self.logged_in()
        st = self.dev.status()
        before, contact, location = st["hostname"], st["sysContact"], st["sysLocation"]
        self.assertEqual(self.ip, self.resolve_mdns(before))
        new = "hil-ups-%d" % (int(time.time()) % 10000)  # fresh name: no stale mDNS cache entry
        keep = {"contact": quote_plus(contact), "location": quote_plus(location)}
        try:
            status, _, page = web.post("/hostname", hostname=new, **keep)
            self.assertEqual(200, status, page)
            self.assertIn("<td>%s.local</td>" % new, page)
            st = self.dev.status()
            self.assertEqual(new, st["hostname"])
            self.assertEqual(new, st["sysName"])
            rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.5.0")
            self.assertIn('STRING: "%s"' % new, out)
            self.assertEqual(self.ip, self.resolve_mdns(new), "%s.local does not resolve" % new)
            # Invalid names change nothing
            status, _, _ = web.post("/hostname", hostname="bad name", **keep)
            self.assertEqual(400, status)
            self.assertEqual(new, self.dev.status()["hostname"])
        finally:
            status, _, _ = web.post("/hostname", hostname=before, **keep)
            self.assertEqual(200, status)
        self.assertEqual(before, self.dev.status()["hostname"])

    def test_default_hostname_is_rf_power_and_the_mac_tail(self):
        st = self.dev.status()
        before = st["sysName"]
        default = "RF-Power-" + st["mac"].replace(":", "")[-6:]
        try:
            # '-' in menu option 4 sets the hostname back to the default
            self.dev.set_snmp(st["community"], "-", st["sysContact"], st["sysLocation"])
            st = self.dev.status()
            self.assertEqual(default, st["hostname"])
            self.assertEqual(default, st["sysName"])
            rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.5.0")
            self.assertIn('STRING: "%s"' % default, out)
            self.assertEqual(self.ip, self.resolve_mdns(default.lower()), "%s.local does not resolve" % default)
        finally:
            self.dev.set_snmp(st["community"], "-" if before == default else before, st["sysContact"],
                              st["sysLocation"])
        self.assertEqual(before, self.dev.status()["sysName"])

    def test_contact_and_location_from_web_reach_snmp(self):
        self.wait_lockout_over()
        web = self.logged_in()
        st = self.dev.status()
        hostname, contact, location = st["hostname"], st["sysContact"], st["sysLocation"]
        # No '"' in the values: net-snmp prints it as \" (escaping is covered by the native tests)
        new_contact, new_location = "ops@example.com (HIL)", "Rack 4 <test> & co"
        try:
            status, _, page = web.post("/hostname", hostname=hostname, contact=quote_plus(new_contact),
                                       location=quote_plus(new_location))
            self.assertEqual(200, status, page)
            self.assertIn("contact and location saved and active.", page)
            self.assertIn('value="Rack 4 &lt;test&gt; &amp; co"', page)
            st = self.dev.status()
            self.assertEqual(new_contact, st["sysContact"])
            self.assertEqual(new_location, st["sysLocation"])
            rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.4.0", "1.3.6.1.2.1.1.6.0")
            self.assertIn('STRING: "%s"' % new_contact, out)
            self.assertIn('STRING: "%s"' % new_location, out)
            # Too long: refused, nothing changes
            status, _, _ = web.post("/hostname", hostname=hostname, contact="c" * 64, location="")
            self.assertEqual(400, status)
            self.assertEqual(new_contact, self.dev.status()["sysContact"])
        finally:
            status, _, _ = web.post("/hostname", hostname=hostname, contact=quote_plus(contact),
                                    location=quote_plus(location))
            self.assertEqual(200, status)
        st = self.dev.status()
        self.assertEqual((contact, location), (st["sysContact"], st["sysLocation"]))

    def test_time_and_syslog_from_web_page_apply(self):
        self.wait_lockout_over()
        self.dev.keep_time_syslog(self)
        web = self.logged_in()
        status, _, page = web.post("/timelog", ntp="192.0.2.123", syslog=quote_plus("[2001:db8::5]:6514"))
        self.assertEqual(200, status, page)
        self.assertIn("Time and syslog settings saved and active.", page)
        self.assertIn('value="[2001:db8::5]:6514"', page)
        self.assertEqual(("192.0.2.123", "[2001:db8::5]:6514"), self.dev.time_syslog_setting())
        st = self.dev.status()
        self.assertIn("NTP 192.0.2.123", st["time"])
        self.assertTrue(st["syslog"].startswith("[2001:db8::5]:6514 (typed)"), st["syslog"])
        # A bad value is refused with the reason, and nothing changes
        status, _, page = web.post("/timelog", ntp="a..b", syslog="none")
        self.assertEqual(400, status)
        self.assertIn("NTP server: an IPv4", page)
        self.assertEqual(("192.0.2.123", "[2001:db8::5]:6514"), self.dev.time_syslog_setting())

    def test_invalid_community_is_rejected(self):
        self.wait_lockout_over()
        web = self.logged_in()
        status, _, page = web.post("/community", community="x" * 32)
        self.assertEqual(400, status)
        self.assertIn('alert alert-danger', page)
        self.assertEqual("public", self.dev.status()["community"])

    def test_invalid_static_address_is_rejected_without_reboot(self):
        self.wait_lockout_over()
        web = self.logged_in()
        status, _, page = web.post("/network", mode="static", ip="10.10.6.250", mask="255.255.0.0", gateway="192.168.1.1")
        self.assertEqual(400, status)
        self.assertIn("ateway", page)  # the netconfig message about the gateway
        st = self.dev.status()
        self.assertEqual("DHCP", st["mode"])
        self.assertEqual("connected", st["state"])

    # ---- restarts ----

    def uptime_ticks(self):
        rc, out = net_snmp("snmpget", self.ip, "1.3.6.1.2.1.1.3.0")
        return int(re.search(r"Timeticks: \((\d+)\)", out).group(1))

    def test_reboot_button_restarts_the_device(self):
        self.wait_lockout_over()
        web = self.logged_in()
        before = self.uptime_ticks()
        status, _, page = web.post("/reboot")
        self.assertEqual(200, status)
        self.assertIn("restarts now", page)
        st = self.dev.reconnect_after_reboot()
        self.assertLess(self.uptime_ticks(), before, "sysUpTime did not restart: no reboot")
        self.assertEqual(self.ip, st["ip"])
        self.assertEqual(self.password, st["webDefaultPassword"], "default password must survive a reboot")
        status, _, _ = Web(self.ip).request("GET", "/", sid=web.sid)
        self.assertEqual(200, status)

    def test_saving_dhcp_reboots_and_keeps_the_settings(self):
        self.wait_lockout_over()
        web = self.logged_in()
        status, _, page = web.post("/network", mode="dhcp")
        self.assertEqual(200, status)
        self.assertIn("restarts now with DHCP", page)
        st = self.dev.reconnect_after_reboot()
        self.assertEqual("DHCP", st["mode"])
        self.assertEqual(self.ip, st["ip"])
        # Sessions do not survive a restart
        _, _, page = Web(self.ip).request("GET", "/", sid=web.sid)
        self.assertIn('action="/login"', page)

    @unittest.skipUnless(os.environ.get("RFPOWER_STATIC_IP"), "set RFPOWER_STATIC_IP to a free address to run")
    def test_static_address_from_the_web_page_and_back(self):
        static_ip = os.environ["RFPOWER_STATIC_IP"]
        mask = os.environ.get("RFPOWER_STATIC_MASK", "255.255.0.0")
        gw = os.environ.get("RFPOWER_STATIC_GW", self.dev.status()["gateway"])
        self.wait_lockout_over()
        web = self.logged_in()
        try:
            status, _, page = web.post("/network", mode="static", ip=static_ip, mask=mask, gateway=gw)
            self.assertEqual(200, status, page)
            self.assertIn("reachable at %s" % static_ip, page)
            st = self.dev.reconnect_after_reboot()
            self.assertEqual("Static", st["mode"])
            self.assertEqual(static_ip, st["ip"])
            web = Web(static_ip)
            self.assertEqual(303, web.login(self.password)[0])
            status, _, page = web.post("/network", mode="dhcp")
            self.assertEqual(200, status)
            st = self.dev.reconnect_after_reboot()
        finally:
            if self.dev.status()["mode"] != "DHCP":
                self.dev.send("2", expect=r"\(y/n\): $")
                self.dev.send("y", expect=r"Rebooting", timeout=10)
                st = self.dev.reconnect_after_reboot()
        self.assertEqual("DHCP", st["mode"])

    # ---- password from the serial menu ----

    def test_own_password_from_serial_menu(self):
        self.wait_lockout_over()
        own = "Pico-pass-42"
        web = self.logged_in()
        try:
            out = self.dev.set_web_password(own)
            self.assertIn("Web password saved", out)
            self.assertNotIn(own, out, "the password must not be echoed")
            self.assertIn("*" * len(own), out)
            st = self.dev.status()
            self.assertTrue(st["webOwnPassword"])
            self.assertIsNone(st["webDefaultPassword"])
            self.assertNotIn(self.password, st["raw"])
            # Changing the password logs every browser out
            _, _, page = web.request("GET", "/")
            self.assertIn('action="/login"', page)
            self.assertEqual(401, Web(self.ip).login(self.password)[0])
            t = time.perf_counter()
            self.assertEqual(303, Web(self.ip).login(own)[0])
            self.assertLess(time.perf_counter() - t, 2.0, "hashed password check too slow")
            # Mismatched repeat and invalid passwords change nothing
            self.assertIn("passwords differ", self.dev.set_web_password("Other-pass-1", repeat="Other-pass-2"))
            self.assertIn("Not accepted", self.dev.set_web_password("short"))
            self.assertEqual(303, Web(self.ip).login(own)[0])
        finally:
            self.assertIn("reset to the device default", self.dev.set_web_password("-"))
        st = self.dev.status()
        self.assertEqual(self.password, st["webDefaultPassword"])
        self.assertEqual(303, Web(self.ip).login(self.password)[0])

    def test_new_default_password_from_serial_menu(self):
        # For when the default password became known: the old one must stop working for good
        self.wait_lockout_over()
        old = self.password
        web = self.logged_in()
        self.assertIn("Cancelled", self.dev.regenerate_web_default_password(confirm=False))
        self.assertEqual(old, self.dev.status()["webDefaultPassword"])

        out = self.dev.regenerate_web_default_password()
        new = re.search(r'New default web password: "(\w+)"', out).group(1)
        self.assertNotIn("NOT saved", out)
        type(self).password = new  # later tests in this process log in with the new one
        self.assertNotEqual(old, new)
        self.assertEqual(10, len(new))
        self.assertEqual(new, self.dev.status()["webDefaultPassword"])
        # Logged-in browsers are logged out, the old password is refused, the new one works
        _, _, page = web.request("GET", "/")
        self.assertIn('action="/login"', page)
        self.assertEqual(401, Web(self.ip).login(old)[0])
        self.assertEqual(303, Web(self.ip).login(new)[0])
        # It is stored: still the same after a restart
        self.dev.send("2", expect=r"\(y/n\): $")
        self.dev.send("y", expect=r"Rebooting", timeout=10)
        st = self.dev.reconnect_after_reboot()
        self.assertEqual(new, st["webDefaultPassword"])
        self.assertEqual(303, Web(self.ip).login(new)[0])

    def test_new_default_keeps_an_own_password_in_use(self):
        self.wait_lockout_over()
        own = "Pico-pass-44"
        self.assertIn("Web password saved", self.dev.set_web_password(own))
        try:
            out = self.dev.regenerate_web_default_password()
            new = re.search(r'New default web password: "(\w+)"', out).group(1)
            type(self).password = new
            self.assertIn("Your own password stays in use", out)
            self.assertEqual(303, Web(self.ip).login(own)[0])
            self.assertEqual(401, Web(self.ip).login(new)[0])
        finally:
            self.assertIn("reset to the device default", self.dev.set_web_password("-"))
        self.assertEqual(303, Web(self.ip).login(self.password)[0])

    def test_login_with_own_password_keeps_snmp_fast(self):
        # The own password is a slow salted hash (~0.45 s of SHA-256); it must not block SNMP
        self.wait_lockout_over()
        own = "Pico-pass-43"
        self.assertIn("Web password saved", self.dev.set_web_password(own))
        try:
            for password, expected in ((own, 303), ("wrong-" + own, 401)):
                samples = []
                stop = threading.Event()

                def poll():
                    i = 0
                    while not stop.is_set():
                        resp, secs = raw_get(self.ip, 7300 + i, timeout=2)
                        samples.append(secs if resp is not None else 9.9)
                        i += 1
                        time.sleep(0.04)  # ~25/s: stay under the SNMP rate limit of 50/s per source

                t = threading.Thread(target=poll)
                t.start()
                time.sleep(0.2)
                start = time.perf_counter()
                status, _ = Web(self.ip).login(password)
                login_s = time.perf_counter() - start
                time.sleep(0.2)
                stop.set()
                t.join()
                self.assertEqual(expected, status)
                self.assertGreater(login_s, 0.2, "the hash should take time")
                self.assertLess(max(samples), 0.1, "SNMP blocked during a login: worst %.0f ms" % (max(samples) * 1000))
        finally:
            self.assertIn("reset to the device default", self.dev.set_web_password("-"))

    # ---- stylesheet ----

    def test_bootstrap_stylesheet_is_served_from_flash(self):
        import base64
        import gzip
        import hashlib
        conn = http.client.HTTPConnection(self.ip, 80, timeout=5)
        conn.request("GET", "/static/bootstrap-5.3.3.min.css")
        r = conn.getresponse()
        data = r.read()
        conn.close()
        self.assertEqual(200, r.status)
        self.assertEqual("gzip", r.getheader("Content-Encoding"))
        self.assertIn("immutable", r.getheader("Cache-Control"))
        css = gzip.decompress(data)
        # Exactly Bootstrap 5.3.3's bootstrap.min.css: its official integrity hash
        sri = "sha384-" + base64.b64encode(hashlib.sha384(css).digest()).decode()
        self.assertEqual("sha384-QWTKZyjpPEjISv5WaRU9OFeRpok6YctnYmDr5pNlyT2bRjXh0JMhjY6hW+ALEwIH", sri)
        # 31 KB from flash must not hold up SNMP
        self.assertLess(raw_get(self.ip, 7400)[1], 0.1)

    # ---- web interface on/off, logo ----

    def port_80_open(self):
        try:
            with socket.create_connection((self.ip, 80), timeout=2):
                return True
        except OSError:
            return False

    def test_web_interface_off_from_serial_survives_reboot_and_back_on(self):
        d = self.dev
        try:
            out = d.set_web_enabled(False)
            self.assertIn("Web interface off", out)
            self.assertFalse(d.status()["webEnabled"])
            self.assertFalse(self.port_80_open(), "port 80 still accepts connections")
            self.assertIsNotNone(raw_get(self.ip, 7700)[0], "SNMP must keep working")
            d.send("2", expect=r"\(y/n\): $")            # DHCP save = restart
            d.send("y", expect=r"Rebooting", timeout=10)
            st = d.reconnect_after_reboot()
            self.assertFalse(st["webEnabled"], "off must survive a restart")
            time.sleep(2)
            self.assertFalse(self.port_80_open())
        finally:
            d.wait_for_menu(timeout=10)
            self.assertIn("Web interface on", d.set_web_enabled(True))
        self.assertTrue(self.port_80_open())
        self.logged_in()

    def test_web_page_can_turn_itself_off(self):
        d = self.dev
        web = self.logged_in()
        try:
            status, _, page = web.post("/disable")
            self.assertEqual(200, status)
            self.assertIn("Turn off the web interface?", page)
            self.assertTrue(self.port_80_open(), "the first step must change nothing")
            status, _, page = web.post("/disable", confirm="yes")
            self.assertEqual(200, status)
            self.assertIn("serial menu option 8", page)
            time.sleep(1)
            self.assertFalse(self.port_80_open())
            d.wait_for_menu(timeout=10)
            self.assertFalse(d.status()["webEnabled"])
        finally:
            d.wait_for_menu(timeout=10)
            d.set_web_enabled(True)
        self.assertTrue(self.port_80_open())

    def test_web_reopens_at_once_after_the_device_closed_connections(self):
        # A login (a POST the device reads in full) ends with the device closing the connection, which
        # leaves it in TIME-WAIT on port 80 for 2 minutes; a plain GET does not (lwIP resets instead).
        # Turning the web interface off and on must still open the port at once
        d = self.dev
        self.wait_lockout_over()
        try:
            for cycle in range(3):
                for _ in range(3):
                    self.logged_in()
                time.sleep(0.5)
                d.wait_for_menu(timeout=10)
                d.set_web_enabled(False)
                self.assertFalse(self.port_80_open())
                d.set_web_enabled(True)
                deadline = time.time() + 3
                while not self.port_80_open() and time.time() < deadline:
                    time.sleep(0.2)
                self.assertTrue(self.port_80_open(), "port 80 not open 3 s after turning on (cycle %d)" % cycle)
        finally:
            d.wait_for_menu(timeout=10)
            d.set_web_enabled(True)

    def test_logo_and_favicon(self):
        status, headers, svg = self.web.request("GET", "/favicon.svg")
        self.assertEqual(200, status)
        self.assertEqual("image/svg+xml", headers.get("Content-Type"))
        self.assertTrue(svg.startswith("<svg xmlns=\"http://www.w3.org/2000/svg\""))
        with open(os.path.join(os.path.dirname(__file__), "..", "..", "logo", "RF-canopy-RGB-Orange.svg")) as f:
            logo_path = "".join(re.search(r'<path[^>]*\sd="([^"]+)"', f.read()).group(1).split())
        self.assertIn('d="%s"' % logo_path, svg, "favicon is not the logo from logo/")
        _, _, page = self.web.request("GET", "/")
        self.assertIn('href="/favicon.svg"', page)
        self.assertEqual(2, page.count('d="%s"' % logo_path), "logo in the navbar and on the login card")

    # ---- robustness ----

    def snmp_times(self, count=10):
        times = []
        for i in range(count):
            resp, secs = raw_get(self.ip, 7000 + i)
            self.assertIsNotNone(resp, "SNMP did not answer")
            times.append(secs)
            time.sleep(0.05)
        return times

    def test_slow_client_does_not_stall_snmp_and_is_dropped(self):
        s = socket.create_connection((self.ip, 80), timeout=10)
        try:
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")  # never finishes the request
            times = self.snmp_times()
            self.assertLess(max(times), 0.1, "SNMP slowed by an open web connection: %r" % times)
            # The device gives up after 5 s without answering
            s.settimeout(10)
            start = time.time()
            self.assertEqual(b"", s.recv(100))
            self.assertLess(time.time() - start, 8)
        finally:
            s.close()
        # And serves the next browser normally
        self.assertEqual(200, Web(self.ip).request("GET", "/")[0])

    def test_trickled_upload_without_login_is_dropped_in_seconds(self):
        # A refused upload's body is read and dropped so the client sees the answer, but without a
        # login the client gets 20 s for it (STREAM_DRAIN_TOTAL_MS), not the upload's 5 minutes
        s = socket.create_connection((self.ip, 80), timeout=10)
        start = time.time()
        closed_after = None
        try:
            s.sendall(b"POST /firmware/upload HTTP/1.1\r\nHost: x\r\n"
                      b"Content-Type: application/octet-stream\r\nContent-Length: 800000\r\n\r\n")
            s.settimeout(0.1)
            while time.time() - start < 40:
                try:
                    s.sendall(b"x")  # one byte every 2 s: never idle for the 5 s drain idle limit
                    if s.recv(100) == b"":
                        closed_after = time.time() - start
                        break
                except socket.timeout:
                    pass
                except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError):
                    closed_after = time.time() - start
                    break
                time.sleep(1.9)
        finally:
            s.close()
        self.assertIsNotNone(closed_after, "a trickling upload without login still held the connection after 40 s")
        self.assertGreater(closed_after, 15)
        self.assertLess(closed_after, 26)
        # And serves the next browser normally
        self.assertEqual(200, Web(self.ip).request("GET", "/")[0])

    def test_oversized_request_is_refused(self):
        status, _, _ = self.web.request("POST", "/login", {"user": "admin", "password": "x" * 3000})
        self.assertEqual(413, status)

    def test_page_load_burst_keeps_snmp_fast_and_stack_headroom(self):
        self.wait_lockout_over()
        web = self.logged_in()
        snmp = []
        for i in range(30):
            self.assertEqual(200, web.request("GET", "/")[0])
            snmp.append(raw_get(self.ip, 7100 + i)[1])
        self.assertLess(statistics.median(snmp), 0.02)
        self.assertLess(max(snmp), 0.1)
        free = self.dev.status()["stackNeverUsed"]
        self.assertIsNotNone(free)
        # Core 0 has 4 KB; keep at least 1 KB that was never touched
        self.assertGreater(free, 1024, "core 0 stack headroom only %d bytes" % free)


if __name__ == "__main__":
    unittest.main(verbosity=2)
