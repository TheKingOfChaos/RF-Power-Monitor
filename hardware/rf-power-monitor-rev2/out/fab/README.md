# Manufacturing files

These files are generated from the KiCad board by `kicad/tools/fab.sh`. Save the board in KiCad and re-run the script after any change.

| File | Use |
|---|---|
| `rf-power-monitor-rev2-gerbers.zip` | Gerbers and Excellon drill files (the same files are in `gerber/`). Upload this to JLCPCB: 2 layers, 1.6 mm, 1 oz, **black** solder mask, white silkscreen, **ENIG** surface finish. |
| `rf-power-monitor-rev2-bom.csv` | BOM in JLCPCB format with LCSC part numbers. U2 (WIZnet WIZPoE-P1 PoE module) and J3 (SWD header) are not assembled. |
| `rf-power-monitor-rev2-cpl.csv` | Component placement list for JLCPCB assembly. Check the parts' rotation in JLCPCB's preview, especially J1 (USB-C) and J2 (RJ45). |
| `rf-power-monitor-rev2-schematic.pdf` | Schematic as PDF. |
| `rf-power-monitor-rev2.step` | 3D model of the assembled board (without the WIZPoE-P1 module). It is about 60 MB, so git only has the zipped copy, `../3d/rf-power-monitor-rev2-step.zip`. |

The RJ45 (J2) sticks out 3.1 mm past the left board edge, so it can pass through the case wall. Its pads and holes are all inside the board.

The 3D model for the web (GLB) is in `../3d/`, and pictures of the board are in `../img/`.
