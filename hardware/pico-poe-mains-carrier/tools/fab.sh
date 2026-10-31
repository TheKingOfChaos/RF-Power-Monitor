#!/bin/bash
# Export JLCPCB-ready fabrication outputs into ../fab
set -e
P=$(cd "$(dirname "$0")/.." && pwd)
N=pico-poe-mains-carrier
F=$P/fab
# replace only the generated files; fab/README.md stays
mkdir -p "$F" && cd "$F"
rm -rf gerber pico-poe-mains-carrier-gerbers.zip pico-poe-mains-carrier-bom.csv pico-poe-mains-carrier-cpl.csv \
  pico-poe-mains-carrier-schematic.pdf
mkdir -p gerber
cd "$P"
kicad-cli pcb export gerbers --layers "F.Cu,B.Cu,F.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts" --subtract-soldermask -o "$F/gerber/" $N.kicad_pcb >/dev/null
kicad-cli pcb export drill --format excellon --excellon-separate-th --generate-map --map-format gerberx2 -o "$F/gerber/" $N.kicad_pcb >/dev/null
(cd "$F/gerber" && zip -q -r "../${N}-gerbers.zip" .)
# JLCPCB needs every designator listed (R1,R2,R3,R4), not ranges (R1-R4)
kicad-cli sch export bom --fields "Reference,Value,Footprint,LCSC,MPN,\${QUANTITY}" --labels "Designator,Comment,Footprint,LCSC Part #,MPN,Quantity" \
  --group-by "Value,Footprint,LCSC,MPN" --ref-delimiter "," --ref-range-delimiter "" --exclude-dnp -o "$F/${N}-bom.csv" $N.kicad_sch >/dev/null
# the module is plugged in by hand, not assembled
python3 - "$F/${N}-bom.csv" <<'PY'
import sys
p = sys.argv[1]
rows = [l for l in open(p) if not l.startswith('"U1"')]
open(p, "w").writelines(rows)
PY
kicad-cli pcb export pos --side front --format csv --units mm --exclude-dnp -o /tmp/$N-pos.csv $N.kicad_pcb >/dev/null
python3 - "$F/${N}-cpl.csv" /tmp/$N-pos.csv <<'PY'
import csv, sys
out, src = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(src)))
with open(out, "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
    for r in rows:
        if r["Ref"].startswith("H") or r["Ref"] == "U1":  # U1 is the plug-in module
            continue
        w.writerow([r["Ref"], r["PosX"] + "mm", r["PosY"] + "mm", "Top", r["Rot"]])
PY
kicad-cli sch export pdf -o "$F/${N}-schematic.pdf" $N.kicad_sch >/dev/null
ls -la "$F"
