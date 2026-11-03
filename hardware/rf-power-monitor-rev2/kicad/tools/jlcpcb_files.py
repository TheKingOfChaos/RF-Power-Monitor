#!/usr/bin/env python3
"""JLCPCB assembly files from the AI Flow export: BOM (Comment, Designator, Footprint, LCSC Part #)
from design/bom.csv (the single source, with LCSC numbers) restricted to the parts in the exported CPL,
and CPL (Designator, Mid X, Mid Y, Layer, Rotation) from kicad-cli's CPL. Parts without an LCSC number
(J3 SWD header, not fitted; consigned parts) are listed on stdout.

usage: jlcpcb_files.py <design/bom.csv> <export cpl.csv> <out dir>
"""
import csv, sys, os
from collections import OrderedDict

bom, cpl, out = sys.argv[1:4]
placed = {r["Ref"]: r for r in csv.DictReader(open(cpl))}
rows = OrderedDict()
missing = []
for r in csv.DictReader(open(bom)):
    if r["ref"] not in placed:
        continue
    if not r["lcsc"].strip():
        missing.append(f'{r["ref"]} {r["value"]} {r["mpn"]}')
        continue
    k = (r["value"], r["package"], r["lcsc"])
    rows.setdefault(k, []).append(r["ref"])
os.makedirs(out, exist_ok=True)
with open(os.path.join(out, "jlcpcb-bom.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #"])
    for (val, pkg, lcsc), refs in rows.items():
        w.writerow([val, ",".join(refs), pkg, lcsc])
fitted = {ref for refs in rows.values() for ref in refs}
with open(os.path.join(out, "jlcpcb-cpl.csv"), "w", newline="") as f:
    w = csv.writer(f); w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for ref, r in placed.items():
        if ref in fitted:
            w.writerow([ref, f'{float(r["PosX"]):.4f}mm', f'{float(r["PosY"]):.4f}mm', "Top" if r["Side"] == "top" else "Bottom", f'{float(r["Rot"]):.1f}'])
not_in_bom = sorted(set(placed) - fitted - {m.split()[0] for m in missing})
print(f"{len(rows)} BOM lines, {len(fitted)} placed parts with LCSC numbers")
print("no LCSC number (not ordered for assembly):", "; ".join(missing) or "none")
print("in CPL but not in design/bom.csv:", ", ".join(not_in_bom) or "none")
