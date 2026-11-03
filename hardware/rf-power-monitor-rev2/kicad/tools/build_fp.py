#!/usr/bin/env python3
"""Project footprints (lib/RFPM_Rev2.pretty) and 3D models (lib/3d).

- WIZPoE-P1: drawn here from the WIZnet WIZPoE-P1 datasheet (fig. 1-1/1-2: 38 x 16 mm, two 1x4
  2.54 mm headers 34.67 mm apart, centred across the width; pin 1 of both at the same edge) and the
  W5500-EVB-Pico-PoE V1.1 pick-and-place (P3 x 20.375, P2 x 55.044, same rotation).
- TestPoint_Pad_D0.5mm: 0.5 mm round SMD copper, mask opening, no paste, 1.1 mm courtyard (adom probing spec).
- RJ45 (C2828082), HR601680 (C242560), SWPA3015 (C43389), USB-C (C165948), TS-1187A (C318884),
  SCT1032 fuse (C19712559): from LCSC/EasyEDA via easyeda2kicad 1.0.1 (provenance.json).
"""
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
KDIR = os.path.dirname(HERE)
PRETTY = os.path.join(KDIR, "lib", "RFPM_Rev2.pretty")
D3 = os.path.join(KDIR, "lib", "3d")
EE = os.environ.get("EE_DIR", "")   # easyeda2kicad output (ee.pretty, ee.3dshapes)


def fp_wizpoe():
    pitch, span = 2.54, 34.67
    L = ['(footprint "WIZPoE-P1" (version 20260206) (generator "rfpm-rev2-gen") (layer "F.Cu")',
         ' (descr "WIZnet WIZPoE-P1 isolated 802.3af PoE module, 38 x 16 mm, two 1x4 2.54 mm headers 34.67 mm apart. '
         'Pads 1-4 = P1 VC1+, VC1-, VC2+, VC2- (input); pads 5-8 = P2 +5V, +5V, GND, GND (output). '
         'Solder the module directly (5 mm pins) or use 1x4 sockets.")',
         ' (tags "PoE WIZnet WIZPoE")',
         ' (property "Reference" "U2" (at 0 -10.5 0) (layer "F.SilkS") (effects (font (size 1 1) (thickness 0.15))))',
         ' (property "Value" "WIZPoE-P1" (at 0 0 0) (layer "F.Fab") (effects (font (size 1 1) (thickness 0.15))))',
         ' (property "Footprint" "" (at 0 0 0) (layer "F.Fab") (hide yes) (effects (font (size 1.27 1.27))))',
         ' (property "Datasheet" "https://docs.wiznet.io/Product/Modules/PoE/WIZPoE-P1" (at 0 0 0) (layer "F.Fab") (hide yes) (effects (font (size 1.27 1.27))))',
         ' (attr through_hole exclude_from_pos_files exclude_from_bom)']
    # origin: module centre. Input header P1 on the right (x +span/2), output P2 on the left (x -span/2),
    # as in the datasheet top view; pin 1 of both at the bottom edge (y +).
    hx = span / 2
    for i in range(4):
        y = round(1.5 * pitch - i * pitch, 4)
        shape = "rect" if i == 0 else "circle"
        L.append(f' (pad "{i + 1}" thru_hole {shape} (at {hx} {y}) (size 1.7 1.7) (drill 1.0) (layers "*.Cu" "*.Mask"))')
        L.append(f' (pad "{i + 5}" thru_hole {shape} (at {-hx} {y}) (size 1.7 1.7) (drill 1.0) (layers "*.Cu" "*.Mask"))')
    w, h = 38.0, 16.0
    x0, y0 = -w / 2, -h / 2
    # module edge: header row 1.67 mm from the module end (datasheet), so x edges at +-(hx + 1.67)
    xe = hx + 1.67
    for lay, off, wd in (("F.Fab", 0, 0.1), ("F.SilkS", 0.12, 0.12)):
        a, b = xe + off, h / 2 + off
        for p, q2 in (((-a, -b), (a, -b)), ((a, -b), (a, b)), ((a, b), (-a, b)), ((-a, b), (-a, -b))):
            L.append(f' (fp_line (start {p[0]} {p[1]}) (end {q2[0]} {q2[1]}) (stroke (width {wd}) (type default)) (layer "{lay}"))')
    cy = 8.0 + 0.5
    L.append(f' (fp_rect (start {-xe - 0.5} {-cy}) (end {xe + 0.5} {cy}) (stroke (width 0.05) (type default)) (fill no) (layer "F.CrtYd"))')
    L.append(f' (fp_text user "IN (VC)" (at {hx - 4.2} 0 90) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))')
    L.append(f' (fp_text user "OUT 5V" (at {-hx + 4.2} 0 90) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))')
    L.append(f' (fp_text user "WIZPoE-P1: components up" (at 0 6.5 0) (layer "F.SilkS") (effects (font (size 0.8 0.8) (thickness 0.12))))')
    L.append(')')
    return "\n".join(L) + "\n"


def fp_tp():
    return ('(footprint "TestPoint_Pad_D0.5mm" (version 20260206) (generator "rfpm-rev2-gen") (layer "F.Cu")\n'
            ' (descr "Test point: 0.5 mm round SMD copper, no paste (Adom probing workcell default)")\n'
            ' (tags "test point probe")\n'
            ' (property "Reference" "TP" (at 0 -1.2 0) (layer "F.SilkS") (hide yes) (effects (font (size 0.6 0.6) (thickness 0.1))))\n'
            ' (property "Value" "TP_0.5mm" (at 0 1.2 0) (layer "F.Fab") (effects (font (size 0.6 0.6) (thickness 0.1))))\n'
            ' (property "Footprint" "" (at 0 0 0) (layer "F.Fab") (hide yes) (effects (font (size 1.27 1.27))))\n'
            ' (property "Datasheet" "" (at 0 0 0) (layer "F.Fab") (hide yes) (effects (font (size 1.27 1.27))))\n'
            ' (attr smd exclude_from_pos_files exclude_from_bom)\n'
            ' (pad "1" smd circle (at 0 0) (size 0.5 0.5) (layers "F.Cu" "F.Mask"))\n'
            ' (fp_circle (center 0 0) (end 0.55 0) (stroke (width 0.05) (type default)) (fill no) (layer "F.CrtYd"))\n'
            ' (fp_circle (center 0 0) (end 0.25 0) (stroke (width 0.1) (type default)) (fill no) (layer "F.Fab"))\n'
            ')\n')


EE_PARTS = {  # easyeda footprint name -> (our footprint name, model base name)
    "RJ45-TH_KH-RJ45-56-8P8C-D": "RJ45-TH_KH-RJ45-56-8P8C-D",
    "SMD-16_L12.7-W7.2-P1.27-LS9.5-BL": "SMD-16_L12.7-W7.2-P1.27-LS9.5-BL",
    "IND-SMD_L3.0-W3.0": "IND-SMD_L3.0-W3.0",
}


def main():
    os.makedirs(PRETTY, exist_ok=True)
    os.makedirs(D3, exist_ok=True)
    open(os.path.join(PRETTY, "WIZPoE-P1.kicad_mod"), "w").write(fp_wizpoe())
    open(os.path.join(PRETTY, "TestPoint_Pad_D0.5mm.kicad_mod"), "w").write(fp_tp())
    if EE:
        for name in EE_PARTS:
            src = os.path.join(EE, "ee.pretty", name + ".kicad_mod")
            t = open(src).read()
            # model path: portable, inside the project
            import re
            t = re.sub(r'\(model "[^"]*?([^/\\"]+)\.wrl"', r'(model "${KIPRJMOD}/lib/3d/\1.step"', t)
            t = re.sub(r'\(model "[^"]*?([^/\\"]+)\.step"', r'(model "${KIPRJMOD}/lib/3d/\1.step"', t)
            open(os.path.join(PRETTY, name + ".kicad_mod"), "w").write(t)
        for fn in os.listdir(os.path.join(EE, "ee.3dshapes")):
            if fn.endswith(".step"):
                shutil.copy(os.path.join(EE, "ee.3dshapes", fn), os.path.join(D3, fn))
    print("footprints:", sorted(os.listdir(PRETTY)))
    print("models:", sorted(os.listdir(D3)))


if __name__ == "__main__":
    main()
