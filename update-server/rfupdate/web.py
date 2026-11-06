"""The update site over HTTP: the device list (no login), the admin page (login),
release downloads and the device check-in API.

Routes:
  GET  /                      devices, online/offline and update status (no login)
  GET  /login, POST /login    admin password (RFUPDATE_ADMIN_PASSWORD); 5 failures a minute lock logins
  POST /logout
  GET  /admin                 releases, upload, assign, shared key
  POST /admin/upload          multipart form, field "file" (a .upd)
  POST /admin/assign          fields "serial" (repeated) and "version"
  POST /admin/release/delete  field "version"
  POST /admin/key/new         a new shared key, shown once
  GET  /releases/<v>.upd      a release file (no login: the device checks the signature)
  POST /api/checkin           device check-in (api.py)
Every admin POST needs the session cookie and the session's CSRF token.
"""

import hmac
import html
import os
import secrets
import socket
import time
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote

from . import api, protocol, upd

SESSION_S = 12 * 3600
LOGIN_FAILURES_MAX = 5
LOGIN_WINDOW_S = 60
FORM_MAX = 64 * 1024
UPLOAD_MAX = upd.MAX_UPLOAD + 64 * 1024  # the file plus the multipart framing

STYLE = """
body{font-family:system-ui,sans-serif;margin:0;background:#f5f6f8;color:#1d2330}
header{background:#1d2330;color:#fff;padding:12px 20px;display:flex;gap:16px;align-items:center}
header a,header button{color:#fff;margin-left:auto;background:none;border:1px solid #fff6;border-radius:4px;
padding:4px 10px;text-decoration:none;font-size:14px;cursor:pointer}
main{padding:20px;max-width:1200px;margin:auto}
table{border-collapse:collapse;width:100%;background:#fff;font-size:14px}
th,td{text-align:left;padding:6px 8px;border-bottom:1px solid #e3e6eb;vertical-align:top}
.ok{color:#18794e;font-weight:600}.off{color:#b42318;font-weight:600}
.box{background:#fff;padding:16px;margin:16px 0;border:1px solid #e3e6eb;border-radius:6px}
.msg{padding:10px;border-radius:4px;margin:12px 0}.err{background:#fde8e8}.good{background:#e6f4ea}
code{font-size:15px;word-break:break-all}input,select,button{font-size:14px;margin:4px 0}
"""


def esc(value):
    return html.escape("" if value is None else str(value), quote=True)


def ago(seconds):
    seconds = max(0, int(seconds))
    if seconds < 90:
        return "%d s ago" % seconds
    if seconds < 5400:
        return "%d min ago" % (seconds // 60)
    if seconds < 172800:
        return "%d h ago" % (seconds // 3600)
    return "%d days ago" % (seconds // 86400)


class Site:
    """State shared by the request handlers: store, sessions, login throttle."""

    def __init__(self, store, admin_password, clock=time.time):
        self.store = store
        self.admin_password = admin_password
        self.clock = clock
        self.sessions = {}  # sid -> (csrf, expires)
        self.failures = {}  # client ip -> [failure times]

    def new_session(self):
        sid, csrf = secrets.token_hex(32), secrets.token_hex(32)
        self.sessions[sid] = (csrf, self.clock() + SESSION_S)
        return sid

    def session(self, sid):
        """The CSRF token of a live session, else None."""
        s = self.sessions.get(sid or "")
        if s is None or s[1] < self.clock():
            self.sessions.pop(sid or "", None)
            return None
        return s[0]

    def locked(self, ip):
        now = self.clock()
        recent = [t for t in self.failures.get(ip, []) if now - t < LOGIN_WINDOW_S]
        self.failures[ip] = recent
        return len(recent) >= LOGIN_FAILURES_MAX

    def password_ok(self, ip, password):
        ok = bool(self.admin_password) and hmac.compare_digest(password.encode(), self.admin_password.encode())
        if not ok:
            self.failures.setdefault(ip, []).append(self.clock())
        return ok


class Handler(BaseHTTPRequestHandler):
    server_version = "rfupdate"
    protocol_version = "HTTP/1.1"

    @property
    def site(self) -> Site:
        return self.server.site

    def log_message(self, fmt, *args):  # quiet unless asked for (--verbose)
        if getattr(self.server, "verbose", False):
            super().log_message(fmt, *args)

    # --- helpers ---

    def send(self, status, body, ctype="text/html; charset=utf-8", headers=None):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if ctype.startswith("text/html"):
            self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; "
                                                        "form-action 'self'; frame-ancestors 'none'")
            self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def redirect(self, location, headers=None):
        h = {"Location": location}
        h.update(headers or {})
        self.send(303, "", headers=h)

    def cookie_sid(self):
        for part in (self.headers.get("Cookie") or "").split(";"):
            name, _, value = part.strip().partition("=")
            if name == "sid":
                return value
        return None

    def read_body(self, limit):
        """The request body, or None (and an error sent) when it is too large."""
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length < 0 or length > limit:
            self.send(413, "Too large")
            self.close_connection = True
            return None
        return self.rfile.read(length)

    def page(self, title, content, status=200, logged_in=False, csrf=None):
        nav = ""
        if logged_in:
            nav = ('<a href="/">Devices</a><form method="post" action="/logout" style="margin:0">'
                   '<input type="hidden" name="csrf" value="%s"><button>Log out</button></form>' % esc(csrf))
        else:
            nav = '<a href="/admin">Admin</a>'
        body = ('<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" '
                'content="width=device-width,initial-scale=1"><title>%s</title><style>%s</style></head><body>'
                '<header><strong>RF-Power-Monitor updates</strong>%s</header><main>%s</main></body></html>'
                % (esc(title), STYLE, nav, content))
        self.send(status, body)

    # --- routes ---

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            return self.devices_page()
        if path == "/login":
            return self.login_page()
        if path == "/admin":
            return self.admin_page()
        if path.startswith("/releases/") and path.endswith(".upd"):
            return self.release_file(unquote(path[len("/releases/"):-4]))
        if path in ("/api/checkin", "/logout") or path.startswith("/admin/"):
            return self.send(405, "Use POST", "text/plain")
        self.send(404, "Not found", "text/plain")

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/checkin":
            return self.checkin()
        if path == "/login":
            return self.login()
        if path == "/admin/upload":
            return self.upload()
        if path in ("/logout", "/admin/assign", "/admin/release/delete", "/admin/key/new"):
            return self.admin_form(path)
        self.send(404, "Not found", "text/plain")

    def checkin(self):
        body = self.read_body(protocol.BODY_MAX * 4)
        if body is None:
            return
        if len(body) > protocol.BODY_MAX:
            return self.send(*api.BAD, ctype="text/plain")
        status, reply = api.handle_checkin(self.site.store, body, self.headers.get("X-RFPower-Auth"),
                                           self.site.clock())
        self.send(status, reply, "text/plain")

    def devices_page(self):
        now = self.site.clock()
        rows = []
        for d in self.site.store.device_rows(now):
            state = '<span class="ok">online</span>' if d["online"] else '<span class="off">offline</span>'
            fw = "%s (slot %s%s)" % (esc(d["version"]), esc(d["slot"]), ", on trial" if d["trial"] else "")
            upd_text = ""
            if d["update_status"]:
                upd_text = "%s: %s" % (esc(d["update_version"]), esc(d["update_status"]))
                if d["update_detail"]:
                    upd_text += " (%s)" % esc(d["update_detail"])
            rows.append("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s<br>%s</td><td>%s</td></tr>"
                        % (esc(d["hostname"]), esc(d["serial"]), esc(d["ipv4"]),
                           "<br>".join(esc(a) for a in d["ipv6"]), fw, state, ago(now - d["last_seen"]), upd_text))
        table = ("<table><tr><th>Hostname</th><th>Serial</th><th>IPv4</th><th>IPv6</th><th>Firmware</th>"
                 "<th>Last check-in</th><th>Update</th></tr>%s</table>" % "".join(rows)) if rows else \
            "<p>No device has checked in yet.</p>"
        self.page("Devices", "<h2>Devices</h2>" + table +
                  '<p style="font-size:13px;color:#667">Devices check in every 30 minutes; offline after 65 '
                  'minutes without one.</p>')

    def login_page(self, message="", status=200):
        if not self.site.admin_password:
            content = ('<div class="msg err">Logins are off: set RFUPDATE_ADMIN_PASSWORD for the container '
                       '(compose.override.yml) and restart it.</div>')
        else:
            content = ('<div class="box"><form method="post" action="/login"><label>Admin password<br>'
                       '<input type="password" name="password" autofocus></label><br><button>Log in</button>'
                       '</form></div>')
        if message:
            content = '<div class="msg err">%s</div>' % esc(message) + content
        self.page("Log in", "<h2>Log in</h2>" + content, status)

    def login(self):
        body = self.read_body(FORM_MAX)
        if body is None:
            return
        ip = self.client_address[0]
        if not self.site.admin_password:
            return self.login_page(status=403)
        if self.site.locked(ip):
            return self.login_page("Too many wrong passwords. Wait a minute.", 429)
        password = parse_qs(body.decode("utf-8", "replace")).get("password", [""])[0]
        if not self.site.password_ok(ip, password):
            return self.login_page("Wrong password.", 401)
        sid = self.site.new_session()
        self.redirect("/admin", {"Set-Cookie": "sid=%s; Path=/; HttpOnly; SameSite=Strict" % sid})

    def admin_page(self, message="", error=False, status=200, extra=""):
        sid = self.cookie_sid()
        csrf = self.site.session(sid)
        if csrf is None:
            return self.redirect("/login")
        store = self.site.store
        now = self.site.clock()
        releases = store.releases()
        rel_rows = "".join(
            '<tr><td>%s</td><td>%d bytes</td><td>%s</td><td><form method="post" action="/admin/release/delete" '
            'style="margin:0"><input type="hidden" name="csrf" value="%s"><input type="hidden" name="version" '
            'value="%s"><button>Delete</button></form></td></tr>'
            % (esc(r["version"]), r["size"], ago(now - r["uploaded"]), esc(csrf), esc(r["version"]))
            for r in releases)
        options = "".join('<option value="%s">%s</option>' % (esc(r["version"]), esc(r["version"])) for r in releases)
        dev_rows = "".join(
            '<tr><td><input type="checkbox" name="serial" value="%s"></td><td>%s</td><td>%s</td><td>%s</td>'
            '<td>%s</td><td>%s</td></tr>'
            % (esc(d["serial"]), esc(d["hostname"]), esc(d["serial"]), esc(d["version"]),
               "online" if d["online"] else "offline",
               esc("%s: %s" % (d["update_version"], d["update_status"])) if d["update_status"] else "")
            for d in store.device_rows(now))
        msg = ('<div class="msg %s">%s</div>' % ("err" if error else "good", esc(message))) if message else ""
        key_state = "set" if store.site_key() else "not set"
        content = (
            msg + extra +
            '<h2>Releases</h2><div class="box"><form method="post" action="/admin/upload" '
            'enctype="multipart/form-data"><input type="hidden" name="csrf" value="%s">'
            '<input type="file" name="file" accept=".upd"> <button>Upload</button></form>'
            '<p style="font-size:13px">A signed .upd from tools/fw_release.py, version %s or newer.</p>'
            '<table><tr><th>Version</th><th>Size</th><th>Uploaded</th><th></th></tr>%s</table></div>'
            '<h2>Update devices</h2><div class="box"><form method="post" action="/admin/assign">'
            '<input type="hidden" name="csrf" value="%s"><table><tr><th></th><th>Hostname</th><th>Serial</th>'
            '<th>Firmware</th><th>State</th><th>Update</th></tr>%s</table><p>Release <select name="version">%s'
            '</select> <button>Update ticked devices</button></p><p style="font-size:13px">Ticked devices '
            'download it at their next check-in (within 30 minutes).</p></form></div>'
            '<h2>Shared key</h2><div class="box"><p>Shared key: %s</p><form method="post" action="/admin/key/new">'
            '<input type="hidden" name="csrf" value="%s"><button>Generate new key</button></form>'
            '<p style="font-size:13px">Every device needs the key (serial menu option 10 or its Firmware page). '
            'A new key stops check-ins from devices that still have the old one.</p></div>'
            % (esc(csrf), upd.MIN_VERSION, rel_rows, esc(csrf), dev_rows, options, key_state, esc(csrf)))
        self.page("Admin", content, status, logged_in=True, csrf=csrf)

    def form_session(self, fields):
        """The CSRF token when the session cookie and the form's csrf field match, else None (403 sent)."""
        csrf = self.site.session(self.cookie_sid())
        sent = (fields.get("csrf") or [""])[0]
        if csrf is None or not hmac.compare_digest(sent.encode(), csrf.encode()):
            self.send(403, "Not logged in, or the form is out of date. Reload the page.", "text/plain")
            return None
        return csrf

    def admin_form(self, path):
        body = self.read_body(FORM_MAX)
        if body is None:
            return
        fields = parse_qs(body.decode("utf-8", "replace"))
        if self.form_session(fields) is None:
            return
        store, now = self.site.store, self.site.clock()
        if path == "/logout":
            self.site.sessions.pop(self.cookie_sid(), None)
            return self.redirect("/", {"Set-Cookie": "sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict"})
        if path == "/admin/key/new":
            key = store.new_site_key()
            extra = ('<div class="box"><p>New shared key (shown only now; enter it on every device):</p>'
                     '<p><code>%s</code></p></div>' % key.hex())
            return self.admin_page("A new key was generated.", extra=extra)
        version = (fields.get("version") or [""])[0]
        try:
            if path == "/admin/assign":
                serials = fields.get("serial") or []
                if not serials:
                    raise ValueError("Tick at least one device.")
                store.assign(serials, version, now)
                return self.admin_page("%d device(s) get %s at their next check-in." % (len(serials), version))
            store.delete_release(version)
            return self.admin_page("Release %s deleted." % version)
        except ValueError as e:
            return self.admin_page(str(e), error=True, status=400)

    def upload(self):
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = -1
        if length < 0 or length > UPLOAD_MAX:
            self.close_connection = True
            return self.send(413, "The file is larger than a release can be.", "text/plain")
        if self.site.session(self.cookie_sid()) is None:
            self.close_connection = True
            return self.send(403, "Not logged in.", "text/plain")
        body = self.rfile.read(length)
        ctype = self.headers.get("Content-Type") or ""
        msg = BytesParser(policy=HTTP).parsebytes(b"Content-Type: " + ctype.encode("latin-1") + b"\r\n\r\n" + body)
        fields, data = {}, None
        if msg.is_multipart():
            for part in msg.iter_parts():
                name = part.get_param("name", header="content-disposition")
                if name == "file":
                    data = part.get_payload(decode=True)
                elif name:
                    fields[name] = [part.get_content().strip()]
        if self.form_session(fields) is None:
            return
        if not data:
            return self.admin_page("Choose a .upd file.", error=True, status=400)
        try:
            version = self.site.store.add_release(data, self.site.clock())
        except ValueError as e:
            return self.admin_page(str(e), error=True, status=400)
        self.admin_page("Release %s uploaded." % version)

    def release_file(self, version):
        path = self.site.store.release_path(version)
        if path is None:
            return self.send(404, "Not found", "text/plain")
        with open(path, "rb") as f:
            data = f.read()
        self.send(200, data, "application/octet-stream")


class SiteServer(ThreadingHTTPServer):
    """The port is ours alone: on Windows SO_REUSEADDR (http.server's default) would let a
    second process, e.g. an old copy with another key, bind it too and answer check-ins."""

    allow_reuse_address = os.name != "nt"

    def server_bind(self):
        if os.name == "nt":
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


def make_server(store, host, port, admin_password, clock=time.time, verbose=False):
    """A threading HTTP server for the site (call serve_forever())."""
    server = SiteServer((host, port), Handler)
    server.daemon_threads = True
    server.site = Site(store, admin_password, clock)
    server.verbose = verbose
    return server
