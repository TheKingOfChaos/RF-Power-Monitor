"""Pin-1 dots on the silkscreen for every chip and transformer (U<n>, T<n>): a filled 0.6 mm dot next to pad 1,
on the chip's side of the board, clear of pads, vias, holes, other silkscreen and the board edge.

Idempotent: dots have uuid5 IDs; existing ones are replaced. KiCad must be closed.
usage: pin1_dots.py <board.kicad_pcb>
"""
import math
import re
import sys
import uuid

import pcbnew

NS = uuid.UUID("2b9c6f1e-7d4a-4c3b-9a51-0e8d2f6c1a77")
R = 0.3            # dot radius (mm), 0.6 mm across
PAD_CLR = 0.25     # dot edge to pad copper/mask opening (JLCPCB silk-to-pad 0.15 + margin)
SILK_CLR = 0.15    # dot edge to other silkscreen
EDGE_CLR = 0.5

mm, MM = pcbnew.ToMM, pcbnew.FromMM
b = pcbnew.LoadBoard(sys.argv[1])
V = lambda x, y: pcbnew.VECTOR2I(MM(x), MM(y))

ids = {str(uuid.uuid5(NS, f.GetReference())) for f in b.GetFootprints()}
for d in list(b.GetDrawings()):
    if d.m_Uuid.AsString() in ids:
        b.Remove(d)

edge = b.GetBoardEdgesBoundingBox()
ex0, ey0, ex1, ey1 = mm(edge.GetLeft()), mm(edge.GetTop()), mm(edge.GetRight()), mm(edge.GetBottom())


def silk_items(layer):
    out = [d for d in b.GetDrawings() if d.GetLayer() == layer]
    for f in b.GetFootprints():
        out += [g for g in f.GraphicalItems() if g.GetLayer() == layer]
        for fld in (f.Reference(), f.Value()):
            if fld.GetLayer() == layer and fld.IsVisible():
                out.append(fld)
    return out


def free(x, y, layer, silk):
    p = V(x, y)
    if not (ex0 + EDGE_CLR + R <= x <= ex1 - EDGE_CLR - R and ey0 + EDGE_CLR + R <= y <= ey1 - EDGE_CLR - R):
        return False
    copper = pcbnew.F_Cu if layer == pcbnew.F_SilkS else pcbnew.B_Cu
    for f in b.GetFootprints():
        for pd in f.Pads():
            if pd.IsOnLayer(copper) or pd.GetAttribute() in (pcbnew.PAD_ATTRIB_PTH, pcbnew.PAD_ATTRIB_NPTH):
                if pd.HitTest(p, MM(R + PAD_CLR)):
                    return False
    for t in b.GetTracks():
        if t.Type() == pcbnew.PCB_VIA_T and t.HitTest(p, MM(R + PAD_CLR)):
            return False
    for s in silk:
        if s.GetClass() in ('PCB_TEXT', 'PCB_FIELD'):
            bb = s.GetBoundingBox()
            bb.Inflate(MM(R + SILK_CLR))
            if bb.Contains(p):
                return False
        elif s.HitTest(p, MM(R + SILK_CLR)):
            return False
    return True


placed, failed = [], []
for f in sorted(b.GetFootprints(), key=lambda f: f.GetReference()):
    ref = f.GetReference()
    if not re.match(r'^(U|T)\d', ref):          # chips and transformers, not test points
        continue
    p1 = [p for p in f.Pads() if p.GetNumber() == '1']
    if not p1:
        continue
    p1 = p1[0]
    layer = pcbnew.B_SilkS if f.IsFlipped() else pcbnew.F_SilkS
    silk = silk_items(layer)
    px, py = mm(p1.GetPosition().x), mm(p1.GetPosition().y)
    pads = list(f.Pads())
    cx = sum(mm(p.GetPosition().x) for p in pads) / len(pads)
    cy = sum(mm(p.GetPosition().y) for p in pads) / len(pads)
    # the chip body (F.Fab outline): a dot under the body is hidden once the part is fitted
    fab = pcbnew.B_Fab if f.IsFlipped() else pcbnew.F_Fab
    xs, ys = [], []
    for g in f.GraphicalItems():
        if g.GetLayer() == fab and g.GetClass() == 'PCB_SHAPE':
            gb = g.GetBoundingBox()
            xs += [mm(gb.GetLeft()), mm(gb.GetRight())]
            ys += [mm(gb.GetTop()), mm(gb.GetBottom())]
    body = (min(xs), min(ys), max(xs), max(ys)) if xs else None
    if body and (body[2] - body[0]) * (body[3] - body[1]) > 400:
        body = None   # a plug-in module (U2): its pin-1 mark goes by the socket, inside the outline
    if body:
        k = R + 0.05
        body = (body[0] - k, body[1] - k, body[2] + k, body[3] + k)
    bb = p1.GetBoundingBox()
    hw, hh = mm(bb.GetWidth()) / 2, mm(bb.GetHeight()) / 2
    best = None
    # candidates on rings around pad 1, preferring the side away from the chip centre, then closeness
    for ring in [k * 0.1 for k in range(0, 16)]:
        for a in range(0, 360, 10):
            t = math.radians(a)
            ux, uy = math.cos(t), math.sin(t)
            # distance from pad centre to its edge along (ux, uy), for a box pad
            edge_d = min(hw / abs(ux) if abs(ux) > 1e-9 else 1e9, hh / abs(uy) if abs(uy) > 1e-9 else 1e9)
            d = edge_d + PAD_CLR + R + ring
            x, y = px + ux * d, py + uy * d
            away = (x - cx) * (px - cx) + (y - cy) * (py - cy)
            score = (ring, -away)
            if best and score >= best[0]:
                continue
            if body and body[0] <= x <= body[2] and body[1] <= y <= body[3]:
                continue
            if free(x, y, layer, silk):
                best = (score, x, y)
        if best:
            break
    if not best:
        failed.append(ref)
        continue
    _, x, y = best
    dot = pcbnew.PCB_SHAPE(b)
    dot.SetShape(pcbnew.SHAPE_T_CIRCLE)
    dot.SetLayer(layer)
    dot.SetCenter(V(x, y))
    dot.SetEnd(V(x + R - 0.05, y))
    dot.SetFilled(True)
    dot.SetWidth(MM(0.1))
    dot.SetUuid(pcbnew.KIID(str(uuid.uuid5(NS, ref)))) if hasattr(dot, 'SetUuid') else None
    b.Add(dot)
    if dot.m_Uuid.AsString() != str(uuid.uuid5(NS, ref)):
        dot.m_Uuid = pcbnew.KIID(str(uuid.uuid5(NS, ref)))
    placed.append(f"{ref} ({x:.2f}, {y:.2f})")

b.Save(sys.argv[1])
print(f"OK: pin-1 dots for {len(placed)} chips: " + ", ".join(placed))
if failed:
    print("ERROR: no free spot next to pin 1 for: " + ", ".join(failed))
    sys.exit(1)
