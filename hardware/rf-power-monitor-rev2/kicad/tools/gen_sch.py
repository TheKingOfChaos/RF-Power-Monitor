#!/usr/bin/env python3
"""Generate the rev2 schematic (and the project symbol library) from design/netlist.json.

Rules: pins of one net that face each other, or sit next to each other on one part, are joined by a
wire; every other pin end gets its net as a power symbol (rails) or a local label (signals); unused
pins get a no-connect flag. Parts sit in framed functional blocks laid out by LAYOUT below (A2,
sheet mm, y down). Run: python3 kicad/tools/gen_sch.py
"""
import json
import math
import os
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import symlib  # noqa: E402
from sexp import q  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
KDIR = os.path.dirname(HERE)
ROOT = os.path.dirname(KDIR)
NAME = "rf-power-monitor-rev2"
LIB = "RFPM_Rev2"
PCM = "PCM_rf-power-monitor-rev2"     # the board's KiCad PCM library (wiki parts + project-library parts)
PCM_SYM = os.path.join(KDIR, "lib", "pcm", "symbols", "rf-power-monitor-rev2.kicad_sym")


def lib_id(key):
    return f"{PCM}:{key[5:]}" if key.startswith("PCM::") else f"{LIB}:{key}"


def pcm_parts(nl):
    """BOM parts -> (symbol key, footprint id) from the PCM library, matched by MPN"""
    import re as _re
    from sexp import parse, findall, uq
    lib = parse(open(PCM_SYM, encoding="utf-8").read())
    nodes = {uq(s[1]): s for s in findall(lib, "symbol")}
    out = {}
    for c in nl["components"]:
        if c.get("exclude_from_bom") or not c.get("mpn"):
            continue
        m = c["mpn"]
        name = next((n for n in (m, m.lower(), _re.sub(r"[^A-Za-z0-9_.+-]", "_", m)) if n in nodes), None)
        if not name:
            sys.exit(f"{c['ref']}: {m} not in {PCM_SYM}")
        fp = next(uq(p[2]) for p in findall(nodes[name], "property") if uq(p[1]) == "Footprint")
        out[c["ref"]] = ("PCM::" + name, f"{PCM}:{fp.split(':')[-1]}", nodes[name])
    return out
NS = uuid.UUID("6f1c9a52-3d2e-4b8e-9a51-0b7e2c1d4a10")
POWER = {"GND", "+3V3", "VSYS", "VBUS", "3V3A", "+1V1", "CHASSIS"}
GNDLIKE = {"GND", "CHASSIS"}
STUB = 2.54


def U(*k):
    return str(uuid.uuid5(NS, "/".join(str(x) for x in k)))


def f(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def snap(v, g=1.27):
    return round(round(v / g) * g, 4)


# ---------------------------------------------------------------------------------------------
# Blocks (title, x0, y0, x1, y1, note) and part positions (ref -> x, y, rot), A2 sheet
# ---------------------------------------------------------------------------------------------
BLOCKS = [
    ("USB-C, VSYS and 3V3 buck", 10, 10, 215, 135,
     "USB VBUS and the PoE module's 5 V meet on VSYS; D1 keeps PoE out of USB.\n"
     "TLV62569: VOUT = 0.6 V x (1 + R5/R6) = 3.33 V; 2.2 uH + 22 uF (TI table 4)."),
    ("PoE: RJ45, magnetics, WIZPoE-P1 (isolated 802.3af)", 220, 10, 405, 135,
     "Mode A: T1 line centre taps -> VC1. Mode B: spare pairs 4/5, 7/8 -> VC2; U2 rectifies both.\n"
     "Bob Smith: 75R + 10 nF/100 V per pair, 1 nF/2 kV to chassis. W5500 has no auto-MDIX: MDI.\n"
     "MDI side (J2, T1 pins 9-16, POE_*, BS*) keeps 2 mm from logic copper."),
    ("W5500 Ethernet (SPI0: GP16-GP21)", 410, 10, 585, 215,
     "PMODE 111: auto-negotiation. Pin 23 RSVD to GND; TOCAP 4.7 uF; 1V2O 10 nF (datasheet v1.1.0).\n"
     "TX: 3.3R series, 49.9R to the 3V3A centre tap. RX: 6.8 nF coupling, 49.9R to C25."),
    ("RP2040, flash, clock, USB, buttons, SWD", 10, 140, 260, 330,
     "Raspberry Pi Pico core: 12 MHz crystal, W25Q16 2 MB flash, BOOTSEL on QSPI_SS.\n"
     "Pin map unchanged (firmware board = pico): GP4/5 I2C0, GP16-21 W5500, GP22 factory reset,\n"
     "GP24 VBUS sense, GP25 status LED, GP27 MAINS_OK."),
    ("Ambient temperature, factory reset", 265, 140, 405, 215,
     "TMP1075 at 0x48 (A0-A2 to GND), placed away from U1, U3, U4.\nSW3: hold 10 s for factory reset."),
    ("Mains sense, 230 VAC (rev1 circuit, unchanged)", 265, 220, 585, 330,
     "DANGER 230 VAC: J4, F1, R33-R36, U6 inputs: 6.5 mm clearance, 6.4 mm creepage to everything else.\n"
     "U6 pulls SENSE low on both half cycles; C43/R37 bridge the zero crossings; U7 gives MAINS_OK (HIGH = mains)."),
    ("Test pads, power flags, mounting holes", 10, 335, 400, 410,
     "Test pads 0.5 mm, from design/tests.json (aiflow test plan)."),
]

L = {}


def put(ref, x, y, rot=0):
    L[ref] = (snap(x), snap(y), rot)


# power
put("J1", 30, 65)
put("R1", 70, 100)
put("R2", 80, 100)
put("D1", 75, 30, 180)
put("R3", 100, 55)
put("R4", 100, 75, 270)
put("C1", 115, 60)
put("C2", 125, 60)
put("C3", 138, 60)
put("U1", 160, 60)
put("L1", 185, 45, 90)
put("C4", 205, 65)
put("R5", 180, 90)
put("R6", 180, 105)
# PoE
put("J2", 250, 60)
put("T1", 305, 60)
put("U2", 375, 35)
for i, r in enumerate(("R7", "R8", "R9", "R10")):
    put(r, 335, 85 + i * 9, 90)
for i, c in enumerate(("C5", "C6", "C7", "C8")):
    put(c, 350, 85 + i * 9, 90)
put("C9", 385, 105)
put("C10", 255, 112, 90)
# W5500
put("U3", 470, 85)
put("FB1", 430, 30)
for i, c in enumerate(("C11", "C12", "C13", "C14", "C15", "C16", "C17", "C26", "C18")):
    put(c, 420 + i * 8, 145)
put("C19", 420, 170)
put("C20", 440, 170)
put("R11", 460, 170)
put("Y1", 540, 70)
put("R12", 540, 55, 90)
put("R13", 560, 95, 90)
put("C21", 522, 95)
put("C22", 578, 108)
for i, r in enumerate(("R14", "R15", "R16")):
    put(r, 532, 140 + i * 10)
put("R25", 570, 145)
put("R26", 580, 145)
put("R17", 425, 185, 90)
put("R18", 425, 197, 90)
put("R19", 445, 192, 180)
put("R20", 455, 192, 180)
put("C23", 485, 185, 90)
put("R21", 500, 185, 90)
put("C24", 485, 200, 90)
put("R22", 500, 200, 90)
put("R23", 525, 190, 180)
put("R24", 535, 190, 180)
put("C25", 560, 185)
# RP2040
put("U4", 70, 230)
put("U5", 150, 175)
put("C42", 165, 165)
put("Y2", 150, 222)
put("R29", 150, 208, 90)
put("C40", 140, 235)
put("C41", 160, 235)
put("R27", 140, 255, 90)
put("R28", 140, 268, 90)
put("R30", 190, 165, 90)
put("SW1", 210, 165)
put("R31", 190, 185, 90)
put("SW2", 210, 185)
put("J3", 215, 215)
put("R32", 185, 245, 90)
put("D2", 212, 245, 180)
for i, c in enumerate(("C27", "C28", "C29", "C30", "C31", "C32", "C33", "C34", "C35", "C39")):
    put(c, 20 + i * 9, 305)
for i, c in enumerate(("C36", "C37", "C38")):
    put(c, 125 + i * 9, 305)
# sensor
put("U8", 295, 180)
put("C45", 325, 160)
put("R40", 357, 158)
put("R41", 357, 170)
put("SW3", 390, 185, 270)
# mains
put("J4", 285, 270, 180)
put("F1", 310, 270, 270)
put("R33", 330, 270, 270)
put("R34", 305, 290, 270)
put("R35", 325, 290, 270)
put("R36", 345, 290, 270)
put("U6", 385, 280)
put("C43", 420, 290)
put("R37", 430, 255, 180)
put("U7", 470, 270)
put("C44", 495, 240)
put("R38", 530, 250)
put("D3", 530, 265)
put("D4", 530, 292)
put("R39", 575, 292, 270)
# test pads, holes, flags
for i in range(14):
    put(f"TP{i + 1}", 20 + i * 26, 352)
for i in range(4):
    put(f"H{i + 1}", 20 + i * 15, 395)
FLAGS = [("GND", 110, 398), ("+3V3", 130, 395), ("3V3A", 150, 395), ("VBUS", 170, 395), ("CHASSIS", 190, 398)]


# ---------------------------------------------------------------------------------------------
def xform(px, py, X, Y, rot):
    vx, vy = px, -py
    a = math.radians(rot)
    rx = vx * math.cos(a) + vy * math.sin(a)
    ry = -vx * math.sin(a) + vy * math.cos(a)
    return round(X + rx, 4), round(Y + ry, 4)


def outward(ang, rot):
    oa = math.radians(ang + 180 + rot)
    return round(math.cos(oa)), -round(math.sin(oa))


def font(s=1.27, extra="", bold=False):
    return f"(effects (font (size {s} {s}){' (bold yes)' if bold else ''}){extra})"


class Sheet:
    def __init__(self, syms, root):
        self.syms = syms
        self.pins = {n: symlib.pins_of(s) for n, s in syms.items()}
        self.root = root
        self.items = []
        self.npwr = 0
        self.used = set()

    def symbol(self, lib_name, ref, value, x, y, rot, fp="", props=None, in_bom=True, on_board=True, uid=None,
               hide_ref=False):
        uid = uid or U("sym", ref)
        self.used.add(lib_name)
        pdefs = self.pins[lib_name]
        rr = rot % 360
        trot = 90 if rr in (90, 270) else 0
        if lib_name in POWER or lib_name == "PWR_FLAG":
            ro, vo, jr, jv = (0, 0), (0, 0), "", ""
            trot = 0
        else:
            pts = [xform(p[0], p[1], 0, 0, rr) for p in pdefs.values()] or [(0, 0)]
            minx, maxx = min(a for a, _ in pts), max(a for a, _ in pts)
            miny, maxy = min(b for _, b in pts), max(b for _, b in pts)
            if maxx - minx < 1 and maxy - miny > 1:      # vertical two-terminal: text right
                j = " (justify right)" if rr == 180 else " (justify left)"   # KiCad mirrors justification at 180
                ro, vo, jr, jv = (2.54, -1.27), (2.54, 1.27), j, j
            elif maxy - miny < 1 and len(pdefs) <= 3:     # horizontal two-terminal: text above / below
                ro, vo, jr, jv = (0, -2.54), (0, 2.54), "", ""
            else:
                ro, vo, jr, jv = (0, miny - 6.35), (0, maxy + 6.35), "", ""
        ps = [f'(property "Reference" {q(ref)} (at {f(x + ro[0])} {f(y + ro[1])} {trot}){" (hide yes)" if hide_ref else ""} '
              f'{font(1.27, jr)})',
              f'(property "Value" {q(value)} (at {f(x + vo[0])} {f(y + vo[1])} {trot}) {font(1.27, jv)})',
              f'(property "Footprint" {q(fp)} (at {f(x)} {f(y)} 0) (hide yes) {font()})',
              f'(property "Datasheet" "" (at {f(x)} {f(y)} 0) (hide yes) {font()})']
        for k, v in (props or {}).items():
            ps.append(f'(property {q(k)} {q(v)} (at {f(x)} {f(y)} 0) (hide yes) {font()})')
        pin_s = " ".join(f'(pin {q(n)} (uuid "{U("pin", uid, n)}"))' for n in pdefs)
        self.items.append(
            f'(symbol (lib_id "{lib_id(lib_name)}") (at {f(x)} {f(y)} {rr}) (unit 1) (body_style 1) (exclude_from_sim no) '
            f'(in_bom {"yes" if in_bom else "no"}) (on_board {"yes" if on_board else "no"}) '
            f'(in_pos_files {"yes" if on_board else "no"}) (dnp no) (uuid "{uid}") '
            + " ".join(ps) + " " + pin_s +
            f' (instances (project "{NAME}" (path "/{self.root}" (reference "{ref}") (unit 1)))))')

    def wire(self, a, b, key):
        self.items.append(f'(wire (pts (xy {f(a[0])} {f(a[1])}) (xy {f(b[0])} {f(b[1])})) '
                          f'(stroke (width 0) (type default)) (uuid "{U("w", *key)}"))')

    def power(self, net, x, y, body_dir, key):
        """power symbol with its pin at (x, y) and its body pointing along body_dir"""
        if net in GNDLIKE:
            rot = {(0, 1): 0, (0, -1): 180, (1, 0): 90, (-1, 0): 270}[body_dir]
        else:
            rot = {(0, -1): 0, (0, 1): 180, (1, 0): 270, (-1, 0): 90}[body_dir]
        self.npwr += 1
        self.symbol(net, f"#PWR{self.npwr:03d}", net, x, y, rot, in_bom=False, on_board=False,
                    uid=U("pwr", *key), hide_ref=True)

    def label(self, net, x, y, ang, key):
        just = "left" if ang in (0, 90) else "right"
        self.items.append(f'(label {q(net)} (at {f(x)} {f(y)} {ang}) (fields_autoplaced yes) '
                          f'{font(1.27, f" (justify {just} bottom)")} (uuid "{U("lbl", *key)}"))')

    def marker(self, net, x, y, d, key, multi=False):
        """net marker at a pin end facing d (outward)"""
        if net in POWER:
            want = (0, 1) if net in GNDLIKE else (0, -1)
            if d == want or (multi and d[1] == 0):
                self.power(net, x, y, d, key)
            else:   # stub out, then an upright symbol
                e = (snap(x + d[0] * STUB), snap(y + d[1] * STUB))
                self.wire((x, y), e, key + ("stub",))
                self.power(net, e[0], e[1], want if d[1] == 0 else d, key)
        else:
            if d[1] == 0:
                self.label(net, x, y, 0 if d[0] > 0 else 180, key)
            else:
                e = (snap(x), snap(y + d[1] * STUB))
                self.wire((x, y), e, key + ("stub",))
                self.label(net, e[0], e[1], 0, key)


def embedded_all(syms):
    from sexp import parse, dump, q as _q
    out = []
    for key, s in syms.items():
        c = parse(dump(s))
        c[1] = _q(lib_id(key))
        out.append(dump(c))
    return "(lib_symbols " + " ".join(out) + ")"


def main():
    nl = json.load(open(os.path.join(ROOT, "design", "netlist.json")))
    syms = symlib.build(nl)
    os.makedirs(os.path.join(KDIR, "lib"), exist_ok=True)
    open(os.path.join(KDIR, "lib", f"{LIB}.kicad_sym"), "w").write(symlib.lib_file(syms))
    pcm = pcm_parts(nl)
    proj_syms = syms
    syms = dict(syms)
    for key, fpid, node in pcm.values():
        syms[key] = node
    root = U("root")
    sh = Sheet(syms, root)
    missing = [c["ref"] for c in nl["components"] if c["ref"] not in L]
    if missing:
        sys.exit(f"no layout for {missing}")

    # 1. place parts, collect pin ends
    P = []          # dicts: ref, num, x, y, d, net
    at_pt = {}
    for c in nl["components"]:
        lib_name = pcm[c["ref"]][0] if c["ref"] in pcm else c["sym"].split(":")[1]
        fp_id = pcm[c["ref"]][1] if c["ref"] in pcm else c["fp"]
        x, y, rot = L[c["ref"]]
        props = {{"mpn": "MPN", "mfr": "Manufacturer", "lcsc": "LCSC"}[k]: c[k] for k in ("mpn", "mfr", "lcsc") if c.get(k)}
        if c.get("note"):
            props["Note"] = c["note"]
        sh.symbol(lib_name, c["ref"], c["value"], x, y, rot, fp_id, props,
                  in_bom=not c.get("exclude_from_bom"), on_board=True)
        pdefs = sh.pins[lib_name]
        for num, (px, py, ang, ln, ptype, hidden) in pdefs.items():
            sx, sy = xform(px, py, x, y, rot)
            net = c["pins"].get(num, "__absent__")
            if net == "__absent__":
                if ptype == "no_connect" or hidden:
                    continue
                sys.exit(f"{c['ref']} pin {num} has no net entry")
            pt = (sx, sy)
            if pt in at_pt:
                if at_pt[pt]["net"] != net:
                    sys.exit(f"{c['ref']}.{num}: stacked pins, nets {at_pt[pt]['net']} / {net}")
                continue
            p = dict(ref=c["ref"], num=num, x=sx, y=sy, d=outward(ang, rot), net=net, lib=lib_name)
            at_pt[pt] = p
            P.append(p)
        for num in c["pins"]:
            if num not in pdefs:
                sys.exit(f"{c['ref']}: netlist pin {num} not on symbol {lib_name}")

    # 2. joins
    parent = list(range(len(P)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    wires = []
    pts_all = {(p["x"], p["y"]) for p in P}

    def clear(a, b):
        """no other pin end strictly between a and b"""
        for (x, y) in pts_all:
            if (x, y) in (a, b):
                continue
            if a[0] == b[0] == x and min(a[1], b[1]) < y < max(a[1], b[1]):
                return False
            if a[1] == b[1] == y and min(a[0], b[0]) < x < max(a[0], b[0]):
                return False
        return True

    idx = {i: p for i, p in enumerate(P)}
    for i, a in idx.items():
        if a["net"] is None:
            continue
        for j, b in idx.items():
            if j <= i or b["net"] != a["net"]:
                continue
            A, B = (a["x"], a["y"]), (b["x"], b["y"])
            same_side = a["ref"] == b["ref"] and a["d"] == b["d"] and (
                (a["d"][0] != 0 and A[0] == B[0] and abs(A[1] - B[1]) <= 5.09) or
                (a["d"][1] != 0 and A[1] == B[1] and abs(A[0] - B[0]) <= 5.09))
            facing = (a["ref"] != b["ref"] and a["d"] == (-b["d"][0], -b["d"][1]) and
                      ((a["d"][1] == 0 and A[1] == B[1] and (B[0] - A[0]) * a["d"][0] > 0) or
                       (a["d"][0] == 0 and A[0] == B[0] and (B[1] - A[1]) * a["d"][1] > 0)) and
                      math.dist(A, B) <= 30)
            two = len(sh.pins[a["lib"]]) <= 2 and len(sh.pins[b["lib"]]) <= 2
            parallel = (two and a["ref"] != b["ref"] and a["d"] == b["d"] and a["net"] not in POWER and
                        ((a["d"][1] != 0 and A[1] == B[1]) or (a["d"][0] != 0 and A[0] == B[0])) and math.dist(A, B) <= 12)
            if (same_side or facing or parallel) and clear(A, B) and find(i) != find(j):
                parent[find(i)] = find(j)
                wires.append((A, B, (a["ref"], a["num"], b["ref"], b["num"])))
    for A, B, key in wires:
        sh.wire(A, B, key)

    # 3. one marker per joined group (labels for every group of a signal net, so names stay)
    groups = {}
    for i, p in idx.items():
        groups.setdefault(find(i), []).append(p)
    for g in groups.values():
        net = g[0]["net"]
        if net is None:
            for p in g:
                sh.items.append(f'(no_connect (at {f(p["x"])} {f(p["y"])}) (uuid "{U("nc", p["ref"], p["num"])}"))')
            continue
        has_facing = any(w[2][0] != w[2][2] and any((p["x"], p["y"]) in (w[0], w[1]) for p in g) for w in wires)
        if len(g) > 1 and net not in POWER and has_facing:
            # a wired group: label on the middle of its first wire
            w = next(w for w in wires if any((p["x"], p["y"]) in (w[0], w[1]) for p in g))
            (ax, ay), (bx, by) = w[0], w[1]
            mx, my = snap((ax + bx) / 2), snap((ay + by) / 2)
            if ay == by:
                sh.label(net, mx, my, 0, ("grp",) + w[2])
            else:
                sh.label(net, mx, my, 0, ("grp",) + w[2])
            continue
        # pick the pin whose direction suits the marker best
        want = (0, 1) if net in GNDLIKE else (0, -1)
        best = sorted(g, key=lambda p: (p["d"] != want, p["d"][1] == 0, p["ref"], p["num"]))[0]
        multi = len(sh.pins[best["lib"]]) > 3
        if len(g) > 1 and net not in POWER:   # same-side group: label at the outermost pin
            best = sorted(g, key=lambda p: (p["y"], p["x"]))[-1 if g[0]["d"][0] == 0 else 0]
        sh.marker(net, best["x"], best["y"], best["d"], (best["ref"], best["num"]), multi)

    for net, x, y in FLAGS:
        x, y = snap(x), snap(y)
        sh.symbol("PWR_FLAG", f"#FLG{net}", "PWR_FLAG", x, y, 0, in_bom=False, on_board=False,
                  uid=U("flg", net), hide_ref=True)
        sh.power(net, x, y, (0, 1) if net in GNDLIKE else (0, 1), ("flag", net))

    for title, x0, y0, x1, y1, note in BLOCKS:
        sh.items.append(f'(rectangle (start {f(x0)} {f(y0)}) (end {f(x1)} {f(y1)}) (stroke (width 0.254) (type dash)) '
                        f'(fill (type none)) (uuid "{U("frame", title)}"))')
        sh.items.append(f'(text {q(title)} (exclude_from_sim no) (at {f(x0 + 2)} {f(y0 + 5)} 0) '
                        f'{font(2.2, " (justify left bottom)", bold=True)} (uuid "{U("title", title)}"))')
        if note:
            sh.items.append(f'(text {q(note)} (exclude_from_sim no) (at {f(x0 + 2)} {f(y1 - 2.5 - 1.9 * note.count(chr(10)))} 0) '
                            f'{font(1.27, " (justify left bottom)")} (uuid "{U("note", title)}"))')

    head = (f'(kicad_sch (version 20260101) (generator "eeschema") (generator_version "10.0") (uuid "{root}") (paper "A2") '
            f'(title_block (title "RF-Power-Monitor rev2") (date "2026-11-02") (rev "2.0") (company "Kenneth Foli Jørgensen") '
            f'(comment 1 "RP2040 + W5500 + WIZPoE-P1 on one board; mains detect GP27, TMP1075, factory reset GP22") '
            f'(comment 2 "Generated by kicad/tools/gen_sch.py from design/netlist.json: edit those, not this file")) ')
    used = set(sh.used)
    body = embedded_all({k: v for k, v in syms.items() if k in used}) + " " + " ".join(sh.items)
    tail = ' (sheet_instances (path "/" (page "1"))) (embedded_fonts no))\n'
    out = os.path.join(KDIR, f"{NAME}.kicad_sch")
    open(out, "w").write(head + body + tail)
    print(f"wrote {out}: {len(nl['components'])} parts, {len(wires)} wires, {sh.npwr} power symbols")


if __name__ == "__main__":
    main()
