"""Rev2 change 2026-11-03: RJ45 J2 out over the board edge for the case, 3D model fixes.

- J2 (Kinghelm KH-RJ45-56-8P8C-D) moves DX mm towards the left edge, as far as its 3.2 mm
  board-lock holes allow with MIN_WEB mm of board left beside them (jack front ~3.1 mm out).
- J2's tracks and vias in the jack area are removed; the AI Flow router reconnects them.
- 3D models: J2's LCSC model sat 3.35 mm behind its footprint (pegs beside their holes);
  J1's LCSC USB-C model faced backwards and sat 1.05 mm inboard. Corrected in place.
Usage: rj45_overhang.py <board.kicad_pcb>   (KiCad closed; zones are refilled)
"""
import sys
import pcbnew

EDGE_X, PEG_R, PEG_Y_LOCAL, MIN_WEB = 100.0, 1.6, 5.83, 0.5
NEW_X = EDGE_X + MIN_WEB + PEG_R + PEG_Y_LOCAL          # 107.93
BOX = (EDGE_X, 102.2, 117.2, 121.5)                      # jack area, old and new

b = pcbnew.LoadBoard(sys.argv[1])
mm, MM = pcbnew.ToMM, pcbnew.FromMM
j2 = b.FindFootprintByReference('J2')
old = j2.GetPosition()
dx = NEW_X - mm(old.x)
nets = {p.GetNetname() for p in j2.Pads() if p.GetNetname()}

def inside(p):
    return BOX[0] <= mm(p.x) <= BOX[2] and BOX[1] <= mm(p.y) <= BOX[3]

ALL = list(b.GetTracks())   # one snapshot: GetTracks() fails once the board is edited
gone = []
for t in ALL:
    if t.GetNetname() not in nets:
        continue
    pts = [t.GetPosition()] if t.Type() == pcbnew.PCB_VIA_T else [t.GetStart(), t.GetEnd()]
    if any(inside(p) for p in pts):
        gone.append(t)
for t in gone:
    b.Remove(t)
j2.SetPosition(pcbnew.VECTOR2I(MM(NEW_X), old.y))

# Leftover pieces of J2's nets that now touch no pad (cut off by the removal above) go too.
def islands(net):
    items = [t for t in ALL if t.GetNetname() == net and not any(t is g for g in gone)]
    par = {}
    def f(k):
        while par.setdefault(k, k) != k:
            k = par[k]
        return k
    for t in items:
        if t.Type() == pcbnew.PCB_VIA_T:
            f((t.GetPosition().x, t.GetPosition().y))
        else:
            par[f((t.GetStart().x, t.GetStart().y))] = f((t.GetEnd().x, t.GetEnd().y))
    groups = {}
    for t in items:
        p = t.GetPosition() if t.Type() == pcbnew.PCB_VIA_T else t.GetStart()
        groups.setdefault(f((p.x, p.y)), []).append(t)
    return groups.values()
pads_of = {}
for fp in b.GetFootprints():
    for pd in fp.Pads():
        pads_of.setdefault(pd.GetNetname(), []).append(pd)
orphans = 0
for n in sorted(nets):
    for grp in islands(n):
        pts = [t.GetPosition()] if False else []
        for t in grp:
            pts += [t.GetPosition()] if t.Type() == pcbnew.PCB_VIA_T else [t.GetStart(), t.GetEnd()]
        if not any(pd.HitTest(p) for pd in pads_of.get(n, []) for p in pts):
            for t in grp:
                b.Remove(t)
                gone.append(t)
                orphans += 1

# C10 (CHASSIS-GND 2 kV cap above the jack): its CHASSIS pad would sit 1.17 mm from J2's
# LED_LINK_A pin (PoE to logic needs 2 mm), so it moves C10_DX mm left; the GND track is
# stretched to the new pad, the CHASSIS stub is left to the router.
C10_DX = -2.1
c10 = b.FindFootprintByReference('C10')
if abs(mm(c10.GetPosition().x) - 112.0) < 1e-3:
    pads = {p.GetNetname(): p.GetPosition() for p in c10.Pads()}
    shift = pcbnew.VECTOR2I(MM(C10_DX), 0)
    for t in [t for t in ALL if not any(t is g for g in gone)]:
        n = t.GetNetname()
        if n not in pads:
            continue
        at = pads[n]
        if t.Type() == pcbnew.PCB_VIA_T:
            continue
        for get, put in ((t.GetStart, t.SetStart), (t.GetEnd, t.SetEnd)):
            if get() == at:
                if n == 'GND':
                    put(at + shift)
                else:
                    b.Remove(t)
                    break
    c10.SetPosition(c10.GetPosition() + shift)

pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(sys.argv[1])

# FOOTPRINT.Models() hands Python copies, so the 3D model fields are set in the saved text
import re
MODELS = {'RJ45-TH_KH-RJ45-56-8P8C-D.step': ('0 -3.35 0', '0 0 0'),
          'USB-C_SMD-TYPE-C-31-M-12_1.step': ('0 -1.05 0', '0 0 180')}
txt = open(sys.argv[1], encoding='utf-8').read()
for name, (off, rot) in MODELS.items():
    pat = re.compile(r'(\(model "[^"]*/' + re.escape(name) + r'"\s*\(offset\s*\(xyz )[^)]*'
                     r'(\)\s*\)\s*\(scale\s*\(xyz [^)]*\)\s*\)\s*\(rotate\s*\(xyz )[^)]*(\))')
    txt, n = pat.subn(lambda m: m.group(1) + off + m.group(2) + rot + m.group(3), txt)
    assert n == 1, (name, n)
open(sys.argv[1], 'w', encoding='utf-8').write(txt)
print(f"OK: J2 moved {dx:+.2f} mm to x={NEW_X:.2f}; removed {len(gone)} track/via items on {len(nets)} nets "
      f"({orphans} in cut-off islands); "
      f"3D models of J1 and J2 corrected; zones refilled")
