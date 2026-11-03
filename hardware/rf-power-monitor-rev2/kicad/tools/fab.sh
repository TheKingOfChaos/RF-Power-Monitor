#!/usr/bin/env bash
# Manufacturing files for RF-Power-Monitor rev2 into out/fab, from the saved KiCad board.
# Re-run after any change to the board (save it in KiCad first).
# usage: bash kicad/tools/fab.sh        (from hardware/rf-power-monitor-rev2; needs kicad-cli and zip)
set -euo pipefail
cd "$(dirname "$0")/../.."
B=kicad/rf-power-monitor-rev2.kicad_pcb
N=rf-power-monitor-rev2
O=out/fab
rm -rf "$O/gerber" && mkdir -p "$O/gerber"

kicad-cli pcb export gerbers --layers F.Cu,B.Cu,F.Paste,B.Paste,F.SilkS,B.SilkS,F.Mask,B.Mask,Edge.Cuts \
  --subtract-soldermask -o "$O/gerber/" "$B" >/dev/null
kicad-cli pcb export drill --excellon-separate-th --excellon-units mm -o "$O/gerber/" "$B" >/dev/null
rm -f "$O/$N-gerbers.zip" && (cd "$O/gerber" && zip -q -j "../$N-gerbers.zip" ./*)

# JLCPCB BOM and CPL: LCSC numbers from design/bom.csv, positions from KiCad
T=$(mktemp -d)
kicad-cli pcb export pos --format csv --units mm --side both -o "$T/cpl.csv" "$B" >/dev/null
python3 kicad/tools/jlcpcb_files.py design/bom.csv "$T/cpl.csv" "$T" >/dev/null
cp "$T/jlcpcb-bom.csv" "$O/$N-bom.csv"
cp "$T/jlcpcb-cpl.csv" "$O/$N-cpl.csv"
rm -rf "$T"

cp design/$N-schematic.pdf "$O/"
[ -f out/3d/$N.step ] && cp out/3d/$N.step "$O/"
# The STEP is about 60 MB; git keeps it zipped (about 10 MB)
[ -f out/3d/$N.step ] && rm -f out/3d/$N-step.zip && (cd out/3d && zip -q -9 $N-step.zip $N.step)

echo "OK: $O: $(ls "$O/gerber" | wc -l) Gerber/drill files, $(($(wc -l < "$O/$N-bom.csv") - 1)) BOM lines, $(($(wc -l < "$O/$N-cpl.csv") - 1)) placements"
