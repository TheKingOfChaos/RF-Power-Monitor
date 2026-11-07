"""Generate the project's Excalidraw diagrams (docs/diagrams/*.excalidraw).

The .excalidraw files can be opened and edited at https://excalidraw.com or
with the VS Code Excalidraw extension. Running this script again overwrites
them, so edit either here or in Excalidraw, not both. render_svg.sh exports
the SVGs used in the README and docs.

    python docs/diagrams/make_diagrams.py

Layout rules (checked by Diagram.check(), which prints a warning for each
break): arrows run straight or in orthogonal steps through the gaps between
boxes, never along a box edge or zone border and never through a box; every
segment is long enough to see; an arrow's label is free text set beside its
segment (above a horizontal one, right of a vertical one, unless told
otherwise), so no line or arrowhead crosses it.
"""

import hashlib
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Excalidraw palette
BLUE, GREEN, YELLOW, RED, VIOLET = "#a5d8ff", "#b2f2bb", "#ffec99", "#ffc9c9", "#d0bfff"
ORANGE, CYAN, GRAY, WHITE = "#ffd8a8", "#99e9f2", "#e9ecef", "#ffffff"
INK, MUTED, NOTE = "#1e1e1e", "#868e96", "#495057"
ZONE_BLUE, ZONE_GREEN, ZONE_YELLOW, ZONE_RED = "#e7f5ff", "#ebfbee", "#fff9db", "#fff5f5"
ZONE_ORANGE, ZONE_GRAY = "#fff4e6", "#f8f9fa"

CHAR_W = 0.58      # average Virgil glyph width per font size unit (for layout)
LINE_H = 1.25      # line height per font size unit
MIN_SEGMENT = 30   # shortest visible arrow segment
CLEARANCE = 12     # an arrow segment parallel to an edge or border keeps at least this distance


def _num(key):
    return int(hashlib.sha1(key.encode()).hexdigest()[:8], 16)


def _text_size(text, size):
    lines = text.split("\n")
    return max(len(l) for l in lines) * size * CHAR_W, len(lines) * size * LINE_H


def _seg_hits_rect(p, q, rect, pad=0.0):
    """True if segment p-q passes through rect (x0, y0, x1, y1) grown by pad."""
    x0, y0, x1, y1 = rect[0] - pad, rect[1] - pad, rect[2] + pad, rect[3] + pad
    steps = max(1, int(math.dist(p, q) / 2))
    for i in range(steps + 1):
        t = i / steps
        x, y = p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t
        if x0 < x < x1 and y0 < y < y1:
            return True
    return False


def _rects_overlap(a, b, pad=0.0):
    return a[0] - pad < b[2] and b[0] - pad < a[2] and a[1] - pad < b[3] and b[1] - pad < a[3]


class Diagram:
    def __init__(self, name, title):
        self.name = name
        self.elements = []
        self.by_id = {}
        self.shapes = {}   # box id -> rect
        self.zones = {}    # zone id -> rect
        self.texts = []    # (rect, text) of free text: titles, notes
        self.labels = []   # (rect, text, arrow id)
        self.arrows = []   # (arrow id, points, start box, end box)
        self.text(40, 20, title, size=32)

    # ---- element basics ----

    def _base(self, eid, etype, x, y, w, h, **kw):
        e = {
            "id": eid, "type": etype, "x": x, "y": y, "width": w, "height": h, "angle": 0,
            "strokeColor": kw.get("stroke", INK), "backgroundColor": kw.get("bg", "transparent"),
            "fillStyle": "solid", "strokeWidth": kw.get("stroke_width", 2),
            "strokeStyle": "dashed" if kw.get("dashed") else "solid", "roughness": 1, "opacity": 100,
            "groupIds": kw.get("groups", []), "frameId": None, "roundness": kw.get("roundness"),
            "seed": _num(self.name + eid), "version": 1, "versionNonce": _num(eid + self.name),
            "isDeleted": False, "boundElements": [], "updated": 1, "link": None, "locked": False,
        }
        self.elements.append(e)
        self.by_id[eid] = e
        return e

    def _text_el(self, eid, x, y, text, size, align, valign, container=None, color=INK, groups=()):
        w, h = _text_size(text, size)
        if container is not None:
            cx, cy = container
            x, y = cx - w / 2, cy - h / 2
        e = self._base(eid, "text", x, y, w, h, stroke=color, groups=list(groups))
        e.update({"text": text, "originalText": text, "fontSize": size, "fontFamily": 1,
                  "textAlign": align, "verticalAlign": valign, "containerId": None,
                  "lineHeight": LINE_H, "autoResize": True, "boundElements": None})
        return e

    def text(self, x, y, text, size=20, color=INK):
        e = self._text_el("t%d" % len(self.elements), x, y, text, size, "left", "top", color=color)
        self.texts.append(((x, y, x + e["width"], y + e["height"]), text))
        return e

    def note(self, x, y, text):
        return self.text(x, y, text, size=16, color=NOTE)

    # ---- shapes ----

    def box(self, eid, x, y, w, h, text, bg=BLUE, size=20, shape="rectangle", dashed=False):
        e = self._base(eid, shape, x, y, w, h, bg=bg, dashed=dashed,
                       roundness={"type": 3} if shape == "rectangle" else {"type": 2})
        self.shapes[eid] = (x, y, x + w, y + h)
        if text:
            t = self._text_el(eid + "-text", 0, 0, text, size, "center", "middle",
                              container=(x + w / 2, y + h / 2))
            t["containerId"] = eid
            e["boundElements"].append({"type": "text", "id": t["id"]})
            tw, th = _text_size(text, size)
            room = (w * 0.62, h * 0.62) if shape == "diamond" else (w - 12, h - 6)
            if tw > room[0] or th > room[1]:
                print("  warning %s: text of %s does not fit (%dx%d in %dx%d)" % (self.name, eid, tw, th, w, h))
        return e

    def zone(self, eid, x, y, w, h, title, bg=ZONE_GRAY, title_at="top", title_x=16):
        """Dashed group box with its title inside at the top (or bottom) left."""
        self._base(eid, "rectangle", x, y, w, h, bg=bg, stroke=MUTED, dashed=True, roundness={"type": 3})
        self.zones[eid] = (x, y, x + w, y + h)
        _, th = _text_size(title, 20)
        ty = y + 10 if title_at == "top" else y + h - th - 10
        self.text(x + title_x, ty, title, size=20, color=NOTE)

    def swatch(self, x, y, bg, text):
        """Small colour key: a square and its meaning."""
        eid = "key-%d" % len(self.elements)
        self._base(eid, "rectangle", x, y, 22, 22, bg=bg, roundness={"type": 3})
        return self.text(x + 30, y, text, size=16, color=NOTE)

    # ---- arrows ----

    def _anchor(self, eid, side, frac=0.5, at=None):
        """Point on a side of a box: at frac of the side, or at the absolute coordinate `at`."""
        x0, y0, x1, y1 = self.shapes[eid]
        w, h = x1 - x0, y1 - y0
        if side in ("top", "bottom"):
            px = at if at is not None else x0 + w * frac
            return (px, y0 if side == "top" else y1)
        py = at if at is not None else y0 + h * frac
        return (x0 if side == "left" else x1, py)

    def arrow(self, a, a_side, b, b_side, label=None, via=(), a_frac=0.5, b_frac=0.5, a_at=None, b_at=None,
              both=False, dashed=False, size=16, color=INK, lseg=None, lpos=0.5, lside=None, loff=10):
        """Arrow from box a to box b (a uses / sends to b).

        via: corner points of an orthogonal route. The label goes beside segment lseg (default: the
        longest) at lpos along it, on side lside ("above"/"below" for a horizontal segment,
        "left"/"right" for a vertical one; default above / right), loff away from the line.
        """
        eid = "a-%s-%s-%d" % (a, b, len(self.elements))
        start = self._anchor(a, a_side, a_frac, a_at)
        end = self._anchor(b, b_side, b_frac, b_at)
        pts = [start] + list(via) + [end]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        groups = [eid + "-g"] if label else []
        e = self._base(eid, "arrow", start[0], start[1], max(xs) - min(xs), max(ys) - min(ys),
                       stroke=color, dashed=dashed, roundness=None, groups=groups)
        e.update({"points": [[px - start[0], py - start[1]] for px, py in pts],
                  "lastCommittedPoint": None,
                  "startBinding": {"elementId": a, "focus": 0, "gap": 1},
                  "endBinding": {"elementId": b, "focus": 0, "gap": 1},
                  "startArrowhead": "arrow" if both else None, "endArrowhead": "arrow", "elbowed": False})
        for target in (a, b):
            self.by_id[target]["boundElements"].append({"type": "arrow", "id": eid})
        self.arrows.append((eid, pts, a, b))
        if label:
            self._label(eid, pts, label, size, color, groups, lseg, lpos, lside, loff)
        return e

    def _label(self, eid, pts, label, size, color, groups, seg, pos, side, off):
        segs = list(zip(pts, pts[1:]))
        if seg is None:
            seg = max(range(len(segs)), key=lambda i: math.dist(*segs[i]))
        (x1, y1), (x2, y2) = segs[seg]
        px, py = x1 + (x2 - x1) * pos, y1 + (y2 - y1) * pos
        w, h = _text_size(label, size)
        horizontal = abs(x2 - x1) >= abs(y2 - y1)
        side = side or ("above" if horizontal else "right")
        if side == "above":
            x, y, align = px - w / 2, py - off - h, "center"
        elif side == "below":
            x, y, align = px - w / 2, py + off, "center"
        elif side == "right":  # a little further: glyphs of left-aligned text start slightly left of x
            x, y, align = px + off + 4, py - h / 2, "left"
        else:
            x, y, align = px - off - 4 - w, py - h / 2, "right"
        self._text_el(eid + "-label", x, y, label, size, align, "top", color=color, groups=groups)
        self.labels.append(((x, y, x + w, y + h), label, eid))

    def line_arrow(self, start, end, color=MUTED):
        """An arrow not attached to any shape."""
        eid = "free-%d" % len(self.elements)
        e = self._base(eid, "arrow", start[0], start[1], abs(end[0] - start[0]), abs(end[1] - start[1]),
                       stroke=color, roundness=None)
        e.update({"points": [[0, 0], [end[0] - start[0], end[1] - start[1]]], "lastCommittedPoint": None,
                  "startBinding": None, "endBinding": None, "startArrowhead": None,
                  "endArrowhead": "arrow", "elbowed": False})
        return e

    # ---- layout check ----

    def check(self):
        """Print a warning for each break of the layout rules (see the module docstring)."""
        warn = lambda msg: print("  warning %s: %s" % (self.name, msg))
        for eid, pts, a, b in self.arrows:
            segs = list(zip(pts, pts[1:]))
            for i, (p, q) in enumerate(segs):
                length = math.dist(p, q)
                if length < MIN_SEGMENT:
                    warn("%s segment %d is only %d px" % (eid, i, length))
                if p[0] != q[0] and p[1] != q[1]:
                    warn("%s segment %d is diagonal" % (eid, i))
                for sid, r in self.shapes.items():
                    if sid in (a, b) and (i == 0 and sid == a or i == len(segs) - 1 and sid == b):
                        continue
                    if _seg_hits_rect(p, q, r, pad=-1):
                        warn("%s segment %d runs through %s" % (eid, i, sid))
                for rid, r in list(self.shapes.items()) + list(self.zones.items()):
                    self._check_parallel(warn, eid, i, p, q, rid, r)
            for rect, text, _ in self.labels:
                if any(_seg_hits_rect(p, q, rect, pad=3) for p, q in segs):
                    warn("label %r is crossed by %s" % (text, eid))
        others = [(r, t) for r, t in self.texts] + [(r, t) for r, t, _ in self.labels]
        for rect, text, _ in self.labels:
            for sid, r in self.shapes.items():
                if _rects_overlap(rect, r, pad=2):
                    warn("label %r overlaps box %s" % (text, sid))
            for zid, (x0, y0, x1, y1) in self.zones.items():
                for edge in ((x0, y0, x1, y0), (x0, y1, x1, y1), (x0, y0, x0, y1), (x1, y0, x1, y1)):
                    if _seg_hits_rect(edge[:2], edge[2:], rect, pad=3):
                        warn("label %r touches the border of %s" % (text, zid))
            for r, t in others:
                if r is not rect and _rects_overlap(rect, r, pad=2):
                    warn("label %r overlaps text %r" % (text, t))
        for r, t in self.texts:
            for eid, pts, _, _ in self.arrows:
                if any(_seg_hits_rect(p, q, r, pad=3) for p, q in zip(pts, pts[1:])):
                    warn("text %r is crossed by %s" % (t, eid))
            for sid, s in self.shapes.items():
                if _rects_overlap(r, s):
                    warn("text %r overlaps box %s" % (t, sid))

    def _check_parallel(self, warn, eid, i, p, q, rid, r):
        x0, y0, x1, y1 = r
        if p[1] == q[1]:  # horizontal segment against horizontal edges
            lo, hi = sorted((p[0], q[0]))
            if min(hi, x1) - max(lo, x0) > 4:
                for edge_y in (y0, y1):
                    if 0 < abs(p[1] - edge_y) < CLEARANCE or (abs(p[1] - edge_y) == 0 and hi - lo > 0
                                                                and rid in self.zones):
                        warn("%s segment %d runs along an edge of %s" % (eid, i, rid))
        if p[0] == q[0]:
            lo, hi = sorted((p[1], q[1]))
            if min(hi, y1) - max(lo, y0) > 4:
                for edge_x in (x0, x1):
                    if 0 < abs(p[0] - edge_x) < CLEARANCE or (abs(p[0] - edge_x) == 0 and rid in self.zones):
                        warn("%s segment %d runs along an edge of %s" % (eid, i, rid))

    def save(self):
        self.check()
        doc = {"type": "excalidraw", "version": 2, "source": "https://excalidraw.com",
               "elements": self.elements,
               "appState": {"gridSize": None, "viewBackgroundColor": "#ffffff"}, "files": {}}
        path = os.path.join(HERE, self.name + ".excalidraw")
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, indent=1, ensure_ascii=False)
            f.write("\n")
        print("wrote", path)


def system_overview():
    d = Diagram("system-overview", "RF-Power-Monitor: system overview")
    d.zone("z-mains", 40, 80, 330, 430, "Mains side\n(galvanically isolated)", ZONE_RED)
    d.box("mains", 75, 150, 260, 60, "Mains 230 V AC", RED)
    d.box("plug", 75, 255, 260, 80, "IKEA TRETAKT smart plug\n(Dirigera hub, tests only)", GRAY, 16, dashed=True)
    d.box("det", 75, 380, 260, 100, "Mains detector\noptocoupler + filter\nclean 3.3 V DC level", ORANGE, 16)
    d.arrow("mains", "bottom", "plug", "top")
    d.arrow("plug", "bottom", "det", "top")
    d.box("pc", 40, 545, 330, 90, "PC: PuTTY or HIL tests\nUSB serial, 115200 8N1", GRAY, 16)

    d.zone("z-board", 520, 80, 680, 700, "RF-Power-Monitor board", ZONE_BLUE)
    d.box("flash", 560, 120, 340, 120, "Internal flash (LittleFS)\nsettings_a/b, update_a/b,\ntimelog_a/b.json",
          GREEN, 16)
    d.box("pico", 560, 300, 340, 320,
          "Raspberry Pi Pico (RP2040)\n\nCore 0: network, SNMP, web page,\nLLDP, NTP, syslog, menu,\n"
          "temperatures, load, watchdog\n8 s (fed while core 1 runs)\n\nCore 1: GPIO27 + GPIO22, 1 kHz",
          BLUE, 16)
    d.box("w5500", 990, 300, 170, 140, "W5500\nEthernet\nSPI0: GPIO16-19\nRST 20, INT 21", CYAN, 16)
    d.box("tmp", 990, 490, 170, 100, "TMP1075\nambient temp.\nI2C0, 0x48\nGPIO4/5", ORANGE, 16)
    d.box("button", 560, 670, 150, 80, "Reset button\nGPIO22 to GND", GRAY, 16)
    d.box("led", 750, 670, 150, 80, "Onboard LED\nstatus, reset", YELLOW, 16)
    d.arrow("pico", "top", "flash", "bottom", both=True)
    d.arrow("pico", "right", "w5500", "left", "SPI", a_at=370, b_at=370, both=True)
    d.arrow("pico", "right", "tmp", "left", "I2C", a_at=540, b_at=540, both=True)
    d.arrow("button", "top", "pico", "bottom", a_at=635, b_at=635)
    d.arrow("pico", "bottom", "led", "top", a_at=825, b_at=825)
    d.arrow("det", "right", "pico", "left", "GPIO27\nHIGH = mains", a_at=430, b_at=430)
    d.arrow("pc", "right", "pico", "left", "USB", a_at=590, b_at=590, both=True)

    # Network: the switch is a tall bar, every server hangs off it with a short straight arrow
    d.zone("z-net", 1330, 80, 640, 800, "Network", ZONE_GRAY)
    d.box("switch", 1370, 130, 120, 720, "LAN\nswitch\n\nIPv4\n+ IPv6\n\nLLDP", GRAY, 18)
    d.arrow("w5500", "right", "switch", "left", "100 Mbit/s\nLLDP", a_at=370, b_at=370, both=True)
    servers = [
        ("dhcp", 130, 110, "DHCP server: options 7 syslog,\n42 NTP, 224 trap destination,\n"
                          "225 update site; DHCPv6 31: NTP", GRAY, True),
        ("browser", 265, 70, "Browser\nweb page, TCP 80", VIOLET, True),
        ("lnms", 360, 80, "SNMP manager: LibreNMS,\nnet-snmp; UDP 161, v1/v2c", VIOLET, True),
        ("traps", 465, 70, "Trap receiver\nUDP 162 (v1 or v2c)", VIOLET, False),
        ("ntp", 560, 70, "NTP server\nUDP 123, every 15 min", VIOLET, True),
        ("syslog", 655, 70, "Syslog server, UDP 514\n(LibreNMS test: UDP 5140)", VIOLET, False),
        ("updsite", 750, 100, "Update site (Docker)\nTCP 8071: check-ins every\n30 min, chosen releases",
         VIOLET, True),
    ]
    for eid, y, h, text, bg, both in servers:
        d.box(eid, 1590, y, 350, h, text, bg, 16)
        d.arrow("switch", "right", eid, "left", a_at=y + h / 2, b_at=y + h / 2, both=both)
    d.save()


def firmware_architecture():
    d = Diagram("firmware-architecture", "RF-Power-Monitor: firmware architecture")
    d.note(40, 66, "An arrow means \"uses\". Each part of src/ sits above the libraries it runs on; "
                   "main.cpp starts all of them.")
    keys = [(BLUE, "src/: firmware on the Pico"), (GREEN, "lib/: pure C++, unit tested on the PC"),
            (YELLOW, "lib/: hardware layers"), (CYAN, "boot/: boot program"), (GRAY, "Arduino-Pico core")]
    for i, (bg, text) in enumerate(keys):
        d.swatch(40 + i * 370, 100, bg, text)

    SLOT, BW = 230, 200

    def sx(i):
        return 70 + SLOT * i

    def span(i, j=None):
        j = i if j is None else j
        return sx(i), SLOT * (j - i) + BW

    def place(eid, i, j, y, h, text, bg):
        x, w = span(i, j)
        d.box(eid, x, y, w, h, text, bg, 16)

    def down(a, b, slot):
        """Straight arrow down the middle of a slot"""
        cx = sx(slot) + BW / 2
        d.arrow(a, "bottom", b, "top", a_at=cx, b_at=cx)

    width = SLOT * 8 + 30
    # Band 1: network, SNMP, web, time
    d.zone("z-net", 40, 140, width, 450, "Network, SNMP, web page, time (core 0)", ZONE_BLUE)
    y1, y2, y3 = 190, 330, 470
    place("svc", 0, 1, y1, 100, "snmp_service.cpp\nUDP 161 requests, traps,\ndevice state", BLUE)
    place("web", 2, 2, y1, 100, "web_service.cpp\nweb page, TCP 80,\nnon-blocking", BLUE)
    place("net", 3, 4, y1, 100, "net_stats.cpp\nframe counters, W5500 PHY,\nlwIP hooks, DHCP options", BLUE)
    place("d6svc", 5, 5, y1, 100, "dhcp6_service.cpp\nstateful DHCPv6,\nframes around lwIP", BLUE)
    place("lldpsvc", 6, 6, y1, 100, "lldp_service.cpp\nLLDP send / listen,\nLLDP-MIB data", BLUE)
    place("time", 7, 7, y1, 100, "time_service.cpp\nlwIP SNTP, server\nfrom DHCP or typed", BLUE)
    place("agent", 0, 0, y2, 90, "snmp_agent\nMIB, Get/Next/Bulk,\ntraps", GREEN)
    place("acl", 1, 1, y2, 90, "access_control\nrate limiter,\naccess log", GREEN)
    place("webui", 2, 2, y2, 90, "web_ui\nHTTP parser, login,\nsessions, pages", GREEN)
    place("ifst", 3, 3, y2, 90, "if_stats\nframe classes,\nPHY decode", GREEN)
    place("dhcpopt", 4, 4, y2, 90, "dhcp_option\nask for and read\noptions 7, 224, 225", GREEN)
    place("d6c", 5, 5, y2, 90, "dhcp6_client\nDHCPv6 messages,\nclient state", GREEN)
    place("lldp", 6, 6, y2, 90, "lldp\nLLDPDU build/parse,\nneighbour, timing", GREEN)
    place("clock", 7, 7, y2, 90, "clock\nUTC calendar, CET/\nCEST, DateAndTime", GREEN)
    place("ber", 0, 0, y3, 80, "snmp_ber\nBER codec, OIDs,\nCounter64", GREEN)
    for a, b, slot in [("svc", "agent", 0), ("svc", "acl", 1), ("web", "webui", 2), ("net", "ifst", 3),
                       ("net", "dhcpopt", 4), ("d6svc", "d6c", 5), ("lldpsvc", "lldp", 6), ("time", "clock", 7),
                       ("agent", "ber", 0)]:
        down(a, b, slot)

    # Band 2: syslog, updates, menu, watchdog, temperatures
    d.zone("z-svc", 40, 630, width, 450, "Syslog, updates, menu, watchdog, temperatures", ZONE_GREEN)
    y1, y2, y3 = 680, 820, 960
    place("syslog", 0, 0, y1, 100, "syslog_service.cpp\nRFC 5424 events\nover UDP", BLUE)
    place("updsvc", 1, 1, y1, 100, "update_service.cpp\nhttp_fetch.cpp: site\ncheck-ins, downloads", BLUE)
    place("fwsvc", 2, 3, y1, 100, "fw_update_service.cpp\nboot_info.cpp: upload to the\nother slot, trial, rollback",
          BLUE)
    place("cli", 4, 5, y1, 100, "cli.cpp\nserial menu, hidden\nbench commands", BLUE)
    place("wdog", 6, 6, y1, 100, "watchdog.cpp\n8 s watchdog,\ncore 1 heartbeat", BLUE)
    place("host", 7, 7, y1, 100, "host_stats.cpp\ncpu_temp.cpp\nambient_temp.cpp", BLUE)
    place("slog", 0, 0, y2, 90, "syslog\nmessages, queue,\nserver settings", GREEN)
    place("updc", 1, 1, y2, 90, "update_client\ncheck-ins, MACs,\nURL, timing,\nCheckinStats", GREEN)
    place("fwu", 2, 2, y2, 90, "fw_update\nboot record, boot\ndecision, receiver,\ntrial monitor", GREEN)
    place("fwi", 3, 3, y2, 90, "fw_image\nslot header,\nSHA-512, verify", GREEN)
    place("netcfg", 4, 4, y2, 90, "netconfig\nIPv4/IPv6 parse\nand validate", GREEN)
    place("bench", 5, 5, y2, 90, "bench_info\nbenchinfo block\nfor the bench tool", GREEN)
    place("wdl", 6, 6, y2, 90, "watchdog_logic\nheartbeat check,\nrestart count", GREEN)
    place("tmp", 7, 7, y2, 90, "tmp1075, cpu_load\nTMP1075 reading,\nload per core", GREEN)
    place("boot", 2, 2, y3, 80, "boot program\nchecks signature,\npicks the slot", CYAN)
    place("mono", 3, 3, y3, 80, "monocypher\nEd25519\n(vendored)", GREEN)
    for a, b, slot in [("syslog", "slog", 0), ("updsvc", "updc", 1), ("fwsvc", "fwu", 2), ("fwsvc", "fwi", 3),
                       ("cli", "netcfg", 4), ("cli", "bench", 5), ("wdog", "wdl", 6), ("host", "tmp", 7),
                       ("fwi", "mono", 3)]:
        down(a, b, slot)
    cx = sx(2) + BW / 2
    d.arrow("boot", "top", "fwu", "bottom", a_at=cx, b_at=cx)
    d.arrow("boot", "right", "mono", "left")

    # Band 3: start-up, settings, mains input, reset button
    d.zone("z-start", 40, 1120, width, 500, "Start-up, settings, mains input, reset button", ZONE_YELLOW)
    y1, y2, y3 = 1170, 1330, 1490
    place("names", 0, 0, y1, 100, "hostname.cpp\nipv6_static.cpp\nname, static IPv6", BLUE)
    place("stack", 1, 1, y1, 100, "stack_watch.cpp\ncore 0 stack\nhigh-water mark", BLUE)
    place("main", 2, 6, y1, 100, "main.cpp\nboot, Ethernet, loop() on core 0, setup1() / loop1() on core 1;\n"
                                 "starts and runs every service above", BLUE)
    place("reset", 7, 7, y1, 100, "factory_reset.cpp\nGPIO22 on core 1", BLUE)
    place("settings", 2, 3, y2, 100, "settings\nLittleFS A/B files: settings,\nupdate, timelog", YELLOW)
    place("mains", 6, 6, y2, 100, "mains_input\nGPIO27 on core 1,\nseqlock", YELLOW)
    place("codec", 2, 2, y3, 90, "settings_codec\nJSON, validation,\nA/B selection", GREEN)
    place("power", 6, 7, y3, 90, "power_monitor\ndebouncer, 60 s mains alarm,\nreset button", GREEN)
    for a, b, slot in [("main", "settings", 2), ("main", "mains", 6), ("settings", "codec", 2),
                       ("mains", "power", 6), ("reset", "power", 7)]:
        down(a, b, slot)
    d.note(sx(4), y2 + 10, "cli.cpp and web_service.cpp save through\nsettings; the update and time/syslog\n"
                           "files use the codecs in update_client\nand syslog.")

    d.zone("z-core", 40, 1660, width, 150,
           "Arduino-Pico core (earlephilhower): used by src/ and the hardware layers", ZONE_GRAY)
    core = [(0, 1, "lwip", "lwIP + Wiznet5500lwIP\n(DHCP, SNTP, TCP/UDP)"), (2, 2, "lfs", "LittleFS"),
            (3, 3, "spi", "SPI, I2C (Wire)"), (4, 4, "usb", "USB serial"), (5, 6, "mc", "multicore\nsetup1 / loop1"),
            (7, 7, "tcp", "WiFiServer\n(TCP)")]
    for i, j, eid, text in core:
        place(eid, i, j, 1710, 70, text, GRAY)
    d.save()


def snmp_request_flow():
    d = Diagram("snmp-request-flow", "RF-Power-Monitor: how an SNMP request is handled")
    d.box("drop", 300, 90, 1180, 70,
          "Dropped without a reply: counted on the status screen, logged in the access log (menu 5) and to syslog",
          RED, 18)
    d.box("udp", 40, 250, 200, 90, "UDP datagram\non port 161", CYAN, 18)
    d.box("size", 300, 235, 200, 120, "at most\n1472 bytes?", YELLOW, 18, shape="diamond")
    d.box("rate", 620, 230, 230, 130, "Rate limiter\nper source 50/s\n(trusted: 200/s)\nuntrusted total 100/s\n"
          "trusted total 200/s", ORANGE, 16)
    d.box("parse", 970, 240, 230, 110, "Parse BER message\nversion, community,\nPDU, max 16 varbinds", BLUE, 16)
    d.box("ver", 1300, 235, 180, 120, "version?", YELLOW, 18, shape="diamond")
    d.arrow("udp", "right", "size", "left")
    d.arrow("size", "right", "rate", "left", "yes", b_at=295)
    d.arrow("rate", "right", "parse", "left", "allowed")
    d.arrow("parse", "right", "ver", "left")
    for eid, label in [("size", "no"), ("rate", "over limit"), ("parse", "malformed"), ("ver", "v3")]:
        x0, _, x1, _ = d.shapes[eid]
        d.arrow(eid, "top", "drop", "bottom", label, b_at=(x0 + x1) / 2)

    d.box("comm", 1300, 470, 180, 130, "community\ncorrect?", YELLOW, 18, shape="diamond")
    d.box("pdu", 880, 480, 280, 110, "Handle the PDU\nGet, GetNext, GetBulk (v2c)\nSet is rejected", BLUE, 16)
    d.box("mib", 520, 455, 260, 160, "MIB: 250 objects + a row\nper address: SNMPv2, IF,\nIP, HOST-RESOURCES,\n"
                                     "ENTITY, LLDP, 63050\n(mains, firmware,\nupdate site)", GREEN, 16)
    d.box("build", 140, 480, 280, 110, "Build GetResponse\nv1 errors / v2c exceptions\nGetBulk at most 16 varbinds",
          BLUE, 16)
    d.box("sent", 140, 690, 280, 90, "Reply sent\nsource now trusted\n(not for GetBulk)", GREEN, 16)
    d.box("auth", 1275, 690, 230, 100, "authenticationFailure\ntrap (max 1 per 10 s)\n+ access log, syslog", RED, 16)
    d.arrow("ver", "bottom", "comm", "top", "v1 / v2c")
    d.arrow("comm", "left", "pdu", "right", "yes")
    d.arrow("comm", "bottom", "auth", "top", "no")
    d.arrow("pdu", "left", "mib", "right")
    d.arrow("mib", "left", "build", "right")
    d.arrow("build", "bottom", "sent", "top")
    d.save()


def mains_flow():
    d = Diagram("mains-flow", "RF-Power-Monitor: from the mains input to an alert")
    d.zone("z-c1", 40, 80, 920, 890, "Core 1: setup1() / loop1(), every millisecond", ZONE_ORANGE)
    d.box("gpio", 70, 170, 180, 90, "GPIO27\nmains detector", ORANGE, 18)
    d.box("sample", 300, 170, 180, 90, "Sample the pin\nevery 1 ms", ORANGE, 16)
    d.box("deb", 530, 170, 180, 90, "Debouncer\nlevel must hold\n5 ms", ORANGE, 16)
    d.box("seq", 760, 160, 170, 110, "Seqlock snapshot\npresent, changes,\nlast change", GREEN, 16)
    d.arrow("gpio", "right", "sample", "left")
    d.arrow("sample", "right", "deb", "left")
    d.arrow("deb", "right", "seq", "left")
    d.note(300, 290, "a real change is seen within 6 ms;\nglitches shorter than 5 ms are ignored")

    d.box("btn", 70, 710, 180, 80, "GPIO22\nreset button", GRAY, 18)
    d.box("rb", 300, 695, 180, 110, "ResetButton\n10 s hold, release\nbefore cancels", GRAY, 16)
    d.box("flag", 530, 710, 180, 80, "Reset requested\n(atomic flag)", GRAY, 16)
    d.box("led", 300, 860, 180, 80, "LED\nslow / fast blink", YELLOW, 16)
    d.arrow("btn", "right", "rb", "left")
    d.arrow("rb", "right", "flag", "left")
    d.arrow("rb", "bottom", "led", "top")

    d.zone("z-c0", 1040, 80, 560, 890, "Core 0: loop()", ZONE_BLUE)
    d.box("read", 1070, 150, 180, 500, "Read snapshot\n(retries while\ncore 1 writes)", BLUE, 16)
    d.box("log", 1340, 150, 230, 70, "Serial log +\nstatus screen", BLUE, 16)
    d.box("obj", 1340, 250, 230, 120, "SNMP objects\n63050.1 / .2 / .3\n.6 alarm: lost\nfor 60 s", BLUE, 16)
    d.box("trap", 1340, 400, 230, 120, "Traps\nmainsPowerLost,\nRestored, and\nLostTooLong (60 s)", VIOLET, 16)
    d.box("slog", 1340, 550, 230, 100, "Syslog MAINS: lost,\nrestored (notice),\n60 s alarm (critical)", VIOLET, 16)
    d.box("fr", 1070, 700, 180, 100, "Factory reset\nsave defaults,\nreboot within 2 s", RED, 16)
    d.arrow("seq", "right", "read", "left", a_at=215, b_at=215)
    for eid in ("log", "obj", "trap", "slog"):
        y0, y1 = d.shapes[eid][1], d.shapes[eid][3]
        d.arrow("read", "right", eid, "left", a_at=(y0 + y1) / 2, b_at=(y0 + y1) / 2)
    d.arrow("flag", "right", "fr", "left", a_at=750, b_at=750)

    d.zone("z-net", 1710, 80, 360, 890, "Network", ZONE_GRAY)
    d.box("lnms", 1740, 240, 300, 140, "LibreNMS\npolls every 5 min\n\"Mains lost over 60 s\"\ncritical: alert\n"
                                      "(lost alone: warning)", VIOLET, 16)
    d.box("rx", 1740, 410, 300, 100, "Trap receiver /\nLibreNMS event log;\na mains trap starts a poll", VIOLET, 16)
    d.box("syslogsrv", 1740, 550, 300, 100, "Syslog server\n(LibreNMS syslog page;\ntest setup: UDP 5140)",
          VIOLET, 16)
    d.arrow("obj", "right", "lnms", "left", "UDP 161", a_at=310, b_at=310)
    d.arrow("trap", "right", "rx", "left", "UDP 162", a_at=460, b_at=460)
    d.arrow("slog", "right", "syslogsrv", "left", "UDP 514", a_at=600, b_at=600)
    d.note(1040, 1000, "A short outage between two polls still shows: mainsPowerChanges goes up by 2, and the "
                       "trap and\nsyslog events record it. Only an outage of 60 s or more raises the alarm and the "
                       "alert.")
    d.save()


def test_layers():
    d = Diagram("test-layers", "RF-Power-Monitor: test layers")
    layers = [
        ("lnms", "LibreNMS in Docker: definitions, sensors, LLDP neighbour, syslog\nreceiver, alert on a real mains cut",
         VIOLET),
        ("mains", "Mains HIL: a Dirigera smart plug switches real mains", RED),
        ("hil", "HIL (Python): menu, SNMP, traps, web, IPv6, DHCP options, DHCPv6 test\nnetwork, LLDP, NTP, syslog, "
                "watchdog, firmware updates, update site", ORANGE),
        ("emb", "On-device Unity tests: LittleFS, settings, W5500, GPIO27 sampler,\nfirmware SHA-512 and flash writes",
         YELLOW),
        ("host", "Host agent vs real net-snmp (WSL): 100+ checks, all MIBs, MIB validation", CYAN),
        ("san", "Sanitizers (WSL): every native suite under ASan + UBSan", CYAN),
        ("native", "Native unit tests: 1000+ tests of lib/, frames from the device,\nKea, Junos and net-snmp; "
                   "update site and tool tests (Python)", GREEN),
    ]
    for i, (eid, text, bg) in enumerate(layers):
        d.box(eid, 120, 110 + i * 100, 820, 80, text, bg, 18)
    d.line_arrow((65, 790), (65, 110))
    d.note(10, 800, "fast, runs everywhere")
    d.note(10, 84, "slower, real hardware")

    d.box("pc", 1040, 110, 330, 380, "Your PC\nPico on USB (RFPOWER_PORT)\nWSL: net-snmp, Docker\nDirigera hub\n"
          "switch console\n\nrun locally", GRAY, 18)
    d.box("ci", 1040, 510, 330, 280, "GitHub Actions CI\non every push\n\nnative tests\nsanitizers\nnet-snmp check\n"
          "tool and site tests\nfirmware build (.uf2)", GREEN, 18)
    for i, (eid, _, _) in enumerate(layers):
        y = 150 + i * 100
        d.arrow(eid, "right", "pc" if i < 4 else "ci", "left", a_at=y, b_at=y)
    d.save()


def settings_storage():
    d = Diagram("settings-storage", "RF-Power-Monitor: power-cut safe settings (A/B slots)")
    d.text(40, 90, "Saving", size=24)
    d.box("new", 40, 140, 220, 90, "New settings\n(menu or web page)", BLUE, 16)
    d.box("codec", 320, 140, 220, 90, "settings_codec\nto JSON", GREEN, 16)
    d.box("write", 640, 135, 230, 100, "Write the slot\nnot in use,\nsequence + 1", YELLOW, 16)
    d.box("verify", 940, 140, 220, 90, "Read back\nand validate", YELLOW, 16)
    d.box("active", 1230, 140, 220, 90, "New copy is\nthe active one", GREEN, 16)
    d.arrow("new", "right", "codec", "left")
    d.arrow("codec", "right", "write", "left")
    d.arrow("write", "right", "verify", "left")
    d.arrow("verify", "right", "active", "left")

    d.zone("z-flash", 300, 300, 620, 170, "Internal flash, LittleFS", ZONE_YELLOW)
    d.box("a", 330, 350, 260, 90, "settings_a.json\nsequence 7", GREEN, 18)
    d.box("b", 625, 350, 260, 90, "settings_b.json\nsequence 8 (newest)", GREEN, 18)
    d.arrow("write", "bottom", "b", "top", "writes", a_at=755, b_at=755, lpos=0.3)

    d.text(330, 500, "Booting", size=24)
    d.box("boot", 330, 560, 555, 80, "Read both slots", BLUE, 16)
    d.box("valid", 980, 540, 200, 120, "valid\ncopies?", YELLOW, 18, shape="diamond")
    d.box("use", 1320, 555, 300, 90, "Use the valid copy with\nthe highest sequence", GREEN, 16)
    d.box("defaults", 970, 760, 220, 90, "None valid:\nsave factory defaults", RED, 16)
    d.arrow("a", "bottom", "boot", "top", a_at=460, b_at=460)
    d.arrow("b", "bottom", "boot", "top", a_at=755, b_at=755)
    d.arrow("boot", "right", "valid", "left", a_at=600)
    d.arrow("valid", "right", "use", "left", "one or two")
    d.arrow("valid", "bottom", "defaults", "top", "none")
    d.note(40, 300, "Power cut during a save:\nthe half-written slot\nfails validation, so the\n"
                    "other slot (the previous\nsettings) is used.")
    d.note(40, 680, "Two more pairs use the same\nscheme in their own files:\nupdate_a/b.json (update site,\n"
                    "3.4.0) and timelog_a/b.json\n(NTP and syslog servers,\n3.6.0). The main file and\n"
                    "older firmware are unchanged.")
    d.save()


def update_site_flow():
    d = Diagram("update-site-flow", "RF-Power-Monitor: check-ins and updates from the update site")
    d.zone("z-site", 20, 80, 2210, 300, "Update site (Docker, TCP 8071)", ZONE_GRAY)
    top, bottom = 170, 280
    d.box("admin", 40, top, 250, 110, "Admin (login):\nupload a signed .upd,\ntick devices", BLUE, 16)
    d.box("store", 340, top, 200, 110, "Releases and\nassignments\n(SQLite)", GREEN, 16)
    d.box("verify", 600, top, 220, 110, "Check the MAC\n(shared key) and\nthe nonce (replay)", YELLOW, 16)
    d.box("decide", 870, top, 260, 110, "Reply: ok, or update\n<version> + path;\nMAC over the nonce", GREEN, 16)
    d.box("serve", 1220, top, 240, 110, "GET /releases/\n<version>.upd\n(no login)", GREEN, 16)
    d.box("status", 1510, top, 630, 110, "Devices page: online, versions; update done /\nrolled back / refused / "
                                         "failed / lost", VIOLET, 16)
    d.arrow("admin", "right", "store", "left")
    d.arrow("store", "top", "decide", "top", "assignments", via=[(440, 120), (1000, 120)], size=14)

    d.zone("z-dev", 20, 480, 2210, 480,
           "RF-Power-Monitor: first check-in 60 s after the network is up, then every 30 min", ZONE_BLUE,
           title_at="bottom")
    row1, row2 = 550, 790
    d.box("checkin", 40, row1, 270, 120, "Check-in: serial, hostname,\naddresses, version, last\nresult, nonce; MAC",
          BLUE, 16)
    d.box("ignore", 590, row1, 250, 120, "No reply or wrong MAC:\nignored, retry in 5 min;\nsyslog, SNMP updateSite",
          RED, 16)
    d.box("chk", 900, row1 - 15, 200, 150, "reply MAC\nright?", YELLOW, 18, shape="diamond")
    d.box("dl", 1215, row1, 250, 120, "update <version>:\ndownload the .upd\n(small TCP window)", BLUE, 16)
    d.box("checks", 1530, row1 - 5, 270, 130, "Before writing: header\nand signed version =\n<version>? signature "
                                              "and\nkey OK? 3.4.0 or newer?", YELLOW, 16)
    d.box("refuse", 1870, row1, 270, 120, "Refused: the other slot\nis kept; reported at\nthe next check-in", RED, 16)
    d.box("ok", 900, row2, 200, 110, "ok: nothing to do;\nconfirms a\nfirmware on trial", GREEN, 16)
    d.box("install", 1530, row2, 270, 110, "Write the other slot,\nrestart on trial", GREEN, 16)
    d.box("report", 1870, row2, 270, 110, "Next check-in confirms\nthe trial, then reports\nkept (or rolled back)",
          GREEN, 16)

    d.arrow("checkin", "top", "verify", "bottom", "POST /api/checkin", via=[(175, 330), (710, 330)], size=14)
    d.arrow("verify", "right", "decide", "left")
    d.arrow("decide", "bottom", "chk", "top", "reply", a_at=1000, size=14, lpos=0.55)
    d.arrow("chk", "left", "ignore", "right", "no", lside="above")
    d.arrow("chk", "bottom", "ok", "top", "ok")
    d.arrow("chk", "right", "dl", "left", "update")
    d.arrow("dl", "top", "serve", "bottom", "GET", a_at=1340, b_at=1340, dashed=True, size=14, lpos=0.55)
    d.arrow("dl", "right", "checks", "left")
    d.arrow("checks", "right", "refuse", "left", "no")
    d.arrow("checks", "bottom", "install", "top", "yes")
    d.arrow("install", "right", "report", "left")
    d.arrow("refuse", "top", "status", "bottom", a_at=2005, b_at=2005, dashed=True)
    d.arrow("report", "right", "status", "right", "reported", via=[(2190, 845), (2190, 225)], size=14,
            lside="left", lpos=0.68)
    d.note(40, 985, "Only a reply with the right MAC (the shared key, over this check-in's nonce) is acted on, and "
                    "the device still installs\nonly firmware signed with your key. Releases are never retried in a "
                    "loop: an update ends as done, rolled back, refused,\nfailed (3 broken downloads) or lost (no report "
                    "in 2 hours). The site cannot reach a device: there is no \"update now\".")
    d.save()


def web_request_flow():
    d = Diagram("web-request-flow", "RF-Power-Monitor: how a web page request is handled")
    d.box("conn", 40, 300, 200, 90, "Browser connects\nTCP port 80", CYAN, 18)
    d.box("read", 300, 285, 220, 120, "Read the request\nnon-blocking,\nat most 2 KB, 5 s", BLUE, 16)
    d.box("closed", 300, 80, 220, 100, "Closed, or\n400 / 413", RED, 18)
    d.box("path", 600, 220, 160, 250, "which\npath?", YELLOW, 18)
    d.box("static", 560, 70, 240, 100, "Logo and stylesheet\n(gzip, from flash)\nsent without login", GREEN, 16)
    d.arrow("conn", "right", "read", "left")
    d.arrow("read", "top", "closed", "bottom", "too slow, malformed\nor too large", lside="left")
    d.arrow("read", "right", "path", "left", a_at=345, b_at=345)
    d.arrow("path", "top", "static", "bottom", "/favicon.svg\n/static/...")

    # GET /
    d.box("getdia", 900, 200, 200, 120, "session\ncookie valid?", YELLOW, 16, shape="diamond")
    d.box("login", 900, 80, 200, 65, "Login page", BLUE, 18)
    d.box("page", 1240, 215, 300, 90, "Status and settings page\nforms carry the CSRF token", GREEN, 16)
    d.arrow("path", "right", "getdia", "left", "GET /", a_at=260, b_at=260)
    d.arrow("getdia", "top", "login", "bottom", "no")
    d.arrow("getdia", "right", "page", "left", "yes")

    # POST /login
    d.box("check", 900, 375, 300, 130, "Check the password\ndefault: constant-time compare\nown: salted SHA-256, "
                                       "2000\nrounds in steps, SNMP answers", BLUE, 16)
    d.box("lock", 1300, 380, 200, 120, "login\nlocked?", YELLOW, 18, shape="diamond")
    d.box("locked", 1300, 590, 200, 90, "429\naccess log", RED, 16)
    d.box("ok", 1700, 380, 180, 120, "right?", YELLOW, 18, shape="diamond")
    d.box("session", 1640, 190, 300, 110, "New session: cookie sid\nHttpOnly, SameSite=Strict\nmax 2, ends after 15 "
                                          "min idle", GREEN, 16)
    d.box("wrong", 1680, 590, 220, 90, "401, access log\n5 in a row: 30 s lock", RED, 16)
    d.arrow("path", "right", "check", "left", "POST /login", a_at=440, b_at=440, lpos=0.45)
    d.arrow("check", "right", "lock", "left")
    d.arrow("lock", "bottom", "locked", "top", "yes")
    d.arrow("lock", "right", "ok", "left", "no")
    d.arrow("ok", "top", "session", "bottom", "yes", b_at=1790)
    d.arrow("ok", "bottom", "wrong", "top", "no")
    d.arrow("session", "left", "page", "right", "redirect\nto /", a_at=260, b_at=260)

    # Actions
    d.box("post", 900, 735, 260, 130, "POST /network /ipv6\n/hostname /community\n/timelog /updateserver\n"
                                      "/firmware/switch /reboot\n/disable /logout", BLUE, 16)
    d.box("csrf", 1260, 735, 200, 130, "session and\nCSRF token\nvalid?", YELLOW, 16, shape="diamond")
    d.box("forbid", 1260, 950, 200, 90, "403: Not logged in /\nForm expired", RED, 16)
    d.box("valid", 1570, 740, 180, 120, "fields\nvalid?", YELLOW, 18, shape="diamond")
    d.box("bad", 1560, 950, 200, 90, "400: reason on the\npage, nothing saved", RED, 16)
    d.box("save", 1870, 700, 320, 200, "community, hostname, time\nand syslog, update server:\nsaved, active now; "
                                       "network,\nIPv6: saved, restart;\nfirmware/switch: other slot;\nreboot: restart; "
                                       "off: port 80\nclosed; logout: session ends\n(500 if saving fails)", GREEN, 16)
    d.arrow("path", "bottom", "post", "left", "POST action", via=[(680, 800)], lseg=0)
    d.arrow("post", "right", "csrf", "left")
    d.arrow("csrf", "bottom", "forbid", "top", "no")
    d.arrow("csrf", "right", "valid", "left", "yes")
    d.arrow("valid", "bottom", "bad", "top", "no")
    d.arrow("valid", "right", "save", "left", "yes")
    d.note(40, 560, "One connection at a time, never\nblocking: SNMP keeps answering\nin between.\n\n"
                    "Plain HTTP: use the page on a\ntrusted management network.")
    d.save()


def network_addresses():
    d = Diagram("network-addresses", "RF-Power-Monitor: where the addresses come from (dual stack)")
    d.zone("z-v4", 20, 80, 1190, 560, "IPv4 (and the server addresses)", ZONE_BLUE)
    d.box("dhcpsrv", 40, 260, 260, 90, "DHCP server\non the LAN", GRAY, 18)
    d.box("dhcp", 380, 130, 320, 360, "DHCP (default)\naddress, mask,\ngateway, DNS;\nsends the hostname\n"
                                      "(option 12), asks\nfor options 7, 42,\n224 and 225", BLUE, 16)
    d.box("static4", 380, 520, 320, 90, "Static IPv4\nserial menu 3 or web page\n(restart)", BLUE, 16)
    right = [("trap", 130, "Trap destination: fixed\n(menu 4, IPv4 or IPv6)\nor DHCP option 224", ORANGE, "option 224"),
             ("ntp", 240, "NTP server: typed (menu 11,\nweb page), DHCP option 42\nor DHCPv6 option 31", ORANGE,
              "option 42"),
             ("slogsrv", 350, "Syslog server: typed\n(menu 11, web page)\nor DHCP option 7", ORANGE, "option 7")]
    for eid, y, text, bg, label in right:
        d.box(eid, 840, y, 330, 90, text, bg, 16)
        d.arrow("dhcp", "right", eid, "left", label, a_at=y + 45, b_at=y + 45)
    d.box("ip4", 840, 460, 330, 160, "IPv4 address\nDHCP or static", GREEN, 18)
    d.arrow("dhcpsrv", "right", "dhcp", "left", a_at=305, b_at=305)
    d.arrow("dhcp", "right", "ip4", "left", a_at=475, b_at=475)
    d.arrow("static4", "right", "ip4", "left", "or", a_at=565, b_at=565)

    d.zone("z-v6", 20, 670, 1190, 560, "IPv6", ZONE_GREEN)
    d.box("ra", 40, 845, 260, 100, "IPv6 router\nrouter advertisements", GRAY, 18)
    d.box("ll", 380, 720, 320, 90, "Link-local fe80::\nfrom the MAC (EUI-64), always", BLUE, 16)
    d.box("slaac", 380, 835, 320, 120, "SLAAC: one address per\nadvertised /64; DHCPv6: one\naddress when the router\n"
                                       "sets the M flag", BLUE, 16)
    d.box("static6", 380, 980, 320, 120, "Static IPv6 (/64)\nserial menu 9 or web page\n(restart)", BLUE, 16)
    d.box("ip6", 870, 720, 300, 300, "IPv6 addresses\nup to 4, each passes\nduplicate address\ndetection", GREEN, 16)
    d.box("gw", 870, 1050, 300, 100, "IPv6 gateway\nthe static one if set,\nelse advertised routers", YELLOW, 16)
    d.arrow("ra", "right", "slaac", "left", a_at=895, b_at=895)
    d.arrow("ll", "right", "ip6", "left", a_at=765, b_at=765)
    d.arrow("slaac", "right", "ip6", "left", a_at=895, b_at=895)
    d.arrow("static6", "right", "ip6", "left", "next to SLAAC", a_at=1000, b_at=1000)
    d.arrow("static6", "right", "gw", "left", "gateway", a_at=1080, b_at=1080)
    d.arrow("ra", "bottom", "gw", "bottom", "routers", via=[(170, 1190), (1020, 1190)])

    d.box("traps", 1290, 130, 320, 90, "Traps (UDP 162)\nto IPv4, or v2c to IPv6", ORANGE, 16)
    d.box("time", 1290, 240, 320, 90, "Clock: SNTP every 15 min\n(UDP 123)", ORANGE, 16)
    d.box("slog", 1290, 350, 320, 90, "Syslog events, RFC 5424\n(UDP 514 unless given)", ORANGE, 16)
    d.box("svc", 1290, 460, 320, 560, "Services on every address\n\nSNMP, UDP 161\n\nweb page, TCP 80\n\n"
          "mDNS name.local\n(A and AAAA)\n\nping", VIOLET, 18)
    d.arrow("trap", "right", "traps", "left")
    d.arrow("ntp", "right", "time", "left")
    d.arrow("slogsrv", "right", "slog", "left")
    d.arrow("ip4", "right", "svc", "left", a_at=540, b_at=540)
    d.arrow("ip6", "right", "svc", "left", a_at=870, b_at=870)
    d.note(1290, 1060, "IPv4 and IPv6 run side by side.\nRate limits and the access log\nwork per full source "
                       "address.\nThe servers may be IPv4 or IPv6.")
    d.save()


def firmware_update_boot():
    d = Diagram("firmware-update-boot", "RF-Power-Monitor: flash layout and which firmware starts")
    # Flash layout (scripts/slot_build.py, lib/fw_image fw_layout.h, boot/boot.ld); not to scale
    d.zone("z-flash", 20, 80, 430, 980, "Flash, 2 MB (not to scale)", ZONE_GRAY)
    regions = [(0x000000, 130, 90, "boot2 (256 bytes) +\nboot program", CYAN),
               (0x00E000, 235, 65, "boot record, 2 x 4 KB\n(0xE000, 0xF000)", YELLOW),
               (0x010000, 315, 245, "slot A, 860 KB\n256-byte header\n(version, Ed25519\nsignature), then\n"
                                    "the firmware", BLUE),
               (0x0E7000, 575, 245, "slot B, 860 KB\nsame layout", BLUE),
               (0x1BE000, 835, 35, "unused", WHITE),
               (0x1BF000, 885, 90, "LittleFS, 256 KB\nsettings, logs", GREEN),
               (0x1FF000, 990, 45, "EEPROM (4 KB)", GRAY)]
    for addr, y, h, text, bg in regions:
        d.box("r%X" % addr, 180, y, 250, h, text, bg, 16)
        d.note(40, y + 4, "0x%06X" % addr)

    d.zone("z-boot", 520, 80, 1260, 970, "Boot program (boot/, runs from 0x100 after boot2)", ZONE_BLUE)
    d.box("reset", 580, 130, 360, 60, "Reset or power on", GRAY, 18)
    d.box("why", 580, 240, 360, 90, "Why the reset? power-on, RUN pin,\nplanned or crash (watchdog);\n"
                                    "crash count (scratch1)", BLUE, 16)
    d.box("rec", 580, 380, 360, 100, "Both boot record copies: the newest\nvalid one; target = its trial slot,\n"
                                     "else its active slot (none: slot A)", BLUE, 16)
    d.box("sig", 610, 530, 300, 140, "target's Ed25519\nsignature OK?\n(SHA-512)", YELLOW, 16, shape="diamond")
    d.box("lim", 610, 730, 300, 140, "3rd crash in a\nrow on this slot?", YELLOW, 16, shape="diamond")
    d.box("start", 580, 930, 360, 90, "Start the target: slot, reason and\ncrash count in scratch0-2;\n"
                                      "watchdog armed (~8.3 s)", GREEN, 16)
    d.box("other", 1060, 530, 260, 140, "other slot\nverifies?", YELLOW, 16, shape="diamond")
    d.box("none", 1040, 370, 300, 100, "Nothing bootable:\nLED 3 short, 1 long;\nwatchdog off", RED, 16)
    d.box("fallback", 1430, 550, 300, 100, "Fallback: start the\nother slot (count 0)", ORANGE, 16)
    d.arrow("reset", "bottom", "why", "top")
    d.arrow("why", "bottom", "rec", "top")
    d.arrow("rec", "bottom", "sig", "top")
    d.arrow("sig", "bottom", "lim", "top", "yes")
    d.arrow("lim", "bottom", "start", "top", "no")
    d.arrow("sig", "right", "other", "left", "no")
    d.arrow("lim", "right", "other", "bottom", "yes", via=[(1190, 800)], lseg=0)
    d.arrow("other", "top", "none", "bottom", "no", a_at=1190, b_at=1190)
    d.arrow("other", "right", "fallback", "left", "yes")

    d.zone("z-fw", 520, 1090, 1260, 380, "Firmware (src/fw_update_service.cpp, lib/fw_update)", ZONE_GREEN,
           title_x=460)
    d.box("trial", 580, 1140, 360, 300, "On trial (the record\nnames a trial slot):\nwait for a valid request\n"
                                        "(SNMP, or a logged-in\nweb request)", BLUE, 16)
    d.box("kept", 1040, 1140, 380, 80, "Confirmed: the record keeps the\nslot (trap firmwareUpdateKept)", GREEN, 16)
    d.box("unreach", 1040, 1245, 380, 100, "No request with the network up\n10 min, or 1 h in all: record back\n"
                                           "to the old slot, restart", ORANGE, 16)
    d.box("crash", 1040, 1370, 380, 70, "Crash or hang (watchdog):\nrestart, counted", RED, 16)
    d.box("fbrec", 1480, 1140, 270, 120, "After a fallback the\nfirmware rewrites the\nrecord at once (trap\n"
                                         "firmwareRolledBack)", ORANGE, 16)
    d.arrow("start", "bottom", "trial", "top")
    for eid in ("kept", "unreach", "crash"):
        y0, y1 = d.shapes[eid][1], d.shapes[eid][3]
        d.arrow("trial", "right", eid, "left", a_at=(y0 + y1) / 2, b_at=(y0 + y1) / 2)
    d.arrow("fallback", "bottom", "fbrec", "top", a_at=1615, b_at=1615)
    d.arrow("crash", "right", "reset", "right", "restart: the boot\nprogram runs again", via=[(1830, 1405), (1830, 160)],
            lseg=1, lpos=0.6, lside="right")
    d.note(40, 1100, "The boot program only reads the boot record;\nthe firmware writes it (two copies, newest\n"
                     "valid wins). A planned restart is not a crash.")
    d.save()


def time_syslog_flow():
    d = Diagram("time-syslog-flow", "RF-Power-Monitor: time from NTP, events to syslog")
    d.box("ntpsrc", 680, 90, 340, 80, "NTP server: DHCP option 42,\nDHCPv6 option 31, typed or none", ORANGE, 16)
    d.box("sntp", 1090, 90, 200, 80, "lwIP SNTP\nevery 15 min", BLUE, 16)
    d.box("clock", 1370, 90, 240, 80, "Clock (UTC; shown\nas CET/CEST)", GREEN, 16)
    d.arrow("ntpsrc", "right", "sntp", "left")
    d.arrow("sntp", "right", "clock", "left")

    d.box("events", 40, 210, 340, 260, "MAINS lost, restored, 60 s alarm\nBOOT each start: cause, slot\n"
                                       "FIRMWARE kept, rolled back\nSETTINGS from web page or menu\n"
                                       "WEB admin logged in\nNETWORK link, IPv4 address\nLLDP switch port changed\n"
                                       "UPDATE check-ins fail / work", BLUE, 16)
    d.box("access", 40, 510, 340, 90, "Access events: failed web\nlogins, lockouts, rejected\nSNMP requests", BLUE, 16)
    d.box("throttle", 450, 495, 230, 120, "5 lines a minute;\nthe rest counted:\n\"N more access\nevents not sent\"",
          ORANGE, 16)
    d.box("queue", 760, 250, 250, 400, "Queue of 16\n(text + uptime)\n\nfull: the oldest\nis dropped, then\n"
                                       "\"N events dropped\n(queue full)\"", YELLOW, 16)
    d.arrow("events", "right", "queue", "left", a_at=330, b_at=330)
    d.arrow("access", "right", "throttle", "left", a_at=555, b_at=555)
    d.arrow("throttle", "right", "queue", "left", a_at=555, b_at=555)

    d.box("net", 1070, 250, 240, 160, "network up,\nserver known?", YELLOW, 16, shape="diamond")
    d.box("time", 1380, 250, 220, 160, "clock set\nby NTP?", YELLOW, 16, shape="diamond")
    d.box("twomin", 1380, 500, 220, 160, "2 min since\nthe server\nwas known?", YELLOW, 16, shape="diamond")
    d.box("wait", 1080, 540, 220, 80, "Events wait\nin the queue", GRAY, 16)
    d.box("dated", 1690, 280, 300, 100, "Dated from when it\nhappened: now minus the\nuptime since then", GREEN, 16)
    d.box("undated", 1690, 530, 300, 100, "Undated (RFC 5424 \"-\"),\ntext starts with\n\"[uptime N s]\"", ORANGE, 16)
    d.box("send", 2080, 250, 320, 410, "RFC 5424 over UDP:\n<PRI>1 TIME HOST\nrfpower - MSGID - text\n\n"
                                       "facility daemon;\none event per loop pass;\nall at once before a\nplanned restart",
          BLUE, 16)
    d.box("server", 2080, 760, 320, 100, "Syslog server: typed (menu 11,\nweb page) or DHCP option 7;\n"
                                         "UDP port 514 unless given", VIOLET, 16)
    d.arrow("queue", "right", "net", "left", a_at=330, b_at=330)
    d.arrow("net", "right", "time", "left", "yes")
    d.arrow("net", "bottom", "wait", "top", "no", a_at=1190, b_at=1190)
    d.arrow("clock", "bottom", "time", "top", a_at=1490, b_at=1490, dashed=True)
    d.arrow("time", "right", "dated", "left", "yes")
    d.arrow("time", "bottom", "twomin", "top", "no")
    d.arrow("twomin", "left", "wait", "right", "no", lside="above")
    d.arrow("twomin", "right", "undated", "left", "yes")
    d.arrow("dated", "right", "send", "left", a_at=330, b_at=330)
    d.arrow("undated", "right", "send", "left", a_at=580, b_at=580)
    d.arrow("send", "bottom", "server", "top")
    d.note(40, 700, "Severity: notice, warning, error or critical; HOSTNAME is the device's hostname, timestamps are UTC.\n"
                    "Events are dated from the time they happened, so the queue can wait for the clock without "
                    "losing it.\nNeither server is built in: with none set, the device contacts no NTP or syslog "
                    "server.")
    d.save()


def librenms_integration():
    d = Diagram("librenms-integration", "RF-Power-Monitor: how LibreNMS sees the device")
    d.box("dev", 40, 150, 320, 520, "RF-Power-Monitor\n\nSNMP agent\nUDP 161, v1 / v2c\n\ntraps\nUDP 162, v1 or v2c\n\n"
                                    "syslog\nRFC 5424 over UDP", BLUE, 18)
    d.zone("z-lnms", 520, 80, 1400, 680, "LibreNMS (test setup: Docker, librenms/test/compose.yml)", ZONE_GRAY)
    d.box("poller", 580, 150, 330, 130, "poller\nSNMP polls every 5 min\n(and right after a\nmains trap)", BLUE, 16)
    d.box("trapd", 580, 340, 330, 120, "snmptrapd sidecar\nsnmptrap-agent-addr.php\nfinds the device by the\n"
                                       "address in the trap", BLUE, 16)
    d.box("slogng", 580, 530, 330, 120, "syslogng sidecar\nRFC 5424 source on UDP\n5140, keeps the message's\n"
                                        "hostname", BLUE, 16)
    d.arrow("poller", "left", "dev", "right", "UDP 161", a_at=215, b_at=215, both=True, lpos=0.67)
    d.arrow("dev", "right", "trapd", "left", "UDP 162", a_at=400, b_at=400, lpos=0.33)
    d.arrow("dev", "right", "slogng", "left", "UDP 5140", a_at=590, b_at=590, lpos=0.33)
    d.arrow("trapd", "top", "poller", "bottom", "mains trap:\npoll now", a_at=745, b_at=745)

    d.box("sensors", 1010, 120, 420, 240, "Device page: OS RF-Power-Monitor\nmains power, mains lost over 60 s,\n"
                                         "mains changes, CPU and ambient\ntemperature, firmware state and\n"
                                         "last update, update-site check-in,\nminutes since check-in (warn > 60);\n"
                                         "CPU, RAM, LittleFS, eth0, IPv4/IPv6,\nLLDP neighbour, inventory", GREEN, 16)
    d.box("events", 1010, 360 + 30, 420, 80, "Event log: the traps (mains,\nfirmware, coldStart, auth. failure)",
          GREEN, 16)
    d.box("syslogp", 1010, 540, 420, 100, "Logs > Syslog: matched to the\ndevice by its hostname (sysName);\n"
                                          "needs enable_syslog", GREEN, 16)
    d.arrow("poller", "right", "sensors", "left", a_at=215, b_at=215)
    d.arrow("trapd", "right", "events", "left", a_at=430, b_at=430)
    d.arrow("slogng", "right", "syslogp", "left", a_at=590, b_at=590)

    d.box("rules", 1520, 150, 360, 150, "Alert rules: State Sensor\nCritical (mains lost over 60 s,\n"
                                       "rollback or fallback) and\nthe sensor limits", ORANGE, 16)
    d.box("notify", 1520, 380, 360, 90, "Alert log and notifications\n(mail, chat, ...)", RED, 16)
    d.arrow("sensors", "right", "rules", "left", a_at=225, b_at=225)
    d.arrow("rules", "bottom", "notify", "top")
    d.note(40, 790, "Definitions: librenms/os_detection and os_discovery (rfpowermonitor.yaml) and the MIB, "
                    "see docs/LIBRENMS.md. A fresh LibreNMS has no alert rules:\nadd \"State Sensor Critical\" "
                    "from the rule collection. Short outages only show as a warning and a step in \"Mains power "
                    "changes\".\nDocker Desktop hides the sender's address: the trap handler uses the address in "
                    "the trap, syslog-ng keeps the hostname.")
    d.save()


def bench_setup():
    d = Diagram("bench-setup", "RF-Power-Monitor: setting up a new board (tools/bench_provision.py)")
    steps = [
        ("pre", 100, 80, "Before touching the board: bench.ini\nreadable (if there), devices.csv writable"),
        ("sig", 230, 90, "Release signed with the field key? fwsign\nverify, the boot program's own check\n"
                         "(--dev allows the development key)"),
        ("flash", 370, 90, "Hold BOOTSEL and plug in the board: copy\nthe -usb.uf2 to the RPI-RP2 drive, wait\n"
                           "for its new serial port (one board only)"),
        ("read", 510, 90, "benchinfo (hidden menu command): hostname,\nMAC, serial, firmware, key id, default\n"
                          "web password and its lock"),
        ("check", 650, 90, "Checks: field key, a valid password,\nthe flashed version, hostname\n"
                           "RF-Power-<end of MAC> (after flashing)"),
        ("lock", 790, 80, "pwlock: lock the default web password\n(menu option 7 then refuses to replace it)"),
        ("upd", 920, 90, "With bench.ini: menu option 10 = URL and\nkey; updatedefault keeps it, a factory\n"
                         "reset restores it"),
        ("label", 1060, 100, "bench/<hostname>.png: 40 x 40 mm label\n(NIIMBOT, white ribbon on black) and a\n"
                             "preview; a row in bench/devices.csv"),
    ]
    for eid, y, h, text in steps:
        d.box(eid, 440, y, 440, h, text, GREEN if eid == "label" else BLUE, 16)
    for (a, _, _, _), (b, _, _, _) in zip(steps, steps[1:]):
        d.arrow(a, "bottom", b, "top", "yes" if a == "sig" else None)

    def side(eid, step, text, bg, dashed=False):
        y0, y1 = d.shapes[step][1], d.shapes[step][3]
        d.box(eid, 40, y0, 300, y1 - y0, text, bg, 16, dashed=dashed)
        d.arrow(eid, "right", step, "left", a_at=(y0 + y1) / 2, b_at=(y0 + y1) / 2)

    side("uf2", "sig", "release/rf-power-monitor-\n<version>-usb.uf2\n(fw_release.py --field)", GRAY)
    side("noflash", "read", "--no-flash: the board\nalready runs 3.5.0\nor newer", GRAY, dashed=True)
    side("ini", "upd", "secret/bench.ini (optional)\n[update] url, key;\nthe key is never printed", GRAY)
    side("reprint", "label", "--reprint <hostname, MAC\nor serial>: the label\nagain from devices.csv", GRAY,
         dashed=True)
    d.box("refuse", 980, 230, 300, 510, "Refused, with the reason:\n\nrelease not field-signed,\n"
                                        "no RPI-RP2 drive or\nno new port, two boards,\nfirmware older than 3.5.0,\n"
                                        "wrong key, version\nor hostname", RED, 16)
    d.arrow("sig", "right", "refuse", "left", "no", a_at=275, b_at=275)
    d.arrow("check", "right", "refuse", "left", "fails", a_at=695, b_at=695)
    d.note(980, 790, "devices.csv holds the default web\npasswords: bench/ is gitignored.\n"
                     "The label carries hostname, MAC,\nserial, firmware, password and\nthe board revision.")
    d.save()


def cores_watchdog():
    d = Diagram("cores-watchdog", "RF-Power-Monitor: the two cores and the watchdog")
    d.zone("z-c0", 40, 80, 760, 860, "Core 0: setup(), loop()", ZONE_BLUE)
    d.note(70, 130, "own 8 KB stack, shared with\nUSB, lwIP and W5500\ninterrupts (core 1 has its\n"
                    "own: core1_separate_stack)")
    d.box("loop0", 430, 130, 340, 390, "loop(), every pass:\nwatchdogPoll()\nfirmware trial\nmains changes, traps\n"
                                       "link, DHCPv6, LLDP\ntemperatures, load\nSNMP, web page\n"
                                       "update site, NTP, syslog\nserial menu\nfactory reset request", BLUE, 16)
    d.box("hb", 470, 600, 260, 160, "core 1 heartbeat\nmoved within\n1 s?", YELLOW, 16, shape="diamond")
    d.box("nofeed", 100, 640, 260, 80, "not fed: the\nwatchdog runs out", RED, 16)
    d.box("feed", 480, 840, 240, 70, "feed the watchdog", GREEN, 16)
    d.arrow("loop0", "bottom", "hb", "top", "each pass", a_at=600, b_at=600)
    d.arrow("hb", "bottom", "feed", "top", "yes")
    d.arrow("hb", "left", "nofeed", "right", "no")

    d.zone("z-shared", 860, 80, 280, 860, "Shared RAM", ZONE_GRAY)
    d.box("snap", 880, 260, 240, 80, "mains snapshot\n(seqlock)", YELLOW, 16)
    d.box("flag", 880, 380, 240, 80, "reset requested\n(atomic flag)", YELLOW, 16)
    d.box("beat", 880, 640, 240, 80, "heartbeat\ncounter", YELLOW, 16)

    d.zone("z-c1", 1200, 80, 420, 860, "Core 1: setup1(), loop1()", ZONE_ORANGE)
    d.box("loop1", 1230, 130, 360, 630, "loop1(), every 1 ms:\n\nmainsSample(): GPIO27,\ndebouncer (5 ms),\n"
                                        "seqlock snapshot\n\nfactoryResetSample():\nGPIO22, 10 s hold,\nLED blink\n\n"
                                        "watchdogCore1Beat():\nheartbeat + 1", ORANGE, 16)
    d.note(1230, 800, "own 8 KB stack")
    for eid in ("snap", "flag", "beat"):
        y = (d.shapes[eid][1] + d.shapes[eid][3]) / 2
        d.arrow("loop1", "left", eid, "right", a_at=y, b_at=y)
    for eid in ("snap", "flag"):
        y = (d.shapes[eid][1] + d.shapes[eid][3]) / 2
        d.arrow(eid, "left", "loop0", "right", a_at=y, b_at=y)
    d.arrow("beat", "left", "hb", "right", a_at=680, b_at=680)

    d.box("wd", 380, 1010, 440, 100, "RP2040 hardware watchdog, 8 s\n(the boot program arms ~8.3 s;\n"
                                     "setup() takes it over and feeds it)", GRAY, 16)
    d.box("restart", 1000, 1000, 620, 120, "Restart: counted in RAM (watchdog restarts since\npower-on, on the "
                                           "status screen); the boot\nprogram sees a crash: the 3rd in a row on one "
                                           "slot\nstarts the other slot", RED, 16)
    d.arrow("feed", "bottom", "wd", "top", a_at=600, b_at=600)
    d.arrow("wd", "right", "restart", "left", "8 s not fed", a_at=1060, b_at=1060)
    d.note(40, 1160, "A hang on either core restarts the device: a hung core 0 stops feeding; a hung core 1 stops "
                     "its heartbeat, so core 0 stops feeding.\nThe hidden menu commands stall0 and stall1 test both "
                     "(HIL test_watchdog).")
    d.save()


if __name__ == "__main__":
    system_overview()
    firmware_architecture()
    snmp_request_flow()
    mains_flow()
    test_layers()
    settings_storage()
    web_request_flow()
    network_addresses()
    update_site_flow()
    firmware_update_boot()
    time_syslog_flow()
    librenms_integration()
    bench_setup()
    cores_watchdog()
