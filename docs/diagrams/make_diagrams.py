"""Generate the project's Excalidraw diagrams (docs/diagrams/*.excalidraw).

The .excalidraw files can be opened and edited at https://excalidraw.com or
with the VS Code Excalidraw extension. Running this script again overwrites
them, so edit either here or in Excalidraw, not both. render_svg.sh exports
the SVGs used in the README and docs.

    python docs/diagrams/make_diagrams.py
"""

import hashlib
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))

# Excalidraw palette
BLUE, GREEN, YELLOW, RED, VIOLET = "#a5d8ff", "#b2f2bb", "#ffec99", "#ffc9c9", "#d0bfff"
ORANGE, CYAN, GRAY, WHITE = "#ffd8a8", "#99e9f2", "#e9ecef", "#ffffff"
INK, MUTED = "#1e1e1e", "#868e96"
ZONE_BLUE, ZONE_GREEN, ZONE_YELLOW, ZONE_RED = "#e7f5ff", "#ebfbee", "#fff9db", "#fff5f5"
ZONE_ORANGE, ZONE_GRAY = "#fff4e6", "#f8f9fa"


def _num(key):
    return int(hashlib.sha1(key.encode()).hexdigest()[:8], 16)


class Diagram:
    def __init__(self, name, title):
        self.name = name
        self.elements = []
        self.by_id = {}
        self.text(40, 20, title, size=32)

    # ---- element basics ----

    def _base(self, eid, etype, x, y, w, h, **kw):
        e = {
            "id": eid, "type": etype, "x": x, "y": y, "width": w, "height": h, "angle": 0,
            "strokeColor": kw.get("stroke", INK), "backgroundColor": kw.get("bg", "transparent"),
            "fillStyle": "solid", "strokeWidth": kw.get("stroke_width", 2),
            "strokeStyle": "dashed" if kw.get("dashed") else "solid", "roughness": 1, "opacity": 100,
            "groupIds": [], "frameId": None, "roundness": kw.get("roundness"),
            "seed": _num(self.name + eid), "version": 1, "versionNonce": _num(eid + self.name),
            "isDeleted": False, "boundElements": [], "updated": 1, "link": None, "locked": False,
        }
        self.elements.append(e)
        self.by_id[eid] = e
        return e

    def _text_el(self, eid, x, y, text, size, align, valign, container=None, color=INK):
        lines = text.split("\n")
        w = max(len(l) for l in lines) * size * 0.58
        h = len(lines) * size * 1.25
        if container is not None:
            cx, cy = container
            x, y = cx - w / 2, cy - h / 2
        e = self._base(eid, "text", x, y, w, h, stroke=color)
        e.update({"text": text, "originalText": text, "fontSize": size, "fontFamily": 1,
                  "textAlign": align, "verticalAlign": valign, "containerId": None,
                  "lineHeight": 1.25, "autoResize": True, "boundElements": None})
        return e

    def text(self, x, y, text, size=20, color=INK):
        return self._text_el("t%d" % len(self.elements), x, y, text, size, "left", "top", color=color)

    # ---- shapes ----

    def box(self, eid, x, y, w, h, text, bg=BLUE, size=20, shape="rectangle", dashed=False):
        e = self._base(eid, shape, x, y, w, h, bg=bg, dashed=dashed,
                       roundness={"type": 3} if shape == "rectangle" else {"type": 2})
        t = self._text_el(eid + "-text", 0, 0, text, size, "center", "middle", container=(x + w / 2, y + h / 2))
        t["containerId"] = eid
        e["boundElements"].append({"type": "text", "id": t["id"]})
        return e

    def zone(self, eid, x, y, w, h, title, bg=ZONE_GRAY, title_at="top", title_x=16):
        """Dashed group box; title_at "bottom" and title_x keep the title clear of arrows."""
        self._base(eid, "rectangle", x, y, w, h, bg=bg, stroke=MUTED, dashed=True, roundness={"type": 3})
        ty = y + 10 if title_at == "top" else y + h - 36
        self.text(x + title_x, ty, title, size=20, color="#495057")

    # ---- arrows ----

    def _anchor(self, eid, side, frac=0.5):
        e = self.by_id[eid]
        x, y, w, h = e["x"], e["y"], e["width"], e["height"]
        return {"top": (x + w * frac, y), "bottom": (x + w * frac, y + h),
                "left": (x, y + h * frac), "right": (x + w, y + h * frac)}[side]

    def arrow(self, a, a_side, b, b_side, label=None, via=(), a_frac=0.5, b_frac=0.5, both=False,
              dashed=False, size=16, color=INK):
        eid = "a-%s-%s-%d" % (a, b, len(self.elements))
        start = self._anchor(a, a_side, a_frac)
        end = self._anchor(b, b_side, b_frac)
        pts = [start] + list(via) + [end]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        e = self._base(eid, "arrow", start[0], start[1], max(xs) - min(xs), max(ys) - min(ys),
                       stroke=color, dashed=dashed, roundness=None if via else {"type": 2})
        e.update({"points": [[px - start[0], py - start[1]] for px, py in pts],
                  "lastCommittedPoint": None,
                  "startBinding": {"elementId": a, "focus": 0, "gap": 4},
                  "endBinding": {"elementId": b, "focus": 0, "gap": 4},
                  "startArrowhead": "arrow" if both else None, "endArrowhead": "arrow", "elbowed": False})
        for target in (a, b):
            self.by_id[target]["boundElements"].append({"type": "arrow", "id": eid})
        if label:
            # Label at the middle of the path length
            segs = list(zip(pts, pts[1:]))
            lengths = [math.dist(p, q) for p, q in segs]
            half = sum(lengths) / 2
            for (p, q), length in zip(segs, lengths):
                if half <= length:
                    t = half / length if length else 0
                    mid = (p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t)
                    break
                half -= length
            t = self._text_el(eid + "-label", 0, 0, label, size, "center", "middle", container=mid, color=color)
            t["containerId"] = eid
            e["boundElements"].append({"type": "text", "id": t["id"]})
        return e

    def line_arrow(self, start, end, color=MUTED):
        """An arrow not attached to any shape."""
        eid = "free-%d" % len(self.elements)
        e = self._base(eid, "arrow", start[0], start[1], abs(end[0] - start[0]), abs(end[1] - start[1]),
                       stroke=color, roundness={"type": 2})
        e.update({"points": [[0, 0], [end[0] - start[0], end[1] - start[1]]], "lastCommittedPoint": None,
                  "startBinding": None, "endBinding": None, "startArrowhead": None,
                  "endArrowhead": "arrow", "elbowed": False})
        return e

    def save(self):
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
    d.zone("z-mains", 40, 80, 330, 470, "Mains side (galvanically isolated)", ZONE_RED)
    d.box("mains", 75, 140, 260, 70, "Mains 230 V AC", RED)
    d.box("plug", 75, 260, 260, 90, "IKEA TRETAKT smart plug\n(Dirigera hub, tests only)", GRAY, 16, dashed=True)
    d.box("det", 75, 400, 260, 110, "Mains detector\noptocoupler + filter\nclean 3.3 V DC level", ORANGE, 18)
    d.arrow("mains", "bottom", "plug", "top")
    d.arrow("plug", "bottom", "det", "top")

    d.zone("z-board", 420, 80, 650, 640, "RF-Power-Monitor board", ZONE_BLUE)
    d.box("flash", 470, 140, 300, 90, "Internal flash (LittleFS)\nsettings_a / settings_b.json", GREEN, 16)
    d.box("pico", 470, 290, 300, 220,
          "Raspberry Pi Pico\n(RP2040)\n\nCore 0: network, SNMP, web page,\nLLDP, menu, CPU temperature,\n"
          "watchdog 8 s (fed while core 1 runs)\nCore 1: GPIO27 + GPIO22, 1 kHz",
          BLUE, 16)
    d.box("w5500", 880, 330, 170, 150, "W5500\nEthernet\nSPI0: GPIO16-19\nRST 20, INT 21", CYAN, 16)
    d.box("tmp", 880, 540, 170, 100, "TMP1075\nambient temp.\nI2C0, 0x48", ORANGE, 16)
    d.box("button", 470, 590, 140, 90, "Reset button\nGPIO22 to GND", GRAY, 16)
    d.box("led", 640, 590, 130, 90, "Onboard LED\nstatus, reset", YELLOW, 16)
    d.arrow("pico", "top", "flash", "bottom", both=True)
    d.arrow("pico", "right", "w5500", "left", "SPI", both=True)
    d.arrow("pico", "right", "tmp", "left", "GPIO4/5", a_frac=0.9, b_frac=0.5, both=True)
    d.arrow("button", "top", "pico", "bottom", a_frac=0.5, b_frac=0.23)
    d.arrow("pico", "bottom", "led", "top", a_frac=0.77)
    d.arrow("det", "right", "pico", "left", "GPIO27\nHIGH = mains", a_frac=0.4, b_frac=0.4)

    d.box("pc", 40, 610, 330, 100, "PC: PuTTY or HIL tests\nUSB serial, 115200 8N1", GRAY, 16)
    d.arrow("pc", "right", "pico", "left", "USB", b_frac=0.85, both=True)

    d.zone("z-net", 1150, 80, 380, 700, "Network", ZONE_GRAY)
    d.box("dhcp", 1190, 130, 140, 100, "DHCP server\nopt. 224 = trap\ndestination", GRAY, 16)
    d.box("browser", 1350, 140, 140, 80, "Browser\nweb page\nTCP 80", VIOLET, 16)
    d.box("switch", 1190, 360, 300, 90, "LAN switch\nIPv4 + IPv6, LLDP", GRAY, 18)
    d.box("lnms", 1190, 520, 300, 110, "SNMP manager\nLibreNMS / net-snmp\nUDP 161, v1/v2c, IPv4/IPv6", VIOLET, 16)
    d.box("traps", 1190, 660, 300, 100, "Trap receiver\nUDP 162 (v1 or v2c)\nfixed IP or from DHCP", VIOLET, 16)
    d.arrow("w5500", "right", "switch", "left", "100 Mbit/s\nLLDP", b_frac=0.5, both=True)
    d.arrow("switch", "top", "dhcp", "bottom", a_frac=0.23, both=True)
    d.arrow("switch", "top", "browser", "bottom", a_frac=0.77, both=True)
    d.arrow("switch", "bottom", "lnms", "top", both=True)
    d.arrow("switch", "right", "traps", "right", "traps", via=[(1515, 405), (1515, 710)])
    d.save()


def firmware_architecture():
    d = Diagram("firmware-architecture", "RF-Power-Monitor: firmware architecture")
    d.text(40, 64, "Arrows show the main dependencies. Everything in lib/ (green) is plain C++ and unit tested on the PC.",
           size=16, color="#495057")
    d.zone("z-src", 40, 100, 1650, 330, "src/  firmware on the Pico (Arduino)", ZONE_BLUE)
    cols = [70, 300, 530, 760, 990, 1220, 1450]
    src = [("main", "main.cpp\nboot, Ethernet,\nloop, starts core 1"), ("cli", "cli.cpp\nserial menu"),
           ("svc", "snmp_service.cpp\nUDP 161, traps,\ndevice state"), ("temp", "cpu_temp.cpp\nambient_temp.cpp\nRP2040, TMP1075"),
           ("net", "net_stats.cpp\nframe counters,\nW5500 PHY, hooks"), ("reset", "factory_reset.cpp\nGPIO22 on core 1"),
           ("web", "web_service.cpp\nweb page, TCP 80,\nnon-blocking")]
    for x, (eid, text) in zip(cols, src):
        d.box(eid, x, 160, 210, 110, text, BLUE, 16)
    # Second row: services added since 2.3.0, each over the library it runs
    src2 = [(70, "d6svc", "dhcp6_service.cpp\nDHCPv6 client,\nframes around lwIP"),
            (300, "lldpsvc", "lldp_service.cpp\nLLDP send / listen,\nLLDP-MIB data"),
            (760, "names", "hostname.cpp\nipv6_static.cpp\nDHCP/mDNS name, IPv6"),
            (1220, "wdog", "watchdog.cpp\n8 s watchdog,\ncore 1 heartbeat")]
    for x, eid, text in src2:
        d.box(eid, x, 290, 210, 110, text, BLUE, 16)

    dy = 130  # everything below the second src row moves down
    d.zone("z-pure", 40, 340 + dy, 1650, 330, "lib/  pure C++, host tested", ZONE_GREEN, title_at="bottom",
           title_x=760)
    pure = [("codec", "settings_codec\nJSON, validation,\nA/B selection"), ("netcfg", "netconfig\nIPv4/IPv6 parse +\nvalidate"),
            ("agent", "snmp_agent\nMIB, Get / GetNext /\nGetBulk, traps"), ("acl", "access_control\nrate limiter,\naccess log"),
            ("ifst", "if_stats\nframe classes,\nPHYCFGR decode"), ("power", "power_monitor\ndebouncer, mains\nalarm, reset button"),
            ("webui", "web_ui\nHTTP parser, login,\nsessions, pages")]
    for x, (eid, text) in zip(cols, pure):
        d.box(eid, x, 400 + dy, 210, 110, text, GREEN, 16)
    d.box("d6c", 100, 548 + dy, 180, 72, "dhcp6_client\nDHCPv6 messages,\nclient state", GREEN, 16)
    d.box("lldp", 300, 548 + dy, 210, 72, "lldp\nLLDPDU build/parse,\nneighbour, timing", GREEN, 16)
    d.box("ber", 530, 560 + dy, 210, 90, "snmp_ber\nBER codec, OIDs,\nCounter64", GREEN, 16)
    d.box("tmp1075", 760, 548 + dy, 200, 72, "tmp1075\nregister to 0.1 C,\nread state", GREEN, 16)
    d.box("dhcpopt", 990, 548 + dy, 210, 72, "dhcp_option\nask for / read\nDHCP option 224", GREEN, 16)
    d.box("wdl", 1220, 548 + dy, 210, 72, "watchdog_logic\nheartbeat check,\nrestart count", GREEN, 16)

    d.zone("z-hw", 40, 710 + dy, 1650, 170, "lib/  hardware layers", ZONE_YELLOW, title_at="bottom")
    d.box("settings", 70, 740 + dy, 210, 90, "settings\nLittleFS A/B slots", YELLOW, 16)
    d.box("mains", 1450, 735 + dy, 210, 100, "mains_input\nGPIO27 on core 1,\nseqlock", YELLOW, 16)

    d.zone("z-core", 40, 920 + dy, 1650, 150, "Arduino-Pico core (earlephilhower)", ZONE_GRAY)
    core = [(70, 330, "lwip", "lwIP + Wiznet5500lwIP\n(CountingW5500)"), (430, 200, "lfs", "LittleFS"),
            (660, 200, "spi", "SPI"), (890, 250, "usb", "USB serial"), (1170, 260, "mc", "multicore\nsetup1 / loop1"), (1460, 200, "tcp", "WiFiServer\n(TCP)")]
    for x, w, eid, text in core:
        d.box(eid, x, 980 + dy, w, 70, text, GRAY, 16)

    # Arrows from the first src row run down the gaps between the columns, past the second row
    def gap(x):
        return x + 220  # the gap right of the column at x
    d.arrow("cli", "bottom", "netcfg", "top", a_frac=0.95, b_frac=0.95, via=[(gap(300), 280), (gap(300), 520)])
    d.arrow("svc", "bottom", "agent", "top")
    d.arrow("svc", "bottom", "acl", "top", a_frac=0.8, b_frac=0.3, via=[(698, 450), (823, 450)])
    d.arrow("agent", "bottom", "ber", "top")
    d.arrow("net", "bottom", "ifst", "top")
    d.arrow("net", "bottom", "dhcpopt", "top", a_frac=0.95, b_frac=0.95, via=[(gap(990), 280), (gap(990), 660)])
    d.arrow("temp", "bottom", "tmp1075", "top", a_frac=0.05, b_frac=0.05, via=[(gap(530), 280), (gap(530), 660)])
    d.arrow("reset", "bottom", "power", "top", a_frac=0.05, b_frac=0.05, via=[(gap(990) + 10, 280), (gap(990) + 10, 520)])
    d.arrow("web", "bottom", "webui", "top")
    d.arrow("d6svc", "bottom", "d6c", "top", a_frac=0.95, b_frac=0.95, via=[(gap(70) - 10, 410), (gap(70) - 10, 668)])
    d.arrow("lldpsvc", "bottom", "lldp", "top", a_frac=0.95, b_frac=0.95, via=[(gap(300) - 10, 410), (gap(300) - 10, 668)])
    d.arrow("wdog", "bottom", "wdl", "top", a_frac=0.95, b_frac=0.95, via=[(gap(1220) - 10, 410), (gap(1220) - 10, 668)])
    d.arrow("settings", "top", "codec", "bottom", a_frac=0.05, b_frac=0.05)  # left of dhcp6_client
    d.arrow("mains", "top", "power", "bottom", b_frac=0.9, via=[(1555, 660), (1409, 660)])  # above watchdog_logic
    d.save()


def snmp_request_flow():
    d = Diagram("snmp-request-flow", "RF-Power-Monitor: how an SNMP request is handled")
    d.box("drop", 290, 80, 1080, 70,
          "Dropped without a reply: counted on the status screen, logged in the access log (menu 5)", RED, 18)
    d.box("udp", 40, 220, 200, 90, "UDP datagram\non port 161", CYAN, 18)
    d.box("size", 290, 205, 200, 120, "at most\n1472 bytes?", YELLOW, 18, shape="diamond")
    d.box("rate", 580, 200, 230, 130, "Rate limiter\nper source 50/s\nuntrusted total 100/s\ntrusted total 200/s",
          ORANGE, 16)
    d.box("parse", 910, 210, 230, 110, "Parse BER message\nversion, community,\nPDU, max 16 varbinds", BLUE, 16)
    d.box("ver", 1190, 205, 180, 120, "version?", YELLOW, 18, shape="diamond")
    d.arrow("udp", "right", "size", "left")
    d.arrow("size", "right", "rate", "left", "yes")
    d.arrow("rate", "right", "parse", "left", "allowed")
    d.arrow("parse", "right", "ver", "left")
    d.arrow("size", "top", "drop", "bottom", "no", b_frac=0.1)
    d.arrow("rate", "top", "drop", "bottom", "over limit", b_frac=0.36)
    d.arrow("parse", "top", "drop", "bottom", "malformed", b_frac=0.66)
    d.arrow("ver", "top", "drop", "bottom", "v3", b_frac=0.92)

    d.box("comm", 1190, 470, 180, 130, "community\ncorrect?", YELLOW, 18, shape="diamond")
    d.box("pdu", 840, 480, 260, 110, "Handle the PDU\nGet, GetNext, GetBulk (v2c)\nSet is rejected", BLUE, 16)
    d.box("mib", 520, 480, 260, 110, "MIB table\nLLDP-MIB, system,\nIF-MIB, 63050: up to\n67 objects (59 in v1)", GREEN, 16)
    d.box("build", 180, 480, 280, 110, "Build GetResponse\nv1 errors / v2c exceptions\nGetBulk at most 16 varbinds",
          BLUE, 16)
    d.box("sent", 180, 680, 280, 90, "Reply sent\nsource now trusted\n(not for GetBulk)", GREEN, 16)
    d.box("auth", 1165, 680, 230, 100, "authenticationFailure\ntrap (max 1 per 10 s)\n+ access log", RED, 16)
    d.arrow("ver", "bottom", "comm", "top", "v1 / v2c")
    d.arrow("comm", "left", "pdu", "right", "yes")
    d.arrow("comm", "bottom", "auth", "top", "no")
    d.arrow("pdu", "left", "mib", "right")
    d.arrow("mib", "left", "build", "right")
    d.arrow("build", "bottom", "sent", "top")
    d.save()


def mains_flow():
    d = Diagram("mains-flow", "RF-Power-Monitor: from the mains input to an alert")
    d.zone("z-c1", 40, 80, 930, 570, "Core 1: setup1() / loop1(), every millisecond", ZONE_ORANGE)
    d.box("gpio", 70, 170, 190, 90, "GPIO27\nmains detector", ORANGE, 18)
    d.box("sample", 300, 170, 190, 90, "Sample the pin\nevery 1 ms", ORANGE, 16)
    d.box("deb", 530, 170, 190, 90, "Debouncer\nlevel must hold\n5 ms", ORANGE, 16)
    d.box("seq", 760, 160, 190, 110, "Seqlock snapshot\npresent, changes,\nlast change", GREEN, 16)
    d.arrow("gpio", "right", "sample", "left")
    d.arrow("sample", "right", "deb", "left")
    d.arrow("deb", "right", "seq", "left")
    d.text(300, 290, "a real change is seen within 6 ms; glitches shorter than 5 ms are ignored", size=16,
           color="#495057")

    d.box("btn", 70, 400, 190, 90, "GPIO22\nreset button", GRAY, 18)
    d.box("rb", 300, 390, 190, 110, "ResetButton\n10 s hold, release\nbefore cancels", GRAY, 16)
    d.box("flag", 530, 400, 190, 90, "Reset requested\n(atomic flag)", GRAY, 16)
    d.box("led", 300, 545, 190, 80, "LED\nslow / fast blink", YELLOW, 16)
    d.arrow("btn", "right", "rb", "left")
    d.arrow("rb", "right", "flag", "left")
    d.arrow("rb", "bottom", "led", "top")

    d.zone("z-c0", 1010, 80, 480, 570, "Core 0: loop()", ZONE_BLUE)
    d.box("read", 1040, 160, 200, 110, "Read snapshot\n(retries while\ncore 1 writes)", BLUE, 16)
    d.box("log", 1280, 130, 190, 80, "Serial log +\nstatus screen", BLUE, 16)
    d.box("obj", 1280, 225, 190, 120, "SNMP objects\n63050.1 / .2 / .3\n.6 alarm: lost\nfor 60 s", BLUE, 16)
    d.box("trap", 1280, 360, 190, 120, "Traps\nmainsPowerLost,\nRestored, and\nLostTooLong (60 s)", VIOLET, 16)
    d.box("fr", 1040, 400, 200, 110, "Factory reset\nsave defaults,\nreboot within 2 s", RED, 16)
    d.arrow("seq", "right", "read", "left")
    d.arrow("read", "right", "log", "left", a_frac=0.3)
    d.arrow("read", "right", "obj", "left", a_frac=0.6)
    d.arrow("read", "right", "trap", "left", a_frac=0.9, b_frac=0.3)
    d.arrow("flag", "right", "fr", "left")

    d.zone("z-net", 1580, 80, 360, 570, "Network", ZONE_GRAY)
    d.box("lnms", 1610, 180, 300, 150, "LibreNMS\npolls every 5 min\n\"Mains lost over 60 s\"\ncritical: alert\n(lost alone: warning)",
          VIOLET, 16)
    d.box("rx", 1610, 380, 300, 100, "Trap receiver /\nLibreNMS event log;\na mains trap starts a poll", VIOLET, 16)
    d.arrow("obj", "right", "lnms", "left", "UDP 161")
    d.arrow("trap", "right", "rx", "left", "UDP 162")
    d.text(1010, 680, "A short outage between two polls still shows: mainsPowerChanges goes up by 2.\n"
                      "Only an outage of 60 s or more raises the alarm and the alert.", size=16, color="#495057")
    d.save()


def test_layers():
    d = Diagram("test-layers", "RF-Power-Monitor: test layers")
    layers = [
        ("lnms", "LibreNMS in Docker: definitions, sensors, LLDP neighbour, alert on a real mains cut", VIOLET),
        ("mains", "Mains HIL: a Dirigera smart plug switches real mains", RED),
        ("hil", "HIL (Python): menu, SNMP, traps, web, IPv6, DHCP opt. 224,\nLLDP (switch console), watchdog, reboot",
         ORANGE),
        ("emb", "On-device Unity tests: LittleFS, W5500, GPIO27 sampler", YELLOW),
        ("host", "Host agent vs real net-snmp (WSL): 65 checks incl. LLDP-MIB, MIB validation", CYAN),
        ("san", "Sanitizers (WSL): every native suite under ASan + UBSan", CYAN),
        ("native", "Native unit tests: about 600 tests of lib/, frames from the device,\nKea, Junos and net-snmp",
         GREEN),
    ]
    for i, (eid, text, bg) in enumerate(layers):
        d.box(eid, 120, 110 + i * 100, 820, 80, text, bg, 18)
    d.line_arrow((65, 790), (65, 110))
    d.text(10, 800, "fast, runs everywhere", size=16, color="#495057")
    d.text(10, 84, "slower, real hardware", size=16, color="#495057")

    d.box("pc", 1010, 110, 330, 380, "Your PC\nPico on USB (RFPOWER_PORT)\nWSL: net-snmp, Docker\nDirigera hub\nswitch console\n\n"
          "run locally", GRAY, 18)
    d.box("ci", 1010, 510, 330, 280, "GitHub Actions CI\non every push\n\nnative tests\nsanitizers\nnet-snmp check\nfirmware build (.uf2)",
          GREEN, 18)
    for eid in ("lnms", "mains", "hil", "emb"):
        d.arrow(eid, "right", "pc", "left", b_frac={"lnms": 0.12, "mains": 0.38, "hil": 0.64, "emb": 0.9}[eid])
    for eid in ("host", "san", "native"):
        d.arrow(eid, "right", "ci", "left", b_frac={"host": 0.18, "san": 0.5, "native": 0.82}[eid])
    d.save()


def settings_storage():
    d = Diagram("settings-storage", "RF-Power-Monitor: power-cut safe settings (A/B slots)")
    d.text(40, 90, "Saving", size=24)
    d.box("new", 40, 140, 220, 90, "New settings\n(serial menu or web page)", BLUE, 16)
    d.box("codec", 310, 140, 220, 90, "settings_codec\nto JSON", GREEN, 16)
    d.box("write", 580, 135, 230, 100, "Write the slot\nnot in use,\nsequence + 1", YELLOW, 16)
    d.box("verify", 860, 140, 220, 90, "Read back\nand validate", YELLOW, 16)
    d.box("active", 1130, 140, 220, 90, "New copy is\nthe active one", GREEN, 16)
    d.arrow("new", "right", "codec", "left")
    d.arrow("codec", "right", "write", "left")
    d.arrow("write", "right", "verify", "left")
    d.arrow("verify", "right", "active", "left")

    d.zone("z-flash", 380, 290, 660, 170, "Internal flash, LittleFS", ZONE_YELLOW)
    d.box("a", 420, 340, 260, 90, "settings_a.json\nsequence 7", GREEN, 18)
    d.box("b", 740, 340, 260, 90, "settings_b.json\nsequence 8 (newest)", GREEN, 18)
    d.arrow("write", "bottom", "b", "top", "writes", b_frac=0.3)

    d.text(40, 490, "Booting", size=24)
    d.box("boot", 600, 540, 220, 90, "Read both slots", BLUE, 16)
    d.box("valid", 900, 525, 200, 120, "valid\ncopies?", YELLOW, 18, shape="diamond")
    d.box("use", 1200, 540, 300, 90, "Use the valid copy with\nthe highest sequence", GREEN, 16)
    d.box("defaults", 890, 720, 220, 90, "None valid:\nsave factory defaults", RED, 16)
    d.arrow("a", "bottom", "boot", "top", b_frac=0.25)
    d.arrow("b", "bottom", "boot", "top", b_frac=0.75)
    d.arrow("boot", "right", "valid", "left")
    d.arrow("valid", "right", "use", "left", "one or two")
    d.arrow("valid", "bottom", "defaults", "top", "none")
    d.text(40, 560, "Power cut during a save:\nthe half-written slot fails\nvalidation, so the other slot,\n"
                     "the previous settings, is used.", size=16, color="#495057")
    d.save()


def web_request_flow():
    d = Diagram("web-request-flow", "RF-Power-Monitor: how a web page request is handled")
    d.box("conn", 40, 300, 200, 90, "Browser connects\nTCP port 80", CYAN, 18)
    d.box("read", 290, 285, 220, 120, "Read the request\nnon-blocking,\nat most 2 KB, 5 s", BLUE, 16)
    d.box("closed", 290, 80, 220, 100, "Closed, or\n400 / 413", RED, 18)
    d.box("path", 560, 280, 180, 130, "path?", YELLOW, 18, shape="diamond")
    d.box("static", 560, 80, 260, 100, "Logo and stylesheet\n(gzip, from flash)\nsent without login", GREEN, 16)
    d.arrow("conn", "right", "read", "left")
    d.arrow("read", "top", "closed", "bottom", "too slow, malformed\nor too large")
    d.arrow("read", "right", "path", "left")
    d.arrow("path", "top", "static", "bottom", "/favicon.svg\n/static/...", b_frac=0.35)

    # GET /
    d.box("getdia", 900, 190, 200, 120, "session\ncookie valid?", YELLOW, 16, shape="diamond")
    d.box("login", 900, 50, 200, 80, "Login page", BLUE, 18)
    d.box("page", 1170, 205, 300, 90, "Status and settings page\nforms carry the CSRF token", GREEN, 16)
    d.arrow("path", "right", "getdia", "left", "GET /", a_frac=0.2, via=[(820, 306), (820, 250)])
    d.arrow("getdia", "top", "login", "bottom", "no")
    d.arrow("getdia", "right", "page", "left", "yes")

    # POST /login
    d.box("check", 870, 375, 300, 130, "Check the password\ndefault: constant-time compare\nown: salted SHA-256, 2000\nrounds in steps, SNMP answers",
          BLUE, 16)
    d.box("lock", 1250, 380, 200, 120, "login\nlocked?", YELLOW, 18, shape="diamond")
    d.box("locked", 1250, 560, 200, 90, "429\naccess log", RED, 16)
    d.box("ok", 1640, 380, 180, 120, "right?", YELLOW, 18, shape="diamond")
    d.box("session", 1630, 190, 300, 120, "New session: cookie sid\nHttpOnly, SameSite=Strict\nmax 2, ends after 15 min idle",
          GREEN, 16)
    d.box("wrong", 1620, 560, 220, 90, "401, access log\n5 in a row: 30 s lock", RED, 16)
    d.arrow("path", "right", "check", "left", "POST /login", a_frac=0.8, via=[(820, 384), (820, 440)])
    d.arrow("check", "right", "lock", "left")
    d.arrow("lock", "bottom", "locked", "top", "yes")
    d.arrow("lock", "right", "ok", "left", "no")
    d.arrow("ok", "top", "session", "bottom", "yes", b_frac=0.35)
    d.arrow("ok", "bottom", "wrong", "top", "no")
    d.arrow("session", "left", "page", "right", "redirect\nto /")

    # Actions
    d.box("post", 900, 740, 240, 110, "POST /network /ipv6\n/hostname /community\n/reboot /disable /logout", BLUE, 16)
    d.box("csrf", 1200, 730, 200, 130, "session and\nCSRF token\nvalid?", YELLOW, 16, shape="diamond")
    d.box("forbid", 1200, 920, 200, 90, "403: Not logged in /\nForm expired", RED, 16)
    d.box("valid", 1470, 735, 180, 120, "fields\nvalid?", YELLOW, 18, shape="diamond")
    d.box("bad", 1460, 920, 200, 90, "400: reason on\nthe page, nothing saved", RED, 16)
    d.box("save", 1720, 730, 300, 130, "community, hostname: saved,\nactive now; network, IPv6:\nsaved, restart; reboot: restart;\noff: saved, port 80 closed;\nlogout: session ends\n(500 if saving fails)", GREEN, 16)
    d.arrow("path", "bottom", "post", "left", "POST action", via=[(650, 795)])
    d.arrow("post", "right", "csrf", "left")
    d.arrow("csrf", "bottom", "forbid", "top", "no")
    d.arrow("csrf", "right", "valid", "left", "yes")
    d.arrow("valid", "bottom", "bad", "top", "no")
    d.arrow("valid", "right", "save", "left", "yes")
    d.text(40, 560, "One connection at a time, never\nblocking: SNMP keeps answering\nin between.\n\n"
                    "Plain HTTP: use the page on a\ntrusted management network.", size=16, color="#495057")
    d.save()


def network_addresses():
    d = Diagram("network-addresses", "RF-Power-Monitor: where the addresses come from (dual stack)")
    d.zone("z-v4", 20, 80, 1180, 380, "IPv4", ZONE_BLUE)
    d.box("dhcpsrv", 40, 180, 300, 90, "DHCP server\non the LAN", GRAY, 18)
    d.box("dhcp", 400, 160, 320, 130, "DHCP (default)\naddress, mask, gateway, DNS;\nsends the hostname (option 12)",
          BLUE, 16)
    d.box("static4", 400, 330, 320, 100, "Static IPv4\nserial menu 3 or web page\n(restart)", BLUE, 16)
    d.box("trap", 860, 120, 300, 100, "Trap destination\nfixed (menu 4, IPv4 or\nIPv6) or DHCP option 224", ORANGE, 16)
    d.box("ip4", 860, 270, 300, 100, "IPv4 address\nDHCP or static", GREEN, 18)
    d.arrow("dhcpsrv", "right", "dhcp", "left")
    d.arrow("dhcp", "right", "trap", "left", "option 224", a_frac=0.25)
    d.arrow("dhcp", "right", "ip4", "left", a_frac=0.75, b_frac=0.3)
    d.arrow("static4", "right", "ip4", "left", "or", b_frac=0.75)

    d.zone("z-v6", 20, 490, 1180, 460, "IPv6", ZONE_GREEN)
    d.box("ra", 40, 660, 300, 100, "IPv6 router\nrouter advertisements", GRAY, 18)
    d.box("ll", 400, 540, 320, 90, "Link-local fe80::\nfrom the MAC (EUI-64), always", BLUE, 16)
    d.box("slaac", 400, 650, 320, 120, "SLAAC: one address per\nadvertised /64; DHCPv6: one\naddress when the router\nsets the M flag", BLUE, 16)
    d.box("static6", 400, 790, 320, 100, "Static IPv6 (/64)\nserial menu 9 or web page\n(restart)", BLUE, 16)
    d.box("ip6", 860, 610, 300, 120, "IPv6 addresses\nup to 4, each passes\nduplicate address detection",
          GREEN, 16)
    d.box("gw", 860, 790, 300, 110, "IPv6 gateway\nthe static one if set,\nelse advertised routers",
          YELLOW, 16)
    d.arrow("ra", "right", "slaac", "left")
    d.arrow("ll", "right", "ip6", "left", b_frac=0.2)
    d.arrow("slaac", "right", "ip6", "left")
    d.arrow("static6", "right", "ip6", "left", "next to SLAAC", a_frac=0.3, b_frac=0.8)
    d.arrow("static6", "right", "gw", "left", "gateway", a_frac=0.75, b_frac=0.6)
    d.arrow("ra", "bottom", "gw", "bottom", "routers", via=[(190, 930), (1010, 930)])

    d.box("traps", 1260, 120, 320, 100, "Traps (UDP 162)\nto IPv4, or v2c to IPv6", ORANGE, 18)
    d.box("svc", 1260, 270, 320, 480, "Services on every address\n\nSNMP, UDP 161\n\nweb page, TCP 80\n\n"
          "mDNS name.local\n(A and AAAA)\n\nping", VIOLET, 18)
    d.arrow("trap", "right", "traps", "left")
    d.arrow("ip4", "right", "svc", "left", b_frac=0.08)
    d.arrow("ip6", "right", "svc", "left", b_frac=0.82)
    d.text(1260, 790, "IPv4 and IPv6 run side by side.\nRate limits and the access log\nwork per full source address.",
           size=16, color="#495057")
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
