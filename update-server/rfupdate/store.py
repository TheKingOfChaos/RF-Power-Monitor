"""The site's data: devices, releases, assignments, seen nonces and the site key.

SQLite in <data>/rfupdate.db, release files in <data>/releases/<version>.upd.
One lock around every call: the web server runs requests in threads.
"""

import json
import os
import re
import secrets
import sqlite3
import threading

from . import upd

OFFLINE_AFTER_S = 3900         # two missed 30-minute check-ins (65 min)
LOST_AFTER_S = 7200            # no report this long after the device was told
NONCE_KEEP_S = 400 * 86400    # a nonce seen in the last 400 days is a replay (~19,000 rows per device)
DOWNLOAD_FAILURES_MAX = 3      # then the assignment ends as failed

ACTIVE = ("waiting", "told", "on trial")  # a device does not check in while it downloads
_ROLLBACK = re.compile(r"(rolledback-crash|rolledback-unreachable|fallback-crash) (\S+)")

_SCHEMA = """
CREATE TABLE IF NOT EXISTS devices (
    serial TEXT PRIMARY KEY, hostname TEXT, ipv4 TEXT, ipv6 TEXT, mac TEXT, version TEXT, slot TEXT,
    trial INTEGER, update_state TEXT, fail TEXT, last TEXT, first_seen REAL, last_seen REAL);
CREATE TABLE IF NOT EXISTS releases (version TEXT PRIMARY KEY, size INTEGER, uploaded REAL);
CREATE TABLE IF NOT EXISTS assignments (
    serial TEXT PRIMARY KEY, version TEXT, status TEXT, detail TEXT, assigned REAL, told REAL,
    last_at_told TEXT, download_failures INTEGER, ended REAL);
CREATE TABLE IF NOT EXISTS nonces (serial TEXT, nonce TEXT, seen REAL, PRIMARY KEY (serial, nonce));
CREATE TABLE IF NOT EXISTS settings (name TEXT PRIMARY KEY, value TEXT);
"""


class Store:
    def __init__(self, data_dir: str):
        self.data_dir = data_dir
        self.release_dir = os.path.join(data_dir, "releases")
        os.makedirs(self.release_dir, exist_ok=True)
        self._lock = threading.RLock()
        self._db = sqlite3.connect(os.path.join(data_dir, "rfupdate.db"), check_same_thread=False)
        self._db.row_factory = sqlite3.Row
        self._db.executescript(_SCHEMA)
        self._db.commit()

    def close(self):
        with self._lock:
            self._db.close()

    # --- site key ---

    def site_key(self):
        """The shared key (32 bytes), or None before one was generated."""
        with self._lock:
            row = self._db.execute("SELECT value FROM settings WHERE name='key'").fetchone()
            return bytes.fromhex(row["value"]) if row else None

    def set_site_key(self, key):
        """Set (bytes) or remove (None) the shared key."""
        with self._lock:
            if key is None:
                self._db.execute("DELETE FROM settings WHERE name='key'")
            else:
                self._db.execute("INSERT OR REPLACE INTO settings VALUES ('key', ?)", (key.hex(),))
            self._db.commit()

    def new_site_key(self) -> bytes:
        """Generate and store a new random key; every device needs it entered again."""
        key = secrets.token_bytes(32)
        self.set_site_key(key)
        return key

    # --- releases ---

    def add_release(self, data: bytes, now: float) -> str:
        """Store an uploaded .upd; returns its version. ValueError: not a .upd, too large,
        older than upd.MIN_VERSION, or a version the site already holds."""
        if len(data) > upd.MAX_UPLOAD:
            raise ValueError("The file is larger than a release can be.")
        version = upd.parse_header(data)
        if upd.compare_versions(version, upd.MIN_VERSION) < 0:
            raise ValueError("%s is older than %s, the first firmware that checks in." % (version, upd.MIN_VERSION))
        with self._lock:
            if self._db.execute("SELECT 1 FROM releases WHERE version=?", (version,)).fetchone():
                raise ValueError("Release %s is already here. Delete it first to replace it." % version)
            path = os.path.join(self.release_dir, version + ".upd")
            tmp = path + ".tmp"
            with open(tmp, "wb") as f:
                f.write(data)
            os.replace(tmp, path)
            self._db.execute("INSERT INTO releases VALUES (?, ?, ?)", (version, len(data), now))
            self._db.commit()
        return version

    def releases(self):
        with self._lock:
            return [dict(r) for r in self._db.execute("SELECT * FROM releases ORDER BY uploaded DESC")]

    def release_path(self, version: str):
        """The file of a release the site holds, else None (also for anything that is not a version)."""
        if not upd.is_safe_version(version):
            return None
        with self._lock:
            if not self._db.execute("SELECT 1 FROM releases WHERE version=?", (version,)).fetchone():
                return None
        return os.path.join(self.release_dir, version + ".upd")

    def delete_release(self, version: str):
        """ValueError while a device is still assigned to it."""
        with self._lock:
            placeholders = ",".join("?" * len(ACTIVE))
            busy = self._db.execute("SELECT 1 FROM assignments WHERE version=? AND status IN (%s)" % placeholders,
                                    (version,) + ACTIVE).fetchone()
            if busy:
                raise ValueError("Release %s is still assigned to a device." % version)
            path = self.release_path(version)
            self._db.execute("DELETE FROM releases WHERE version=?", (version,))
            self._db.commit()
            if path and os.path.exists(path):
                os.remove(path)

    # --- devices and assignments ---

    def nonce_seen(self, serial: str, nonce: str, now: float) -> bool:
        """Record a check-in's nonce; True if this device already used it within 24 h."""
        with self._lock:
            self._db.execute("DELETE FROM nonces WHERE seen < ?", (now - NONCE_KEEP_S,))
            if self._db.execute("SELECT 1 FROM nonces WHERE serial=? AND nonce=?", (serial, nonce)).fetchone():
                self._db.commit()
                return True
            self._db.execute("INSERT INTO nonces VALUES (?, ?, ?)", (serial, nonce, now))
            self._db.commit()
            return False

    def upsert_checkin(self, f: dict, now: float):
        """Store what a check-in reported (one row per serial number)."""
        with self._lock:
            self._db.execute(
                "INSERT INTO devices VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(serial) DO UPDATE SET "
                "hostname=excluded.hostname, ipv4=excluded.ipv4, ipv6=excluded.ipv6, mac=excluded.mac, "
                "version=excluded.version, slot=excluded.slot, trial=excluded.trial, "
                "update_state=excluded.update_state, fail=excluded.fail, last=excluded.last, "
                "last_seen=excluded.last_seen",
                (f["serial"], f["hostname"], f["ipv4"], json.dumps(f["ipv6"]), f["mac"], f["version"], f["slot"],
                 int(f["trial"]), f["update"], f["fail"], f["last"], now, now))
            self._db.commit()

    def assign(self, serials, version: str, now: float) -> int:
        """Give these devices this release at their next check-in (replaces earlier assignments).
        A device that already runs it, kept, is done at once. Returns how many known devices
        were assigned (unknown serial numbers are skipped)."""
        with self._lock:
            if not self.release_path(version):
                raise ValueError("There is no release %s." % version)
            count = 0
            for serial in serials:
                dev = self._db.execute("SELECT * FROM devices WHERE serial=?", (serial,)).fetchone()
                if dev is None:
                    continue
                count += 1
                done = dev["version"] == version and not dev["trial"]
                self._db.execute(
                    "INSERT OR REPLACE INTO assignments VALUES (?,?,?,?,?,?,?,?,?)",
                    (serial, version, "done" if done else "waiting", "already running it" if done else "",
                     now, None, None, 0, now if done else None))
            self._db.commit()
            return count

    def _end(self, serial, status, detail, now):
        self._db.execute("UPDATE assignments SET status=?, detail=?, ended=? WHERE serial=?",
                         (status, detail, now, serial))

    def _expire(self, now):
        self._db.execute("UPDATE assignments SET status='lost', detail='no report within 2 hours', ended=? "
                         "WHERE status = 'told' AND told < ?", (now, now - LOST_AFTER_S))

    def advance(self, f: dict, now: float):
        """Move this device's assignment on from what its check-in reported; returns the
        release version to send it, or None."""
        with self._lock:
            self._expire(now)
            a = self._db.execute("SELECT * FROM assignments WHERE serial=?", (f["serial"],)).fetchone()
            if a is None or a["status"] not in ACTIVE:
                self._db.commit()
                return None
            serial, v = f["serial"], a["version"]
            fail = f["fail"].split(" ", 2) if f["fail"] else []
            rollback = _ROLLBACK.fullmatch(f["last"])
            send = None
            if f["version"] == v:
                if f["trial"]:
                    self._db.execute("UPDATE assignments SET status='on trial' WHERE serial=?", (serial,))
                else:
                    self._end(serial, "done", "", now)
            elif (a["told"] is not None and rollback and rollback.group(2) == v
                  and f["last"] != a["last_at_told"]):
                self._end(serial, "rolled back", rollback.group(1).replace("-", " "), now)
            elif len(fail) == 3 and fail[0] == v and fail[1] == "refused":
                self._end(serial, "refused", fail[2], now)
            elif len(fail) == 3 and fail[0] == v and fail[1] == "download":
                failures = a["download_failures"] + 1
                if failures >= DOWNLOAD_FAILURES_MAX:
                    self._db.execute("UPDATE assignments SET download_failures=? WHERE serial=?", (failures, serial))
                    self._end(serial, "failed", fail[2], now)
                else:
                    self._db.execute("UPDATE assignments SET download_failures=?, detail=? WHERE serial=?",
                                     (failures, fail[2], serial))
                    send = v
            else:
                send = v
            if send is not None:
                if a["told"] is None:
                    self._db.execute("UPDATE assignments SET status='told', told=?, last_at_told=? WHERE serial=?",
                                     (now, f["last"], serial))
            self._db.commit()
            return send

    def device_rows(self, now: float):
        """Every device, newest check-in first, with "online" and its assignment's status."""
        with self._lock:
            self._expire(now)
            self._db.commit()
            rows = self._db.execute(
                "SELECT d.*, a.version AS update_version, a.status AS update_status, a.detail AS update_detail "
                "FROM devices d LEFT JOIN assignments a ON a.serial = d.serial ORDER BY d.last_seen DESC").fetchall()
        out = []
        for r in rows:
            d = dict(r)
            d["ipv6"] = json.loads(d["ipv6"] or "[]")
            d["trial"] = bool(d["trial"])
            d["online"] = now - d["last_seen"] <= OFFLINE_AFTER_S
            out.append(d)
        return out
