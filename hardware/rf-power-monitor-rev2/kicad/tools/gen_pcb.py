#!/usr/bin/env python3
"""Generate the rev2 board (.kicad_pcb) from design/netlist.json: footprints from the board's PCM
library (lib/pcm) and the project library, placed by PLACE below (board mm, origin top-left, y down),
the outline, the mains slot and the board's .kicad_dru (JLCPCB 2-layer pack + isolation rules).
Routing, pours and vias are AI Flow's (route / pour / land), not this file's.
Run: python3 kicad/tools/gen_pcb.py
"""
import json
import math
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, dump, q, uq, find, findall  # noqa: E402
import gen_sch  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
KDIR = os.path.dirname(HERE)
ROOT = os.path.dirname(KDIR)
NAME = "rf-power-monitor-rev2"
OX, OY = 100.0, 100.0           # board origin on the page
W, H = 100.0, 64.0              # board size
STOCK = os.environ.get("KICAD10_FOOTPRINT_DIR", "/mnt/c/Program Files/KiCad/10.0/share/kicad/footprints")
PCM_PRETTY = os.path.join(KDIR, "lib", "pcm", "footprints", "rf-power-monitor-rev2.pretty")
PROJ_PRETTY = os.path.join(KDIR, "lib", "RFPM_Rev2.pretty")
REV1 = os.path.join(ROOT, "..", "pico-poe-mains-carrier")
POWER_NETS = {"GND", "+3V3", "VSYS", "VBUS", "3V3A", "+1V1", "CHASSIS"}


def U(*k):
    return str(uuid.uuid5(gen_sch.NS, "pcb/" + "/".join(str(x) for x in k)))


def f(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


# ---------------------------------------------------------------------------------------------
# Placement: ref -> (x, y, rot, side)   (board mm; rot in degrees CCW as KiCad shows it)
# ---------------------------------------------------------------------------------------------
PLACE = {}


def pl(ref, x, y, rot=0, side="F"):
    PLACE[ref] = (x, y, rot, side)


# --- Ethernet: RJ45 at the left edge (mouth out), magnetics, W5500 -------------------------------
pl("J2", 11.3, 11.5, 270)
pl("T1", 24.5, 11.5, 90)
pl("U3", 44.5, 12.0, 0)
pl("FB1", 39.2, 3.4, 0)
pl("C11", 42.4, 3.4, 0)
pl("C17", 44.5, 5.9, 0)      # AVDD (top)
pl("C12", 38.3, 9.2, 90)     # AVDD pins 4/8 (left)
pl("C13", 38.3, 12.4, 90)
pl("R11", 38.3, 15.4, 90)    # EXRES1
pl("C14", 41.5, 18.4, 0)     # AVDD 15/17 (bottom)
pl("C15", 44.5, 18.4, 0)
pl("C16", 47.5, 18.4, 0)     # AVDD 21
pl("C19", 43.0, 19.8, 0)     # TOCAP
pl("C20", 46.0, 19.8, 0)     # 1V2O
pl("C18", 50.6, 15.6, 90)    # VDD 28
pl("C26", 30.4, 3.6, 0)      # transformer centre taps (chip side)
pl("R14", 51.4, 6.6, 0)
pl("R15", 51.4, 8.0, 0)
pl("R16", 51.4, 9.4, 0)
pl("Y1", 54.0, 18.6, 0)
pl("R12", 54.0, 15.8, 0)
pl("C21", 51.0, 21.0, 90)
pl("C22", 57.6, 18.6, 90)
pl("R13", 56.0, 21.4, 90)
# front end between T1 and U3 (chip side of the magnetics)
pl("R19", 32.0, 5.6, 0)
pl("R20", 32.0, 7.0, 0)
pl("R17", 32.0, 8.4, 0)
pl("R18", 32.0, 9.8, 0)
pl("C23", 32.0, 13.2, 0)
pl("C24", 32.0, 14.6, 0)
pl("R21", 35.2, 13.2, 0)
pl("R22", 35.2, 14.6, 0)
pl("R23", 32.0, 16.0, 0)
pl("R24", 32.0, 17.4, 0)
pl("C25", 35.2, 17.4, 90)
pl("R25", 17.6, 3.0, 0)      # link / act LED resistors at the jack's LED pins (logic side of the jack)
pl("R26", 17.6, 20.0, 0)
# PoE side: Bob Smith network and chassis capacitors under the module's input end
for i, r in enumerate(("R7", "R8", "R9", "R10")):
    pl(r, 7.5, 27.0 + i * 2.2, 0)
for i, c in enumerate(("C5", "C6", "C7", "C8")):
    pl(c, 11.6, 27.0 + i * 2.2, 0)
pl("C9", 9.5, 37.0, 0)       # Bob Smith node to chassis
pl("C10", 12.0, 1.7, 0)     # chassis to logic GND (the one part across the barrier)
pl("U2", 21.5, 33.5, 180)    # WIZPoE-P1: input header left (near the jack), 5 V out on the right
# --- RP2040 (rotated: GPIO16-21 face the W5500) -----------------------------------------------------
pl("U4", 67.0, 14.0, 180)
pl("Y2", 67.0, 5.6, 0)
pl("R29", 63.6, 5.0, 90)
pl("C40", 70.6, 4.2, 90)
pl("C41", 61.6, 6.4, 0)
pl("C27", 60.4, 9.0, 90)
pl("C28", 60.4, 19.0, 90)
pl("C29", 73.6, 9.0, 90)
pl("C30", 73.6, 19.0, 90)
pl("C31", 60.4, 14.0, 90)
pl("C32", 70.4, 7.6, 0)
pl("C33", 60.4, 16.2, 90)
pl("C34", 73.6, 16.2, 90)
pl("C35", 63.4, 20.4, 90)
pl("C36", 65.0, 20.4, 90)
pl("C37", 63.8, 9.1, 0)
pl("C38", 67.4, 20.4, 90)
pl("C39", 75.0, 21.0, 0)
pl("U5", 68.6, 25.8, 270)
pl("C42", 73.4, 27.0, 90)
pl("R30", 73.4, 23.0, 90)
pl("R27", 79.5, 19.4, 0)
pl("R28", 79.5, 20.8, 0)
pl("R3", 85.0, 23.0, 90)
pl("R4", 86.5, 23.0, 90)
pl("J1", 95.6, 21.0, 90)     # USB-C at the right edge
pl("R1", 88.8, 17.0, 90)
pl("R2", 88.8, 25.0, 90)
pl("SW1", 80.0, 4.5, 0)      # BOOTSEL
pl("SW2", 88.5, 4.5, 0)      # RESET
pl("R31", 86.6, 10.6, 0)
pl("J3", 78.8, 10.0, 90)     # SWD
pl("R32", 93.6, 12.6, 90)
pl("D2", 93.6, 9.6, 90)
# --- power: VSYS OR-ing and the 3V3 buck next to the module's 5 V output -------------------------------
pl("D1", 80.0, 30.0, 180)
pl("C1", 43.5, 28.0, 90)
pl("C2", 45.8, 28.0, 90)
pl("C3", 56.6, 31.0, 270)
pl("U1", 53.5, 31.0, 0)
pl("L1", 52.4, 34.6, 90)
pl("C4", 56.2, 36.0, 0)
pl("R5", 55.6, 28.4, 180)
pl("R6", 53.6, 28.4, 180)
# --- mains (bottom right): J4 at the right edge, isolation boundary at x ~ 66 ----------------------
pl("J4", 93.5, 58.0, 90)
pl("F1", 81.8, 51.0, 0)
pl("R33", 73.2, 51.0, 0)
pl("R34", 87.0, 64.0, 0)
pl("R35", 81.0, 64.0, 0)
pl("R36", 75.0, 64.0, 0)
pl("U6", 66.5, 58.0, 180)
pl("C43", 57.0, 55.0, 90)
pl("R37", 57.0, 59.0, 90)
pl("U7", 53.0, 57.0, 180)
pl("C44", 53.0, 53.0, 0)
pl("R38", 48.0, 60.5, 90)
pl("D3", 48.0, 65.0, 90)
pl("D4", 52.0, 65.0, 90)
pl("R39", 52.0, 61.0, 90)
# --- ambient sensor and factory reset (bottom left, away from the heat) ------------------------------
pl("U8", 9.0, 59.0, 0)
pl("C45", 14.0, 56.0, 90)
pl("R40", 4.0, 54.5, 0)
pl("R41", 4.0, 56.0, 0)
pl("SW3", 24.0, 64.5, 0)
# --- mounting holes ------------------------------------------------------------------------------
pl("H1", 3.5, 66.5)
pl("H2", 96.5, 3.5)
pl("H3", 96.5, 38.0)
pl("H4", 36.0, 66.5)
# --- test pads: beside the part on each probed net ---------------------------------------------------
TP_SPOTS = {"TP1": (84.0, 27.0), "TP2": (42.0, 30.5), "TP3": (64.0, 31.5), "TP4": (49.8, 33.6), "TP5": (54.6, 26.9),
            "TP6": (62.3, 9.1), "TP7": (37.0, 2.6), "TP8": (60.4, 3.4), "TP9": (51.0, 23.0), "TP10": (54.0, 4.6),
            "TP11": (59.5, 11.5), "TP12": (59.4, 55.0), "TP13": (49.6, 56.0), "TP14": (16.0, 59.0)}
for _r, (x, y) in TP_SPOTS.items():
    pl(_r, x, y)

# the RP2040 cluster sits 1.5 mm further from the W5500 than first drawn (room for its top-left escapes)
_U4_CLUSTER = {"U4", "Y2", "R29", "C40", "C41", "C27", "C28", "C29", "C30", "C31", "C32", "C33", "C34", "C35", "C36",
               "C37", "C38", "C39", "U5", "C42", "R30", "TP6", "TP8", "TP11"}
for _r in ():   # tried: shifting the cluster did not route better (route12); kept as a record
    _x, _y, _rot, _s = PLACE[_r]
    PLACE[_r] = (round(_x + 1.5, 4), _y, _rot, _s)

# the bottom section (mains, sensor, holes, its test pads) sits 6 mm higher than first drawn
for _r, (_x, _y, _rot, _s) in list(PLACE.items()):
    if _y > 45:
        PLACE[_r] = (_x, round(_y - 6, 4), _rot, _s)

# isolation slot under the optocoupler (between its mains and logic pins), as rev1
SLOT = (66.5, 48.5, 55.5, 0.8)    # x, y0, y1, width (vertical slot)


# ---------------------------------------------------------------------------------------------
def load_fp(fpid):
    lib, name = fpid.split(":", 1)
    if lib == gen_sch.PCM:
        path = os.path.join(PCM_PRETTY, name + ".kicad_mod")
    elif lib == "RFPM_Rev2":
        path = os.path.join(PROJ_PRETTY, name + ".kicad_mod")
    else:
        path = os.path.join(STOCK, lib + ".pretty", name + ".kicad_mod")
    return parse(open(path, encoding="utf-8").read())


def bnet(net):
    return net if net in POWER_NETS else "/" + net


def rotate_at(node, rot):
    at = find(node, "at")
    if at is None:
        return
    a = (float(at[3]) if len(at) > 3 else 0.0) + rot
    a = a % 360
    del at[3:]
    if a:
        at.append(f(a))


def place_fp(c, fpid, x, y, rot, side, sym_uuid):
    fp = load_fp(fpid)
    fp[1] = q(fpid)
    keep = []
    for it in fp[2:]:
        if isinstance(it, list) and it[0] in ("version", "generator", "generator_version"):
            continue
        keep.append(it)
    head = [fp[0], fp[1], ["layer", q("F.Cu")], ["uuid", q(U("fp", c["ref"]))],
            ["at", f(OX + x), f(OY + y)] + ([f(rot)] if rot else [])]
    body = [it for it in keep if not (isinstance(it, list) and it[0] == "layer")]
    for it in body:
        if not isinstance(it, list):
            continue
        if it[0] == "property":
            k = uq(it[1])
            if k == "Reference":
                it[2] = q(c["ref"])
            elif k == "Value":
                it[2] = q(c["value"])
            rotate_at(it, rot)
        elif it[0] == "fp_text":
            if len(it) > 2 and it[1] == "reference":
                it[2] = q(c["ref"])
            rotate_at(it, rot)
        elif it[0] == "pad":
            rotate_at(it, rot)
            num = uq(it[1])
            net = c["pins"].get(num)
            for old in findall(it, "net"):
                it.remove(old)
            if net:
                it.append(["net", q(bnet(net))])
    if c.get("exclude_from_bom"):
        attr = next((it for it in body if isinstance(it, list) and it[0] == "attr"), None)
        if attr is None:
            attr = ["attr", "through_hole"]
            body.append(attr)
        for flag in ("exclude_from_pos_files", "exclude_from_bom"):
            if flag not in attr:
                attr.append(flag)
    props = []
    for k, v in (("MPN", c.get("mpn")), ("LCSC", c.get("lcsc")), ("Manufacturer", c.get("mfr"))):
        if v:
            props.append(parse(f'(property {q(k)} {q(v)} (at 0 0 {f(rot)}) (layer "F.Fab") (hide yes) (uuid {q(U("p", c["ref"], k))}) '
                               f'(effects (font (size 1 1) (thickness 0.15))))'))
    link = [["path", q("/" + sym_uuid)], ["sheetname", q("/")], ["sheetfile", q(NAME + ".kicad_sch")]]
    node = head + props + link + body
    # remove earlier uuid if the library footprint had one at top level
    seen_uuid = False
    out = []
    for it in node:
        if isinstance(it, list) and it and it[0] == "uuid":
            if seen_uuid:
                continue
            seen_uuid = True
        out.append(it)
    if side == "B":
        raise NotImplementedError("bottom-side parts: none on this board")
    return dump(out)


def gr_line(a, b, layer, w=0.1):
    return (f'(gr_line (start {f(OX + a[0])} {f(OY + a[1])}) (end {f(OX + b[0])} {f(OY + b[1])}) '
            f'(stroke (width {w}) (type default)) (layer {q(layer)}) (uuid {q(U("l", layer, a, b))}))')


def gr_arc(s, m, e, layer, w=0.1):
    return (f'(gr_arc (start {f(OX + s[0])} {f(OY + s[1])}) (mid {f(OX + m[0])} {f(OY + m[1])}) '
            f'(end {f(OX + e[0])} {f(OY + e[1])}) (stroke (width {w}) (type default)) (layer {q(layer)}) '
            f'(uuid {q(U("a", layer, s, e))}))')


def outline():
    r = 2.0
    k = r * (1 - math.sqrt(0.5))
    o = [gr_line((r, 0), (W - r, 0), "Edge.Cuts"), gr_line((W, r), (W, H - r), "Edge.Cuts"),
         gr_line((W - r, H), (r, H), "Edge.Cuts"), gr_line((0, H - r), (0, r), "Edge.Cuts"),
         gr_arc((W - r, 0), (W - k, k), (W, r), "Edge.Cuts"), gr_arc((W, H - r), (W - k, H - k), (W - r, H), "Edge.Cuts"),
         gr_arc((r, H), (k, H - k), (0, H - r), "Edge.Cuts"), gr_arc((0, r), (k, k), (r, 0), "Edge.Cuts")]
    sx, y0, y1, sw = SLOT
    hw = sw / 2
    o += [gr_line((sx + hw, y0), (sx + hw, y1), "Edge.Cuts"), gr_arc((sx + hw, y1), (sx, y1 + hw), (sx - hw, y1), "Edge.Cuts"),
          gr_line((sx - hw, y1), (sx - hw, y0), "Edge.Cuts"), gr_arc((sx - hw, y0), (sx, y0 - hw), (sx + hw, y0), "Edge.Cuts")]
    return o


def main():
    nl = json.load(open(os.path.join(ROOT, "design", "netlist.json")))
    pcm = gen_sch.pcm_parts(nl)
    missing = [c["ref"] for c in nl["components"] if c["ref"] not in PLACE]
    if missing:
        sys.exit(f"no placement for {missing}")
    head = open(os.path.join(REV1, "tools", "stock_fp", "board_header.txt"), encoding="utf-8").read()
    o = ['(kicad_pcb (version 20260206) (generator "pcbnew") (generator_version "10.0") '
         '(general (thickness 1.6) (legacy_teardrops no)) (paper "A4") '
         '(title_block (title "RF-Power-Monitor rev2") (date "2026-11-02") (rev "2.0") (company "Kenneth Foli Jørgensen"))\n',
         head]
    for c in nl["components"]:
        fpid = pcm[c["ref"]][1] if c["ref"] in pcm else c["fp"]
        x, y, rot, side = PLACE[c["ref"]]
        o.append(place_fp(c, fpid, x, y, rot, side, gen_sch.U("sym", c["ref"])) + "\n")
    o += [x + "\n" for x in outline()]
    o.append("(embedded_fonts no))\n")
    out = os.path.join(KDIR, NAME + ".kicad_pcb")
    open(out, "w", encoding="utf-8").write("".join(o))
    # rules: the JLCPCB 2-layer pack, then the board's isolation rules
    pack = open(os.path.join(ROOT, "aiflow-run", "rules", "jlcpcb-2L.kicad_dru")).read()
    extra = open(os.path.join(HERE, "isolation.kicad_dru")).read()
    open(os.path.join(KDIR, NAME + ".kicad_dru"), "w").write(pack.rstrip() + "\n\n" + extra)
    print(f"wrote {out}: {len(nl['components'])} footprints")


if __name__ == "__main__":
    main()
