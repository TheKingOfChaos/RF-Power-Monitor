#!/usr/bin/env python3
"""Silkscreen for the routed rev2 board (run with KiCad closed; edits the saved board in place).

- Every footprint Reference is placed by a greedy search: candidate spots around the part's body,
  0.8 mm text (JLCPCB's tested minimum), clear of every pad, part body, via, hole, the board edge
  and the labels already placed; important parts first. A small passive with no free spot is hidden
  (its reference stays on the fab drawing). Values stay on F.Fab.
- Test pads get their net name; connectors, buttons and LEDs get what they are; the mains area is
  framed with a DANGER label; the bottom carries the board's identity.
- Writes <run>/silkscreen/audit.json for `adom-aiflow silkscreen-audit` (obstacles, inventory, marks).

usage: silkscreen.py <board.kicad_pcb> <audit.json>
"""
import json
import math
import sys
import uuid

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sexp import parse, dump, q, uq, find, findall  # noqa: E402

H = 0.8            # text height
STROKE = 0.15
CW = 0.82          # character advance as a fraction of the height (KiCad stroke font, with margin)
CLR = 0.15         # silk to anything
EDGE = 0.5         # silk to board edge
NS = uuid.UUID("2b1b6f0e-7a3c-4d65-9b1e-5d3f8c2a7e44")
OX, OY, W, HB = 100.0, 100.0, 100.0, 64.0


def U(*k):
    return str(uuid.uuid5(NS, "/".join(map(str, k))))


def text_box(cx, cy, s, h=H, rot=0):
    w = len(s) * h * CW + STROKE
    hh = h + STROKE
    return (cx - w / 2, cy - hh / 2, cx + w / 2, cy + hh / 2) if rot in (0, 180) else (cx - hh / 2, cy - w / 2, cx + hh / 2, cy + w / 2)


def overlap(a, b, m=0.0):
    return a[0] < b[2] + m and b[0] < a[2] + m and a[1] < b[3] + m and b[1] < a[3] + m


def xform(X, Y, rot, x, y):
    t = math.radians(rot)
    return X + x * math.cos(t) + y * math.sin(t), Y - x * math.sin(t) + y * math.cos(t)


def inv(X, Y, rot, gx, gy):
    t = math.radians(rot)
    dx, dy = gx - X, gy - Y
    return dx * math.cos(t) - dy * math.sin(t), dx * math.sin(t) + dy * math.cos(t)


def main():
    path, audit_out = sys.argv[1], sys.argv[2]
    b = parse(open(path, encoding="utf-8").read())
    parts, pads, vias, holes, silk_graphics = {}, [], [], [], []
    for fp in findall(b, "footprint"):
        at = find(fp, "at")
        X, Y, rot = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        ref = uq([p for p in findall(fp, "property") if uq(p[1]) == "Reference"][0][2])
        xs, ys = [], []
        for g in fp:
            if isinstance(g, list) and g and g[0] in ("fp_line", "fp_rect", "fp_poly", "fp_circle", "fp_arc"):
                lay = find(g, "layer")
                if lay and "CrtYd" in uq(lay[1]):
                    for k in ("start", "end", "center", "mid"):
                        n = find(g, k)
                        if n:
                            gx, gy = xform(X, Y, rot, float(n[1]), float(n[2]))
                            xs.append(gx)
                            ys.append(gy)
                    pts = find(g, "pts")
                    if pts:
                        for xy in findall(pts, "xy"):
                            gx, gy = xform(X, Y, rot, float(xy[1]), float(xy[2]))
                            xs.append(gx)
                            ys.append(gy)
        for g in list(fp):
            if isinstance(g, list) and g and g[0] in ("fp_text",) and len(g) > 2 and g[1] == "user" and ref == "U2":
                fp.remove(g)
        for g in fp:
            if isinstance(g, list) and g and g[0] in ("fp_line", "fp_rect", "fp_circle", "fp_arc", "fp_poly"):
                lay = find(g, "layer")
                if lay and uq(lay[1]) == "F.SilkS":
                    pts = []
                    for k in ("start", "end", "center", "mid"):
                        n = find(g, k)
                        if n:
                            pts.append(xform(X, Y, rot, float(n[1]), float(n[2])))
                    if pts:
                        silk_graphics.append((min(a for a, _ in pts) - 0.1, min(c for _, c in pts) - 0.1,
                                              max(a for a, _ in pts) + 0.1, max(c for _, c in pts) + 0.1))
        fpads = []
        for p in findall(fp, "pad"):
            a = find(p, "at")
            gx, gy = xform(X, Y, rot, float(a[1]), float(a[2]))
            sz = find(p, "size")
            w, h = float(sz[1]), float(sz[2])
            pr = (float(a[3]) if len(a) > 3 else rot)
            if round(pr) % 180 == 90:
                w, h = h, w
            r = max(w, h) / 2 if round(pr) % 90 else None
            box = (gx - (r or w / 2), gy - (r or h / 2), gx + (r or w / 2), gy + (r or h / 2))
            lays = [uq(v) for v in find(p, "layers")[1:]]
            fpads.append({"id": f"{ref}:pad:{uq(p[1]) or 'h'}", "box": box, "front": any(l in ("F.Cu", "*.Cu", "F.Mask", "*.Mask") for l in lays)})
            if p[2] in ("thru_hole", "np_thru_hole"):
                holes.append(box)
        if not xs:
            pb = [pd["box"] for pd in fpads] or [(X - 0.5, Y - 0.5, X + 0.5, Y + 0.5)]
            xs = [min(p[0] for p in pb), max(p[2] for p in pb)]
            ys = [min(p[1] for p in pb), max(p[3] for p in pb)]
        parts[ref] = {"node": fp, "X": X, "Y": Y, "rot": rot, "body": (min(xs), min(ys), max(xs), max(ys)), "pads": fpads}
        pads += fpads
    for v in findall(b, "via"):
        a = find(v, "at")
        s = float(find(v, "size")[1]) / 2
        vias.append({"id": f"via@{a[1]},{a[2]}", "box": (float(a[1]) - s, float(a[2]) - s, float(a[1]) + s, float(a[2]) + s)})

    # the mains frame (drawn below) is an obstacle too: its four sides as thin boxes
    mx0, my0, mx1, my1 = OX + 68.6, OY + 42.6, OX + W - 0.8, OY + HB - 0.8
    frame = [(mx0 - 0.15, my0 - 0.15, mx1 + 0.15, my0 + 0.15), (mx1 - 0.15, my0 - 0.15, mx1 + 0.15, my1 + 0.15),
             (mx0 - 0.15, my1 - 0.15, mx1 + 0.15, my1 + 0.15), (mx0 - 0.15, my0 - 0.15, mx0 + 0.15, my1 + 0.15)]
    obstacles = [p["box"] for p in pads if p["front"]] + [v["box"] for v in vias] + holes + silk_graphics + frame
    bodies = {r: p["body"] for r, p in parts.items()}
    placed = []
    inside = (OX + EDGE, OY + EDGE, OX + W - EDGE, OY + HB - EDGE)

    def free(box, own=None):
        if box[0] < inside[0] or box[1] < inside[1] or box[2] > inside[2] or box[3] > inside[3]:
            return False
        if any(overlap(box, o, CLR) for o in obstacles):
            return False
        if any(overlap(box, bb, 0.05) for r, bb in bodies.items()):
            return False
        return not any(overlap(box, p, CLR) for p in placed)

    def place(text, near, rots=(0, 90), h=H, own=None):
        x0, y0, x1, y1 = near
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        cands = []
        for rot in rots:
            for gap in (0.25, 0.6, 1.0, 1.6):
                tb = text_box(0, 0, text, h, rot)
                tw, th = tb[2] - tb[0], tb[3] - tb[1]
                for px, py in ((cx, y0 - gap - th / 2), (cx, y1 + gap + th / 2), (x0 - gap - tw / 2, cy), (x1 + gap + tw / 2, cy),
                               (x0 + tw / 2, y0 - gap - th / 2), (x1 - tw / 2, y0 - gap - th / 2),
                               (x0 + tw / 2, y1 + gap + th / 2), (x1 - tw / 2, y1 + gap + th / 2)):
                    cands.append((gap + 0.3 * (rot != 0), px, py, rot))
        cands.sort()
        for _, px, py, rot in cands:
            box = text_box(px, py, text, h, rot)
            if free(box, own):
                placed.append(box)
                return (round(px, 3), round(py, 3), rot, box)
        return None

    def prio(r):
        order = ["J", "U", "SW", "T", "F", "L", "D", "Y", "TP", "H", "R", "C", "FB"]
        k = next((i for i, p in enumerate(order) if r.startswith(p) and (p != "T" or not r.startswith("TP"))), 99)
        return (k, -(bodies[r][2] - bodies[r][0]) * (bodies[r][3] - bodies[r][1]))

    netname = {}
    for fp in findall(b, "footprint"):
        ref = uq([p for p in findall(fp, "property") if uq(p[1]) == "Reference"][0][2])
        if ref.startswith("TP"):
            n = find(find(fp, "pad"), "net")
            netname[ref] = uq(n[1]).lstrip("/") if n else ref

    marks, hidden = [], []
    for ref in sorted(parts, key=prio):
        p = parts[ref]
        if ref.startswith("H"):
            continue
        label = netname.get(ref, ref)
        res = place(label, p["body"], own=ref)
        refprop = [x for x in findall(p["node"], "property") if uq(x[1]) == "Reference"][0]
        # rebuild the Reference property: position (footprint-local), size, visibility
        for k in list(refprop[3:]):
            refprop.remove(k)
        if res:
            px, py, rot, box = res
            lx, ly = inv(p["X"], p["Y"], p["rot"], px, py)
            refprop += [["at", f"{lx:.3f}", f"{ly:.3f}", str(rot)], ["layer", q("F.SilkS")], ["uuid", q(U("ref", ref))],
                        parse(f"(effects (font (size {H} {H}) (thickness {STROKE})))")]
            if label != ref:
                refprop[2] = q(ref)
                # the test pad's label is its net: a board text, the reference itself stays hidden
                refprop.append(["hide", "yes"])
                b.append(parse(f'(gr_text {q(label)} (at {px:.3f} {py:.3f} {rot}) (layer "F.SilkS") (uuid {q(U("tp", ref))}) '
                               f'(effects (font (size {H} {H}) (thickness {STROKE}))))'))
            marks.append({"id": ref, "layer": "F.SilkS", "box": [round(v, 3) for v in box], "text": label})
        else:
            refprop += [["at", "0", "0", "0"], ["layer", q("F.SilkS")], ["hide", "yes"], ["uuid", q(U("ref", ref))],
                        parse(f"(effects (font (size {H} {H}) (thickness {STROKE})))")]
            hidden.append(ref)

    # board texts: function labels, mains warning, identity
    extra = []

    def add_text(text, near, layer="F.SilkS", h=H, rots=(0,), mirror=False):
        res = place(text, near, rots, h)
        if not res:
            return False
        px, py, rot, box = res
        just = " (justify mirror)" if mirror else ""
        b.append(parse(f'(gr_text {q(text)} (at {px:.3f} {py:.3f} {rot}) (layer {q(layer)}) (uuid {q(U("txt", text))}) '
                       f'(effects (font (size {h} {h}) (thickness {STROKE})){just}))'))
        marks.append({"id": f"text:{text}", "layer": layer, "box": [round(v, 3) for v in box], "text": text})
        extra.append(text)
        return True

    for text, ref in (("USB 5V (4.75-5.5 V)", "J1"), ("PoE 802.3af 37-57 V", "J2"), ("BOOT", "SW1"), ("RESET", "SW2"),
                      ("FACTORY RESET", "SW3"), ("STATUS", "D2"), ("MAINS ON", "D3"), ("MAINS OFF", "D4"), ("SWD", "J3"),
                      ("TEMP", "U8"), ("230VAC L N", "J4")):
        add_text(text, bodies[ref])
    # mains area frame (logic side boundary at the optocoupler slot) and warning
    for a, c in (((mx0, my0), (mx1, my0)), ((mx1, my0), (mx1, my1)), ((mx1, my1), (mx0, my1)), ((mx0, my1), (mx0, my0))):
        for lay in ("F.SilkS", "B.SilkS"):
            b.append(parse(f'(gr_line (start {a[0]:.3f} {a[1]:.3f}) (end {c[0]:.3f} {c[1]:.3f}) (stroke (width 0.25) (type default)) '
                           f'(layer {q(lay)}) (uuid {q(U("frame", lay, a, c))}))'))
    add_text("DANGER 230VAC", (mx0, my0 + 6, mx1, my0 + 9), h=1.0)
    # bottom: identity (mirrored text reads correctly from below)
    idy = [("RF-Power-Monitor rev2", 1.2), ("2026-11 Kenneth Foli Jorgensen", 0.8),
           ("wiki.adom.inc/thekingofchaos/rf-power-monitor-rev2", 0.8), ("DANGER: 230VAC - HAZARDOUS VOLTAGE", 1.0)]
    # each identity line on its own via- and hole-free spot on the bottom face, out of the mains corner
    back_obst = [v["box"] for v in vias] + holes
    back_placed = []
    for text, h in idy:
        bw = text_box(0, 0, text, h)[2] * 2
        spot = None
        for yy in [OY + 2 + 0.25 * k for k in range(int((HB - 4) / 0.25))]:
            for xx in [OX + 2 + bw / 2 + 0.25 * k for k in range(int(max(0, W - 4 - bw) / 0.25))]:
                bb = text_box(xx, yy, text, h)
                if bb[2] > OX + 66 and bb[3] > OY + 41:
                    continue
                if not any(overlap(bb, o, CLR) for o in back_obst + back_placed):
                    spot = (xx, yy, bb)
                    break
            if spot:
                break
        if not spot:
            print(f"no via-free spot on the bottom for {text!r}: left out")
            continue
        sx, sy, bx = spot
        back_placed.append(bx)
        b.append(parse(f'(gr_text {q(text)} (at {sx:.3f} {sy:.3f} 0) (layer "B.SilkS") (uuid {q(U("id", text))}) '
                       f'(effects (font (size {h} {h}) (thickness {STROKE})) (justify mirror)))'))
        marks.append({"id": f"text:{text}", "layer": "B.SilkS", "box": [round(v, 3) for v in bx], "text": text})

    open(path, "w", encoding="utf-8").write(dump(b) + "\n")

    obst = []
    inv_items = []
    for r, p in parts.items():
        obst.append({"id": f"{r}:body", "kind": "body", "layer": "F.SilkS", "box": [round(v, 3) for v in p["body"]]})
        inv_items.append({"id": f"{r}:body", "kind": "body", "layers": ["F.SilkS"]})
        for pd in p["pads"]:
            obst.append({"id": pd["id"], "kind": "pad", "layer": "F.SilkS", "box": [round(v, 3) for v in pd["box"]]})
    for v in vias:
        for lay in ("F.SilkS", "B.SilkS"):
            obst.append({"id": v["id"], "kind": "via", "layer": lay, "box": [round(x, 3) for x in v["box"]]})
        inv_items.append({"id": v["id"], "kind": "via", "layers": ["F.SilkS", "B.SilkS"]})
    json.dump({"obstacles": obst, "inventory": inv_items, "marks": marks}, open(audit_out, "w"), indent=0)
    print(f"labels placed {len(marks)}, references hidden {len(hidden)}: {' '.join(sorted(hidden))}; board texts {len(extra)}")


if __name__ == "__main__":
    main()
