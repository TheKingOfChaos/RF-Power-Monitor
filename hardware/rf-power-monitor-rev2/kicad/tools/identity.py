#!/usr/bin/env python3
"""Front identity block as on rev1: the RF logo (hardware/pico-poe-mains-carrier/logo) with the board
name and maker beside it, on F.SilkS in the largest free spot found by a grid search against the
silkscreen audit's obstacles (pads, bodies, vias) and the labels already placed. Idempotent: items it
added before (uuid namespace below) are removed first. Run after silkscreen.py, with KiCad closed.

usage: identity.py <board.kicad_pcb> <audit.json>
"""
import json
import os
import re
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sexp import parse, dump, q, uq, find, findall  # noqa: E402

NS = uuid.UUID("6d0c3f52-1b7e-4a8e-9c55-0e7f2b9d4a11")
LOGO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "pico-poe-mains-carrier", "logo",
                    "RF-canopy-RGB-Orange.svg")
OX, OY, W, HB = 100.0, 100.0, 100.0, 64.0
EDGE, CLR, STROKE, CW = 0.8, 0.3, 0.15, 0.82
LOGO_W = 8.0
LINES = [("RF-Power-Monitor rev2", 1.2, True), ("Made by Kenneth Foli Jørgensen 2026", 0.8, False)]
MAINS = (OX + 68.6, OY + 42.6, OX + W, OY + HB)


def U(*k):
    return str(uuid.uuid5(NS, "/".join(map(str, k))))


def svg_polys(d, steps=12):
    toks = re.findall(r"[MmLlHhVvCcZz]|[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?", d)
    polys, cur, x, y, i, cmd = [], [], 0.0, 0.0, 0, None
    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]
            i += 1
            if cmd in "Zz":
                if cur:
                    polys.append(cur)
                cur = []
                continue
        rel, c = cmd.islower(), cmd.upper()
        n = [float(v) for v in toks[i:i + {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6}[c]]]
        i += len(n)
        if c == "M":
            if cur:
                polys.append(cur)
            x, y = (x + n[0], y + n[1]) if rel else (n[0], n[1])
            cur = [(x, y)]
            cmd = "l" if rel else "L"
        elif c in "LHV":
            if c == "L":
                x, y = (x + n[0], y + n[1]) if rel else (n[0], n[1])
            elif c == "H":
                x = x + n[0] if rel else n[0]
            else:
                y = y + n[0] if rel else n[0]
            cur.append((x, y))
        else:
            ox, oy = (x, y) if rel else (0.0, 0.0)
            p1, p2, p3 = (ox + n[0], oy + n[1]), (ox + n[2], oy + n[3]), (ox + n[4], oy + n[5])
            for k in range(1, steps + 1):
                t = k / steps
                a, b, c3, e = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
                cur.append((a * x + b * p1[0] + c3 * p2[0] + e * p3[0], a * y + b * p1[1] + c3 * p2[1] + e * p3[1]))
            x, y = p3
    if cur:
        polys.append(cur)
    return polys


def tw(s, h, bold=False):
    return len(s) * h * CW * (1.08 if bold else 1.0) + STROKE


def overlap(a, b, m=0.0):
    return a[0] < b[2] + m and b[0] < a[2] + m and a[1] < b[3] + m and b[1] < a[3] + m


def main():
    path, audit_path = sys.argv[1], sys.argv[2]
    b = parse(open(path, encoding="utf-8").read())
    mine = {U("logo", k) for k in range(500)} | {U("line", t) for t, _, _ in LINES}
    for n in [n for n in b if isinstance(n, list) and n and n[0] in ("gr_poly", "gr_text")]:
        u = find(n, "uuid")
        if u and uq(u[1]) in mine:
            b.remove(n)
    # the bottom name as on rev1, with the ø
    for n in findall(b, "gr_text"):
        if uq(n[1]) == "2026-11 Kenneth Foli Jorgensen":
            n[1] = q("2026-11 Kenneth Foli Jørgensen")

    audit = json.load(open(audit_path))
    audit["marks"] = [m for m in audit["marks"] if not str(m["id"]).startswith("identity:")]
    obst = [o["box"] for o in audit["obstacles"] if o["layer"] == "F.SilkS"]
    obst += [m["box"] for m in audit["marks"] if m["layer"] == "F.SilkS"]
    obst.append(MAINS)
    # every footprint's outline (fab, silk, courtyard): plug-in modules like U2 cover the board above
    import math
    for fp in findall(b, "footprint"):
        a = find(fp, "at")
        X, Y, r = float(a[1]), float(a[2]), math.radians(float(a[3]) if len(a) > 3 else 0.0)
        gx, gy = [], []
        for g in fp:
            if isinstance(g, list) and g and g[0] in ("fp_line", "fp_rect", "fp_poly", "fp_circle", "fp_arc"):
                lay = find(g, "layer")
                if not lay or not any(k in uq(lay[1]) for k in ("Fab", "SilkS", "CrtYd")):
                    continue
                pts = [find(g, k) for k in ("start", "end", "center", "mid")]
                pts = [(float(n[1]), float(n[2])) for n in pts if n]
                if find(g, "pts"):
                    pts += [(float(xy[1]), float(xy[2])) for xy in findall(find(g, "pts"), "xy")]
                for x, y in pts:
                    gx.append(X + x * math.cos(r) + y * math.sin(r))
                    gy.append(Y - x * math.sin(r) + y * math.cos(r))
        if gx:
            obst.append((min(gx), min(gy), max(gx), max(gy)))

    polys = [p for d in re.findall(r'\sd="([^"]+)"', open(LOGO, encoding="utf-8").read()) for p in svg_polys(d)]
    xs = [x for p in polys for x, _ in p]
    ys = [y for p in polys for _, y in p]
    s = LOGO_W / (max(xs) - min(xs))
    logo_h = (max(ys) - min(ys)) * s
    gap = 1.0
    text_w = max(tw(t, h, bd) for t, h, bd in LINES)
    text_h = sum(h + STROKE for _, h, _ in LINES) + 0.5
    bw, bh = LOGO_W + gap + text_w, max(logo_h, text_h)

    spot = None
    # prefer the lower half of the logic side, left to right
    ys_ = [OY + HB - EDGE - bh - 0.25 * k for k in range(int((HB - 2 * EDGE - bh) / 0.25))]
    xs_ = [OX + EDGE + 0.25 * k for k in range(int((W - 2 * EDGE - bw) / 0.25))]
    for y0 in ys_:
        for x0 in xs_:
            box = (x0, y0, x0 + bw, y0 + bh)
            if not any(overlap(box, o, CLR) for o in obst):
                spot = box
                break
        if spot:
            break
    if not spot:
        sys.exit("ERROR: no free spot on F.SilkS for the identity block")
    x0, y0 = spot[0], spot[1]
    lcx, lcy = x0 + LOGO_W / 2, y0 + bh / 2
    mx, my = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    for k, p in enumerate(polys):
        pts = " ".join(f"(xy {lcx + (px - mx) * s:.4f} {lcy + (py - my) * s:.4f})" for px, py in p)
        b.append(parse(f'(gr_poly (pts {pts}) (stroke (width 0) (type solid)) (fill yes) (layer "F.SilkS") (uuid {q(U("logo", k))}))'))
    audit["marks"].append({"id": "identity:logo", "layer": "F.SilkS", "text": "RF logo",
                           "box": [round(v, 3) for v in (x0, lcy - logo_h / 2, x0 + LOGO_W, lcy + logo_h / 2)]})
    tx = x0 + LOGO_W + gap
    ty = y0 + (bh - text_h) / 2
    for t, h, bold in LINES:
        th = 0.2 if bold else STROKE
        cy = ty + (h + th) / 2
        w = tw(t, h, bold)
        b.append(parse(f'(gr_text {q(t)} (at {tx:.3f} {cy:.3f} 0) (layer "F.SilkS") (uuid {q(U("line", t))}) '
                       f'(effects (font (size {h} {h}) (thickness {0.2 if bold else STROKE}){" (bold yes)" if bold else ""}) (justify left)))'))
        audit["marks"].append({"id": f"identity:{t}", "layer": "F.SilkS", "text": t,
                               "box": [round(v, 3) for v in (tx, cy - (h + th) / 2, tx + w, cy + (h + th) / 2)]})
        ty += h + th + 0.5
    for m in audit["marks"]:
        if m.get("text") == "2026-11 Kenneth Foli Jorgensen":
            m["text"] = "2026-11 Kenneth Foli Jørgensen"
    open(path, "w", encoding="utf-8").write(dump(b) + "\n")
    json.dump(audit, open(audit_path, "w"), indent=0)
    print(f"OK: logo {LOGO_W:.1f} x {logo_h:.1f} mm and {len(LINES)} lines at ({x0 - OX:.2f}, {y0 - OY:.2f}) mm "
          f"from the board corner, block {bw:.1f} x {bh:.1f} mm")


if __name__ == "__main__":
    main()
