#!/usr/bin/env python3
"""Generate the KiCad 10 project for the W5500-EVB-Pico-PoE mains-monitor carrier.

Writes schematic, board, project, DRC rules and the project-local symbol and
footprint libraries. Stock footprints are read from ../tools/stock_fp (copied
from the KiCad 10.0 install) and embedded into the board.

Coordinates in this file are "plan" coordinates in mm: x=0 is the end of the
module PCB on the RJ45 side, y=0 is the top board edge. The board is exactly as
long as the module (x=0..80), so the RJ45 overhangs the left edge like it does
on the module itself.
"""
import math
import os
import re
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.dirname(HERE)
STOCK = os.path.join(HERE, "stock_fp")
NAME = "pico-poe-mains-carrier"
LIB = "PicoPoE_Carrier"

# plan -> KiCad page coordinates
DX, OX, OY = 0.0, 40.0, 40.0
BOARD_W, BOARD_H = 80.0, 50.0


def U():
    return str(uuid.uuid4())


def P(x, y):
    """plan -> page coordinates (board)"""
    return round(x + DX + OX, 4), round(y + OY, 4)


def B(x, y):
    """board-local coordinates (0,0 = board top-left) -> page coordinates"""
    return round(x + OX, 4), round(y + OY, 4)


def f(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


# ---------------------------------------------------------------------------
# Module pinout (W5500-EVB-Pico-PoE == Raspberry Pi Pico pinout)
# ---------------------------------------------------------------------------
PICO_PINS = {
    1: "GP0", 2: "GP1", 3: "GND", 4: "GP2", 5: "GP3", 6: "GP4", 7: "GP5", 8: "GND",
    9: "GP6", 10: "GP7", 11: "GP8", 12: "GP9", 13: "GND", 14: "GP10", 15: "GP11",
    16: "GP12", 17: "GP13", 18: "GND", 19: "GP14", 20: "GP15",
    21: "GP16_MISO", 22: "GP17_CS", 23: "GND", 24: "GP18_SCK", 25: "GP19_MOSI",
    26: "GP20_RST", 27: "GP21_INT", 28: "GND", 29: "GP22", 30: "RUN", 31: "GP26",
    32: "GP27", 33: "AGND", 34: "GP28", 35: "ADC_VREF", 36: "3V3", 37: "3V3_EN",
    38: "GND", 39: "VSYS", 40: "VBUS",
}
GND_PINS = [3, 8, 13, 18, 23, 28, 33, 38]
MODULE_NETS = {p: "GND" for p in GND_PINS}
MODULE_NETS.update({6: "I2C_SDA", 7: "I2C_SCL", 29: "FACTORY_RESET", 32: "MAINS_OK", 36: "+3V3"})


def pico_pad_xy(n):
    """pad position relative to the module's top-left corner (RJ45 end left)"""
    if n <= 20:
        return 75.63 - (n - 1) * 2.54, 1.61
    return 75.63 - (40 - n) * 2.54, 19.39


MOD_X, MOD_Y = 0.0, 0.0  # module top-left corner, plan coords (flush with the top edge)


def pico_pin_plan(n):
    x, y = pico_pad_xy(n)
    return round(MOD_X + x, 4), round(MOD_Y + y, 4)


# ---------------------------------------------------------------------------
# Layout: module on top; below it the low-voltage logic on the left and the
# mains section on the right with J1 at the USB end. MAINS_OK, +3V3 and I2C
# reach the left block in bottom-layer lanes between the two header rows.
# ---------------------------------------------------------------------------
MAINS_NETS = ["AC_L", "AC_N", "L_FUSED", "N_MID1", "N_MID2", "OPTO_A", "OPTO_K"]
YN, YL = 29.92, 35.0          # N and L rows of the divider
OPTO = (45.0, (YN + YL) / 2)   # optocoupler centre, rotated 180: inputs right, outputs left

PARTS = [
    # ref, symbol, value, footprint, x, y, rot, nets{pin: net}, props
    ("U1", "W5500-EVB-Pico-PoE", "W5500-EVB-Pico-PoE", f"{LIB}:W5500-EVB-Pico-PoE_Socket",
     MOD_X, MOD_Y, 0, MODULE_NETS,
     {"MPN": "W5500-EVB-Pico-PoE", "Note": "Plug-in module, 2x 1x20 female header 2.54mm"}),
    ("J1", "Screw_Terminal_2", "230VAC L/N", "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2-5.08_1x02_P5.08mm_Horizontal",
     74.8, YL, 90, {"1": "AC_L", "2": "AC_N"}, {"MPN": "Phoenix 1715721 (MKDS 1,5/2-5,08)"}),
    # one series loop L -> F1 -> R1 -> opto -> R4 -> R3 -> R2 -> N: F1 carries any fault current
    # SETfuse SCT1032 (10.3 x 3.3 mm) on the UMT 250 footprint (10.1 x 3.1 mm); T1A is the smallest
    # stocked at JLCPCB, and the loop only carries ~1.2 mA, so it acts on hard faults either way
    ("F1", "Fuse", "T1A 250VAC", "Fuse:Fuse_Schurter_UMT250", 63.3, YL, 0,
     {"1": "L_FUSED", "2": "AC_L"}, {"LCSC": "C19712559", "MPN": "SETfuse SCT1032T1A250V (T 1 A, 250 VAC, 100 A breaking)"}),
    ("R1", "R", "68k 1206 200V", "Resistor_SMD:R_1206_3216Metric", 55.0, YL, 0,
     {"1": "OPTO_A", "2": "L_FUSED"}, {"LCSC": "C2889820"}),
    ("R2", "R", "68k 1206 200V", "Resistor_SMD:R_1206_3216Metric", 66.6, YN, 0,
     {"1": "N_MID1", "2": "AC_N"}, {"LCSC": "C2889820"}),
    ("R3", "R", "68k 1206 200V", "Resistor_SMD:R_1206_3216Metric", 60.9, YN, 0,
     {"1": "N_MID2", "2": "N_MID1"}, {"LCSC": "C2889820"}),
    ("R4", "R", "68k 1206 200V", "Resistor_SMD:R_1206_3216Metric", 55.0, YN, 0,
     {"1": "OPTO_K", "2": "N_MID2"}, {"LCSC": "C2889820"}),
    ("U2", "LTV-814S", "LTV-814S", "Package_DIP:SMDIP-4_W9.53mm", OPTO[0], OPTO[1], 180,
     {"1": "OPTO_A", "2": "OPTO_K", "3": "GND", "4": "SENSE"}, {"LCSC": "C125118", "MPN": "LTV-814S-TA1"}),
    ("C1", "C", "1uF", "Capacitor_SMD:C_0603_1608Metric", 36.8, 30.2, 90,
     {"1": "SENSE", "2": "GND"}, {"LCSC": "C15849"}),
    ("R5", "R", "47k", "Resistor_SMD:R_0603_1608Metric", 37.575, 36.9, 0,
     {"1": "SENSE", "2": "+3V3"}, {"LCSC": "C25819"}),
    ("U3", "74LVC1G14", "SN74LVC1G14DBVR", "Package_TO_SOT_SMD:SOT-23-5", 34.0, 34.3, 180,
     {"1": "unconnected-(U3-NC-Pad1)", "2": "SENSE", "3": "GND", "4": "MAINS_OK", "5": "+3V3"}, {"LCSC": "C7835"}),
    ("C3", "C", "100nF", "Capacitor_SMD:C_0603_1608Metric", 30.2, 35.25, 0,
     {"1": "GND", "2": "+3V3"}, {"LCSC": "C14663"}),
    ("R6", "R", "100R", "Resistor_SMD:R_0603_1608Metric", 24.0, 32.0, 90,
     {"1": "LED_G", "2": "MAINS_OK"}, {"LCSC": "C22775"}),
    ("D1", "LED", "Green", "LED_SMD:LED_0805_2012Metric", 24.0, 35.5, 90,
     {"1": "GND", "2": "LED_G"}, {"LCSC": "C2297"}),
    ("D2", "LED", "Red", "LED_SMD:LED_0805_2012Metric", 27.5, 32.4, 270,
     {"1": "MAINS_OK", "2": "LED_R"}, {"LCSC": "C84256"}),
    ("R7", "R", "330R", "Resistor_SMD:R_0603_1608Metric", 27.5, 36.0, 90,
     {"1": "+3V3", "2": "LED_R"}, {"LCSC": "C23138"}),
    ("U4", "TMP1075", "TMP1075DR", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", 14.0, 45.0, 270,
     {"1": "I2C_SDA", "2": "I2C_SCL", "3": "unconnected-(U4-ALERT-Pad3)", "4": "GND", "5": "GND", "6": "GND", "7": "GND", "8": "+3V3"},
     {"LCSC": "C2878381"}),
    ("C2", "C", "100nF", "Capacitor_SMD:C_0603_1608Metric", 18.6, 47.475, 0,
     {"1": "+3V3", "2": "GND"}, {"LCSC": "C14663"}),
    ("R8", "R", "4.7k", "Resistor_SMD:R_0603_1608Metric", 17.6, 40.2, 0,
     {"1": "I2C_SDA", "2": "+3V3"}, {"LCSC": "C23162"}),
    ("R9", "R", "4.7k", "Resistor_SMD:R_0603_1608Metric", 13.0, 39.0, 90,
     {"1": "I2C_SCL", "2": "+3V3"}, {"LCSC": "C23162"}),
    # factory reset: GP22 to GND, the firmware's internal pull-up holds it high; hold 10 s
    ("SW1", "SW_Push", "FACTORY RESET", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A", 11.5, 24.875, 0,
     {"1": "FACTORY_RESET", "2": "GND"}, {"LCSC": "C318884", "MPN": "XKB TS-1187A-B-A-B"}),
]
REF_POS = {"C3": (30.4, 36.75), "D2": (27.5, 28.9), "C2": (21.3, 47.475), "H1": (4.0, 29.6), "H3": (71.3, 46.0),
           "F1": (63.3, 37.9), "R2": (66.6, 32.0), "SW1": (11.5, 28.9),
           "U2": (42.3, 28.4)}  # U2: left of the slot, which hides the default spot
HOLES = [("H1", 4.0, 25.5), ("H2", 4.0, 46.0), ("H3", 76.0, 46.0),
         # under the module, between the header rows: reachable only with the module on headers
         ("H4", 30.0, 10.5), ("H5", 66.5, 10.5)]  # board coords
# these use a project footprint without courtyard (under_module_hole_text), so the courtyard check
# stays on for all real parts; the mounting holes are the board's only NPTHs, so "NPTH inside
# courtyard" is off in the project (see main())
UNDER_MODULE_HOLES = ("H4", "H5")
HOLE_UNDER_MODULE = "MountingHole_3.2mm_M3_UnderModule"


def hole_fpid(ref):
    return f"{LIB}:{HOLE_UNDER_MODULE}" if ref in UNDER_MODULE_HOLES else "MountingHole:MountingHole_3.2mm_M3"

# ---------------------------------------------------------------------------
# Routing (plan coords): (net, layer, width, [points])
# ---------------------------------------------------------------------------
WM, WS, WP = 0.5, 0.25, 0.4
X_SCL, X_SDA, X_3V3, X_OK = 14.635, 15.905, 18.375, 20.5   # lane drops into the left block
TRACKS = [
    # mains side: J1 -> F1 -> R1 -> optocoupler (L), J1 -> R2 -> R3 -> R4 -> optocoupler (N)
    ("AC_L", "F.Cu", WM, [(74.8, YL), (67.55, YL)]),
    ("L_FUSED", "F.Cu", WM, [(59.05, YL), (56.4625, YL)]),
    ("OPTO_A", "F.Cu", WM, [(53.5375, YL), (51.035, YL), (49.765, YL - 1.27)]),
    ("AC_N", "F.Cu", WM, [(74.8, YN), (68.0625, YN)]),
    ("N_MID1", "F.Cu", WM, [(65.1375, YN), (62.3625, YN)]),
    ("N_MID2", "F.Cu", WM, [(59.4375, YN), (56.4625, YN)]),
    ("OPTO_K", "F.Cu", WM, [(53.5375, YN), (51.035, YN), (49.765, YN + 1.27)]),
    # factory reset: GP22 (module pin 29, bottom row) out below the module, along its edge to SW1 next to H1
    # (on to SW1's second pin-1 pad; the GND pads join the pour)
    ("FACTORY_RESET", "F.Cu", WS, [pico_pin_plan(29), (47.69, 21.0), (45.69, 23.0), (8.5, 23.0)]),
    # opto output -> C1, R5 pull-up, Schmitt trigger input
    ("SENSE", "F.Cu", WS, [(40.235, 33.73), (36.8, 33.73)]),
    ("SENSE", "F.Cu", WS, [(36.8, 30.975), (36.8, 36.9)]),
    ("SENSE", "F.Cu", WS, [(36.8, 34.3), (35.1375, 34.3)]),
    # MAINS_OK: U3 -> LEDs -> lane to GP27
    ("MAINS_OK", "F.Cu", WS, [(32.8625, 33.35), (29.5125, 30.0), (X_OK, 30.0)]),
    ("MAINS_OK", "F.Cu", WS, [(24.0, 31.225), (24.0, 30.0)]),
    ("MAINS_OK", "F.Cu", WS, [(27.5, 31.4625), (27.5, 30.0)]),
    ("MAINS_OK", "B.Cu", WS, [(X_OK, 30.0), (X_OK, 25.6), (25.7, 25.6), (25.7, 17.6), (55.31, 17.6), pico_pin_plan(32)]),
    ("LED_G", "F.Cu", WS, [(24.0, 32.775), (24.0, 34.5625)]),
    ("LED_R", "F.Cu", WS, [(27.5, 33.3375), (27.5, 35.225)]),
    # +3V3 from module pin 36
    ("+3V3", "B.Cu", WP, [pico_pin_plan(36), (65.47, 16.4), (24.7, 16.4), (24.7, 24.6), (X_3V3, 24.6), (X_3V3, 38.5)]),
    ("+3V3", "F.Cu", WP, [(X_3V3, 38.5), (38.35, 38.5)]),
    ("+3V3", "F.Cu", WP, [(38.35, 38.5), (38.35, 36.9)]),
    ("+3V3", "F.Cu", WP, [(27.5, 38.5), (27.5, 36.775)]),
    ("+3V3", "F.Cu", WP, [(31.9, 38.5), (31.9, 35.25)]),
    ("+3V3", "F.Cu", WP, [(30.975, 35.25), (32.8625, 35.25)]),
    ("+3V3", "F.Cu", WS, [(X_3V3, 38.5), (X_3V3, 37.6), (13.0, 37.6), (13.0, 38.225)]),
    ("+3V3", "F.Cu", WP, [(X_3V3, 38.5), (X_3V3, 44.5), (17.825, 45.05), (17.825, 47.475)]),
    ("+3V3", "F.Cu", WS, [(15.905, 47.475), (17.825, 47.475)]),
    # I2C from GP4/GP5 (top header row) along lanes under the module to the sensor
    ("I2C_SCL", "B.Cu", WS, [pico_pin_plan(7), (60.39, 3.4), (22.7, 3.4), (22.7, 22.6), (X_SCL, 22.6), (X_SCL, 38.9)]),
    ("I2C_SCL", "F.Cu", WS, [(X_SCL, 38.9), (X_SCL, 42.525)]),
    ("I2C_SCL", "F.Cu", WS, [(13.0, 39.775), (X_SCL, 39.775)]),
    ("I2C_SDA", "B.Cu", WS, [pico_pin_plan(6), (62.93, 4.6), (23.7, 4.6), (23.7, 23.6), (X_SDA, 23.6), (X_SDA, 39.6)]),
    ("I2C_SDA", "F.Cu", WS, [(X_SDA, 39.6), (X_SDA, 42.525)]),
    ("I2C_SDA", "F.Cu", WS, [(16.825, 40.2), (X_SDA, 40.2)]),
    # TMP1075 address pins A0..A2 to GND (address 0x48)
    ("GND", "F.Cu", WS, [(12.095, 47.475), (14.635, 47.475)]),
]
VIAS = [
    ("+3V3", X_3V3, 38.5), ("MAINS_OK", X_OK, 30.0),
    ("I2C_SCL", X_SCL, 38.9), ("I2C_SDA", X_SDA, 39.6),
    # GND stitching
    ("GND", 35.0, 8.0), ("GND", 45.0, 12.0), ("GND", 50.0, 8.0), ("GND", 62.0, 13.0),
    ("GND", 70.0, 7.0), ("GND", 40.0, 13.0), ("GND", 70.0, 17.5),
    ("GND", 10.0, 30.0), ("GND", 30.0, 26.0), ("GND", 40.5, 36.5), ("GND", 34.0, 37.0),
    ("GND", 22.0, 33.5), ("GND", 10.0, 41.0), ("GND", 24.0, 44.0), ("GND", 35.0, 45.0),
    ("GND", 55.0, 45.0), ("GND", 68.0, 46.0),
]

# Mains area, plan coords (no pour, no vias)
MAINS_AREA = [(46.0, 22.5), (BOARD_W, 22.5), (BOARD_W, 42.0), (46.0, 42.0)]
SLOT = (OPTO[0], OPTO[1] - 7.6, OPTO[1] + 7.6, 1.2)  # vertical: centre x, y0, y1, width

# Clearance for parts that stick out under the module when it is soldered flat
# (positions from WIZnet's W5500-EVB-Pico-PoE V1.1 drill file):
TOP = -1.5                       # board edge above the module, room for the solder fillets
NOTCH_RJ45 = (21.9, 19.9)        # corner notch: RJ45 pins/posts/shield + PoE header 1 (x 20.4)
WIN_POE = (53.9, 5.6, 59.6, 15.4)  # window: PoE header 2 (x 55.0) + SWD header (x 58.5)
NOTCH_USB = (72.6, 5.5, 15.5)    # notch at the USB end: USB-C shell and its two tabs


# ---------------------------------------------------------------------------
# Footprint handling
# ---------------------------------------------------------------------------
def rot_pt(x, y, deg):
    a = math.radians(deg)
    return x * math.cos(a) + y * math.sin(a), -x * math.sin(a) + y * math.cos(a)


def module_footprint_text():
    """Project-local footprint for the module: 2x20 THT header + outline incl. RJ45 overhang."""
    L = []
    L.append(f'(footprint "W5500-EVB-Pico-PoE_Socket"\n\t(version 20260206)\n\t(generator "{NAME}-gen")\n\t(layer "F.Cu")')
    L.append('\t(descr "WIZnet W5500-EVB-Pico-PoE (80x21mm, Pico pinout). Either on two 1x20 2.54mm female headers (THT pads) or soldered flat by its castellated edges (SMD pads). Origin = module corner at the RJ45 end, pin 20 side.")')
    L.append('\t(tags "W5500 Pico PoE RP2040 socket")')
    L.append('\t(property "Reference" "REF**"\n\t\t(at 40 6.5 0)\n\t\t(layer "F.SilkS")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)')
    L.append('\t(property "Value" "W5500-EVB-Pico-PoE"\n\t\t(at 40 10.5 0)\n\t\t(layer "F.Fab")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)')
    L.append('\t(attr through_hole)')

    def line(x1, y1, x2, y2, layer, w):
        L.append(f'\t(fp_line\n\t\t(start {f(x1)} {f(y1)})\n\t\t(end {f(x2)} {f(y2)})\n\t\t(stroke\n\t\t\t(width {w})\n\t\t\t(type solid)\n\t\t)\n\t\t(layer "{layer}")\n\t)')

    def rect(x1, y1, x2, y2, layer, w):
        L.append(f'\t(fp_rect\n\t\t(start {f(x1)} {f(y1)})\n\t\t(end {f(x2)} {f(y2)})\n\t\t(stroke\n\t\t\t(width {w})\n\t\t\t(type solid)\n\t\t)\n\t\t(fill no)\n\t\t(layer "{layer}")\n\t)')

    def text(t, x, y, layer, size=1.0):
        L.append(f'\t(fp_text user "{t}"\n\t\t(at {f(x)} {f(y)} 0)\n\t\t(layer "{layer}")\n\t\t(effects\n\t\t\t(font\n\t\t\t\t(size {size} {size})\n\t\t\t\t(thickness {round(size * 0.15, 3)})\n\t\t\t)\n\t\t)\n\t)')

    rect(0, 0, 80, 21, "F.Fab", 0.1)
    rect(-4.8, 2.5, 17.2, 18.5, "F.Fab", 0.1)          # RJ45 body, overhangs the module end
    rect(-5.05, -0.25, 80.25, 21.25, "F.CrtYd", 0.05)
    # pin-1 marker
    line(77.0, 3.0, 78.3, 3.0, "F.SilkS", 0.12)
    text("<- RJ45", 38.5, 10.5, "F.SilkS", 1.0)
    text("USB ->", 47.5, 10.5, "F.SilkS", 1.0)
    text("${REFERENCE}", 40, 13.0, "F.Fab")
    for n in range(1, 41):
        x, y = pico_pad_xy(n)
        shape = "rect" if n == 1 else "circle"
        L.append(f'\t(pad "{n}" thru_hole {shape}\n\t\t(at {f(x)} {f(y)})\n\t\t(size 1.7 1.7)\n\t\t(drill 1.0)\n\t\t(layers "*.Cu" "*.Mask")\n\t\t(remove_unused_layers no)\n\t)')
        # castellation pad: reaches 1 mm past the module edge for the solder fillet
        cy = 0.5 if n <= 20 else 20.5
        L.append(f'\t(pad "{n}" smd rect\n\t\t(at {f(x)} {f(cy)})\n\t\t(size 1.6 3.0)\n\t\t(layers "F.Cu" "F.Mask")\n\t)')
    L.append('\t(embedded_fonts no)')
    # WIZnet's own model; shown soldered flat (its board underside is 0.62 mm below its origin)
    L.append('\t(model "${KIPRJMOD}/3d/W5500-EVB-Pico-PoE_V1.1.step"\n\t\t(offset\n\t\t\t(xyz 0 -21 0.62)\n\t\t)\n'
             '\t\t(scale\n\t\t\t(xyz 1 1 1)\n\t\t)\n\t\t(rotate\n\t\t\t(xyz 0 0 0)\n\t\t)\n\t)\n)')
    return "\n".join(L) + "\n"


AT_RE = re.compile(r"\(at (-?[\d.]+) (-?[\d.]+)(?: (-?[\d.]+))?\)")


def under_module_hole_text():
    """Stock M3 hole without courtyard, for holes that sit under the module (used with the module on headers)."""
    src = open(os.path.join(STOCK, "MountingHole_3.2mm_M3.kicad_mod")).read()
    src = re.sub(r'\n\t\(fp_\w+\n(?:\t\t[^\n]*\n)*?\t\t\(layer "F\.CrtYd"\)\n(?:\t\t[^\n]*\n)*?\t\)', "", src)
    src = src.replace('(footprint "MountingHole_3.2mm_M3"', f'(footprint "{HOLE_UNDER_MODULE}"', 1)
    src = re.sub(r'\(descr "[^"]*"\)', '(descr "Mounting Hole 3.2mm, M3, no annular, no courtyard: sits under the W5500-EVB-Pico-PoE '
                 'and is used with the module on headers (screw in before plugging the module in)")', src, count=1)
    src = src.replace("(attr exclude_from_pos_files exclude_from_bom)", "(attr exclude_from_pos_files exclude_from_bom allow_missing_courtyard)", 1)
    src = re.sub(r'\(generator "[^"]*"\)', f'(generator "{NAME}-gen")', src, count=1)
    return src


def load_fp(fpid):
    lib, name = fpid.split(":", 1)
    if lib == LIB:
        return under_module_hole_text() if name == HOLE_UNDER_MODULE else module_footprint_text()
    with open(os.path.join(STOCK, name + ".kicad_mod")) as fh:
        return fh.read()


def bnet(net):
    """schematic net name as KiCad derives it (local labels live in the root sheet)"""
    return net if net in ("GND", "+3V3") or net.startswith("unconnected-") else "/" + net


def board_footprint(fpid, ref, value, x, y, rot, nets, props, sym_uuid, mains, ref_pos=None):
    src = load_fp(fpid)
    if ref_pos:
        # property positions are stored footprint-local (unrotated); angle is absolute
        lx, ly = rot_pt(ref_pos[0] - x, ref_pos[1] - y, -rot)
        src = re.sub(r'(\(property "Reference" "REF\*\*"\s*\n\s*)\(at [^)]*\)', lambda m: m.group(1) + f"(at {f(lx)} {f(ly)} {f(-rot % 360)})", src, count=1)
    lines = src.splitlines()
    out = []
    name = fpid.split(":", 1)[1]
    depth = 0
    cur_pad = None
    seen_layer = False
    in_point = False
    for i, ln in enumerate(lines):
        s = ln.strip()
        if i == 0:
            out.append(f'(footprint "{fpid}"')
            out.append('\t(layer "F.Cu")')
            out.append(f'\t(uuid "{U()}")')
            out.append(f"\t(at {f(x)} {f(y)}{(' ' + f(rot)) if rot else ''})")
            depth = 1
            continue
        if ln.startswith("\t(") and not ln.startswith("\t\t"):
            if s.startswith(("(version", "(generator", "(generator_version")):
                continue
            if s.startswith('(layer "F.Cu")') and not seen_layer:
                seen_layer = True
                continue
            if s.startswith("(pad "):
                cur_pad = s.split('"')[1]
            elif not s.startswith(")"):
                cur_pad = None
            in_point = s.startswith("(point")
            if in_point:
                cur_pad = None
        # rotate the absolute angle of texts and pads
        def fix_at(m):
            a = float(m.group(3) or 0) + rot
            a = a % 360
            return f"(at {m.group(1)} {m.group(2)}{(' ' + f(a)) if a else ''})"

        if rot and "(at " in ln and not in_point:
            ln = AT_RE.sub(fix_at, ln)
        ln = ln.replace('"REF**"', f'"{ref}"')
        if s.startswith('(property "Value"'):
            ln = re.sub(r'\(property "Value" "[^"]*"', f'(property "Value" "{value}"', ln)
        out.append(ln)
        if cur_pad is not None and s.startswith("(layers "):
            net = nets.get(cur_pad)
            if net:
                out.append(f'\t\t(net "{bnet(net)}")')
    text = "\n".join(out)
    # sheet linkage, inserted before (attr ...)
    link = (f'\t(path "/{sym_uuid}")\n\t(sheetname "/")\n\t(sheetfile "{NAME}.kicad_sch")\n')
    extra = ""
    for k, v in props.items():
        extra += (f'\t(property "{k}" "{v}"\n\t\t(at 0 0 {f(rot)})\n\t\t(layer "F.Fab")\n\t\t(hide yes)\n\t\t(uuid "{U()}")\n'
                  f'\t\t(effects\n\t\t\t(font\n\t\t\t\t(size 1 1)\n\t\t\t\t(thickness 0.15)\n\t\t\t)\n\t\t)\n\t)\n')
    text = re.sub(r"\n\t\(attr ", "\n" + extra + link + "\t(attr ", text, count=1)
    return "\n".join("\t" + l for l in text.splitlines()) + "\n"


# ---------------------------------------------------------------------------
# Board
# ---------------------------------------------------------------------------
def gr_line(x1, y1, x2, y2, layer, w=0.1):
    return (f'\t(gr_line\n\t\t(start {f(x1)} {f(y1)})\n\t\t(end {f(x2)} {f(y2)})\n\t\t(stroke\n\t\t\t(width {w})\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(layer "{layer}")\n\t\t(uuid "{U()}")\n\t)\n')


def gr_arc(s, m, e, layer, w=0.1):
    return (f'\t(gr_arc\n\t\t(start {f(s[0])} {f(s[1])})\n\t\t(mid {f(m[0])} {f(m[1])})\n\t\t(end {f(e[0])} {f(e[1])})\n'
            f'\t\t(stroke\n\t\t\t(width {w})\n\t\t\t(type solid)\n\t\t)\n\t\t(layer "{layer}")\n\t\t(uuid "{U()}")\n\t)\n')


def gr_dot(x, y, r, layer="F.SilkS"):
    return (f'\t(gr_circle\n\t\t(center {f(x)} {f(y)})\n\t\t(end {f(x + r)} {f(y)})\n\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(fill yes)\n\t\t(layer "{layer}")\n\t\t(uuid "{U()}")\n\t)\n')


def gr_text(t, x, y, layer="F.SilkS", size=1.0, rot=0, bold=False, justify=None):
    j = f"\n\t\t\t(justify {justify})" if justify else ""
    b = "\n\t\t\t\t(bold yes)" if bold else ""
    th = round(size * (0.2 if bold else 0.15), 3)
    return (f'\t(gr_text "{t}"\n\t\t(at {f(x)} {f(y)}{(" " + f(rot)) if rot else ""})\n\t\t(layer "{layer}")\n\t\t(uuid "{U()}")\n'
            f'\t\t(effects\n\t\t\t(font\n\t\t\t\t(size {size} {size})\n\t\t\t\t(thickness {th}){b}\n\t\t\t){j}\n\t\t)\n\t)\n')


def zone(net, layer, pts, name, priority=0):
    p = " ".join(f"(xy {f(x)} {f(y)})" for x, y in pts)
    return (f'\t(zone\n\t\t(net "{net}")\n\t\t(layer "{layer}")\n\t\t(uuid "{U()}")\n\t\t(name "{name}")\n\t\t(hatch edge 0.5)\n'
            f'\t\t(priority {priority})\n\t\t(connect_pads\n\t\t\t(clearance 0.3)\n\t\t)\n\t\t(min_thickness 0.25)\n\t\t(filled_areas_thickness no)\n'
            f'\t\t(fill yes\n\t\t\t(thermal_gap 0.4)\n\t\t\t(thermal_bridge_width 0.4)\n\t\t\t(island_removal_mode 0)\n\t\t)\n'
            f'\t\t(polygon\n\t\t\t(pts\n\t\t\t\t{p}\n\t\t\t)\n\t\t)\n\t)\n')


def keepout(pts, name):
    p = " ".join(f"(xy {f(x)} {f(y)})" for x, y in pts)
    return (f'\t(zone\n\t\t(net "")\n\t\t(layers "F.Cu" "B.Cu")\n\t\t(uuid "{U()}")\n\t\t(name "{name}")\n\t\t(hatch edge 0.5)\n'
            f'\t\t(connect_pads\n\t\t\t(clearance 0)\n\t\t)\n\t\t(min_thickness 0.25)\n\t\t(filled_areas_thickness no)\n'
            f'\t\t(keepout\n\t\t\t(tracks allowed)\n\t\t\t(vias not_allowed)\n\t\t\t(pads allowed)\n\t\t\t(copperpour not_allowed)\n\t\t\t(footprints allowed)\n\t\t)\n'
            f'\t\t(fill\n\t\t\t(thermal_gap 0.5)\n\t\t\t(thermal_bridge_width 0.5)\n\t\t)\n'
            f'\t\t(polygon\n\t\t\t(pts\n\t\t\t\t{p}\n\t\t\t)\n\t\t)\n\t)\n')


def svg_path_points(d, steps=12):
    """Flatten an SVG path (M/L/H/V/C/Z, absolute and relative) into closed point lists."""
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
        rel = cmd.islower()
        c = cmd.upper()
        n = [float(v) for v in toks[i:i + {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6}[c]]]
        i += len(n)
        if c == "M":
            if cur:
                polys.append(cur)
            x, y = (x + n[0], y + n[1]) if rel else (n[0], n[1])
            cur = [(x, y)]
            cmd = "l" if rel else "L"   # further pairs are implicit line-tos
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


def logo_polys(cx, cy, width, layer="F.SilkS"):
    """The RF logo (../logo/RF-canopy-RGB-Orange.svg) as filled polygons, centred on (cx, cy) plan coords."""
    svg = open(os.path.join(OUT, "logo", "RF-canopy-RGB-Orange.svg"), encoding="utf-8").read()
    polys = [p for d in re.findall(r'\sd="([^"]+)"', svg) for p in svg_path_points(d)]
    xs = [px for p in polys for px, _ in p]
    ys = [py for p in polys for _, py in p]
    s = width / (max(xs) - min(xs))
    mx, my = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    out = ""
    for p in polys:
        pts = " ".join("(xy {} {})".format(*map(f, P(cx + (px - mx) * s, cy + (py - my) * s))) for px, py in p)
        out += (f'\t(gr_poly\n\t\t(pts\n\t\t\t{pts}\n\t\t)\n\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type solid)\n\t\t)\n'
                f'\t\t(fill yes)\n\t\t(layer "{layer}")\n\t\t(uuid "{U()}")\n\t)\n')
    return out


def make_board(sym_uuids):
    o = []
    o.append(f'(kicad_pcb\n\t(version 20260206)\n\t(generator "pcbnew")\n\t(generator_version "10.0")\n\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)\n\t(paper "A4")\n')
    o.append('\t(title_block\n\t\t(title "RF-Power-Monitor carrier for the W5500-EVB-Pico-PoE")\n\t\t(date "2026")\n\t\t(rev "1.1")\n\t\t(company "Kenneth Foli Jørgensen")\n\t)\n')
    demo = open(os.path.join(STOCK, "board_header.txt")).read()
    o.append(demo)
    for ref, sym, val, fp, x, y, rot, nets, props in PARTS:
        px, py = P(x, y)
        if ref == "U1":
            nets = dict(nets)
            for n in range(1, 41):
                nets.setdefault(n, f"unconnected-(U1-{PICO_PINS[n]}-Pad{n})")
        mains = any(n in MAINS_NETS for n in nets.values())
        rp = P(*REF_POS[ref]) if ref in REF_POS else None
        o.append(board_footprint(fp, ref, val, px, py, rot, {str(k): v for k, v in nets.items()}, props, sym_uuids[ref], mains, rp))
    for ref, x, y in HOLES:
        hx, hy = B(x, y)
        rp = P(*REF_POS[ref]) if ref in REF_POS else None
        o.append(board_footprint(hole_fpid(ref), ref, "MountingHole_3.2mm_M3", hx, hy, 0, {}, {}, sym_uuids[ref], False, rp))
    # outline with the clearance notches; rounded outer corners
    r = 2.0
    k = r * (1 - math.sqrt(0.5))
    nx, ny = NOTCH_RJ45
    ux, uy0, uy1 = NOTCH_USB
    W, H = BOARD_W, BOARD_H
    q = 1.0                          # radius of the small cutout corners
    kq = q * (1 - math.sqrt(0.5))
    for a, b_ in [((nx + r, TOP), (W - r, TOP)), ((W, TOP + r), (W, uy0 - q)), ((W - q, uy0), (ux + q, uy0)),
                  ((ux, uy0 + q), (ux, uy1 - q)), ((ux + q, uy1), (W - q, uy1)), ((W, uy1 + q), (W, H - r)),
                  ((W - r, H), (r, H)), ((0, H - r), (0, ny + r)),
                  ((r, ny), (nx - r, ny)), ((nx, ny - r), (nx, TOP + r))]:
        o.append(gr_line(*P(*a), *P(*b_), "Edge.Cuts"))
    # USB notch: rounded where it meets the right edge and at its inside corners
    o.append(gr_arc(P(W, uy0 - q), P(W - kq, uy0 - kq), P(W - q, uy0), "Edge.Cuts"))
    o.append(gr_arc(P(ux + q, uy0), P(ux + kq, uy0 + kq), P(ux, uy0 + q), "Edge.Cuts"))
    o.append(gr_arc(P(ux, uy1 - q), P(ux + kq, uy1 - kq), P(ux + q, uy1), "Edge.Cuts"))
    o.append(gr_arc(P(W - q, uy1), P(W - kq, uy1 + kq), P(W, uy1 + q), "Edge.Cuts"))
    # rounded RJ45-notch corners (as drawn by the user in KiCad, r = 2 mm)
    o.append(gr_arc(P(0, ny + r), P(k, ny + k), P(r, ny), "Edge.Cuts"))
    o.append(gr_arc(P(nx, ny - r), P(nx - k, ny - k), P(nx - r, ny), "Edge.Cuts"))
    o.append(gr_arc(P(nx, TOP + r), P(nx + k, TOP + k), P(nx + r, TOP), "Edge.Cuts"))
    o.append(gr_arc(P(W - r, TOP), P(W - k, TOP + k), P(W, TOP + r), "Edge.Cuts"))
    o.append(gr_arc(P(W, H - r), P(W - k, H - k), P(W - r, H), "Edge.Cuts"))
    o.append(gr_arc(P(r, H), P(k, H - k), P(0, H - r), "Edge.Cuts"))
    wx0, wy0, wx1, wy1 = WIN_POE
    for a, b_ in [((wx0 + q, wy0), (wx1 - q, wy0)), ((wx1, wy0 + q), (wx1, wy1 - q)), ((wx1 - q, wy1), (wx0 + q, wy1)),
                  ((wx0, wy1 - q), (wx0, wy0 + q))]:
        o.append(gr_line(*P(*a), *P(*b_), "Edge.Cuts"))
    o.append(gr_arc(P(wx1 - q, wy0), P(wx1 - kq, wy0 + kq), P(wx1, wy0 + q), "Edge.Cuts"))
    o.append(gr_arc(P(wx1, wy1 - q), P(wx1 - kq, wy1 - kq), P(wx1 - q, wy1), "Edge.Cuts"))
    o.append(gr_arc(P(wx0 + q, wy1), P(wx0 + kq, wy1 - kq), P(wx0, wy1 - q), "Edge.Cuts"))
    o.append(gr_arc(P(wx0, wy0 + q), P(wx0 + kq, wy0 + kq), P(wx0 + q, wy0), "Edge.Cuts"))
    # isolation slot under the optocoupler (vertical, between its input and output pins)
    sx, sy0, sy1, sw = SLOT
    a, b_ = P(sx + sw / 2, sy0 + sw / 2), P(sx + sw / 2, sy1 - sw / 2)
    c, d = P(sx - sw / 2, sy1 - sw / 2), P(sx - sw / 2, sy0 + sw / 2)
    o.append(gr_line(*a, *b_, "Edge.Cuts"))
    o.append(gr_arc(b_, P(sx, sy1), c, "Edge.Cuts"))
    o.append(gr_line(*c, *d, "Edge.Cuts"))
    o.append(gr_arc(d, P(sx, sy0), a, "Edge.Cuts"))
    # silkscreen: hazardous area (open where the optocoupler bridges the barrier)
    for a, b_ in [((46.5, 22.8), (79.6, 22.8)), ((46.5, 22.8), (46.5, 29.2)), ((46.5, 35.8), (46.5, 41.5)),
                  ((46.5, 41.5), (79.6, 41.5))]:
        for layer in ("F.SilkS", "B.SilkS"):
            o.append(gr_line(*P(*a), *P(*b_), layer, 0.3))
    o.append(gr_text("DANGER 230VAC", *P(59.5, 39.9), size=1.2, bold=True))
    o.append(gr_text("HAZARDOUS VOLTAGE", *P(62.0, 39.9), "B.SilkS", 1.0, justify="mirror"))
    o.append(gr_text("L", *P(68.4, 37.9), size=1.2, bold=True))
    o.append(gr_text("N", *P(67.6, 28.0), size=1.2, bold=True))
    o.append(gr_text("RESET 10s", *P(15.6, 24.875), size=0.8, justify="left"))
    # U2 pin 1: the slot cuts away the footprint's notch, so mark it with a dot just below pad 1
    # (U2 is rotated 180: pad 1 at local (-4.765, -1.27) lands right of centre, below it)
    o.append(gr_dot(*P(OPTO[0] + 4.765, OPTO[1] + 1.27 + 1.75), 0.35))
    o.append(gr_text("230VAC IN", *P(74.6, 39.9), size=1.0))
    o.append(gr_text("ON", *P(24.0, 38.0), size=0.8))
    o.append(gr_text("OFF", *P(29.0, 32.4), size=0.8, justify="left"))
    o.append(gr_text("TEMP", *P(14.0, 49.0), size=0.8))
    o.append(gr_text("GP27=MAINS_OK (1=ON)", *P(29.0, 42.6), size=0.8, justify="left"))
    # logo, project name and maker in the free strip along the bottom edge
    o.append(logo_polys(28.0, 46.4, 8.0))
    o.append(gr_text("RF-Power-Monitor v1.1", *P(33.2, 44.9), size=1.2, bold=True, justify="left"))
    o.append(gr_text("Made by Kenneth Foli Jørgensen 2026", *P(33.2, 46.7), size=0.8, justify="left"))
    o.append(gr_text("I2C 0x48 GP4/GP5", *P(33.2, 48.3), size=0.8, justify="left"))
    # tracks + vias
    for net, layer, w, pts in TRACKS:
        for (xa, ya), (xb, yb) in zip(pts, pts[1:]):
            ax, ay = P(xa, ya)
            bx, by = P(xb, yb)
            o.append(f'\t(segment\n\t\t(start {f(ax)} {f(ay)})\n\t\t(end {f(bx)} {f(by)})\n\t\t(width {w})\n\t\t(layer "{layer}")\n\t\t(net "{bnet(net)}")\n\t\t(uuid "{U()}")\n\t)\n')
    for net, x, y in VIAS:
        vx, vy = P(x, y)
        o.append(f'\t(via\n\t\t(at {f(vx)} {f(vy)})\n\t\t(size 0.6)\n\t\t(drill 0.3)\n\t\t(layers "F.Cu" "B.Cu")\n\t\t(net "{bnet(net)}")\n\t\t(uuid "{U()}")\n\t)\n')
    # GND pours over the whole board (mains area excluded by the keepout + DRC rules)
    outline = [P(0, TOP), P(BOARD_W, TOP), P(BOARD_W, BOARD_H), P(0, BOARD_H)]
    o.append(zone("GND", "F.Cu", outline, "GND_top"))
    o.append(zone("GND", "B.Cu", outline, "GND_bottom"))
    o.append(keepout([P(*p) for p in MAINS_AREA], "MAINS_NO_POUR"))
    o.append("\t(embedded_fonts no)\n)\n")
    return "".join(o)


# ---------------------------------------------------------------------------
# Symbols (lib coords, y up)
# ---------------------------------------------------------------------------
def font(size=1.27):
    return f"(effects (font (size {size} {size})))"


def prop(name, val, x=0, y=0, hide=False, rot=0):
    h = " (hide yes)" if hide else ""
    return f'(property "{name}" "{val}" (at {f(x)} {f(y)} {rot}){h} {font()})'


def pin(ptype, x, y, ang, num, name="~", length=2.54, hide=False):
    h = " (hide yes)" if hide else ""
    return (f'(pin {ptype} line (at {f(x)} {f(y)} {ang}) (length {length}){h} '
            f'(name "{name}" {font()}) (number "{num}" {font()}))')


def symdef(name, ref, body, pins, props=None, power=False, pin_names_hidden=False, pin_numbers_hidden=False, offset=0.508):
    """returns (lib symbol text without lib prefix, pin table {num: (x, y, ang)})"""
    hdr = f'(symbol "{name}"'
    if power:
        hdr += " (power global)"
    if pin_numbers_hidden:
        hdr += " (pin_numbers (hide yes))"
    hdr += f" (pin_names (offset {offset})" + (" (hide yes))" if pin_names_hidden else ")")
    hdr += " (exclude_from_sim no) (in_bom {}) (on_board {})".format("no" if power or name == "MountingHole" else "yes", "no" if power else "yes")
    pr = props or []
    s = hdr + " " + " ".join(pr) + f' (symbol "{name}_0_1" {body}) (symbol "{name}_1_1" ' + " ".join(p[0] for p in pins) + ") (embedded_fonts no))"
    table = {p[1]: p[2] for p in pins}
    return s, table


def P_(ptype, x, y, ang, num, name="~", **kw):
    return (pin(ptype, x, y, ang, num, name, **kw), str(num), (x, y, ang))


def rect_g(x1, y1, x2, y2, fill="background"):
    return f"(rectangle (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.254) (type default)) (fill (type {fill})))"


def poly_g(pts, fill="none", w=0.254):
    p = " ".join(f"(xy {f(x)} {f(y)})" for x, y in pts)
    return f"(polyline (pts {p}) (stroke (width {w}) (type default)) (fill (type {fill})))"


def build_symbols():
    S = {}
    # module
    pins = []
    for n in range(1, 41):
        nm = PICO_PINS[n]
        ptype = "passive" if nm in ("GND", "AGND") else ("power_out" if nm == "3V3" else "bidirectional")
        if n <= 20:
            pins.append(P_(ptype, -17.78, 24.13 - (n - 1) * 2.54, 0, n, nm))
        else:
            pins.append(P_(ptype, 17.78, -24.13 + (n - 21) * 2.54, 180, n, nm))
    S["W5500-EVB-Pico-PoE"] = symdef("W5500-EVB-Pico-PoE", "U", rect_g(-15.24, 26.67, 15.24, -26.67), pins,
                                     [prop("Reference", "U", 0, 27.94), prop("Value", "W5500-EVB-Pico-PoE", 0, -27.94),
                                      prop("Footprint", f"{LIB}:W5500-EVB-Pico-PoE_Socket", 0, -30.48, True),
                                      prop("Datasheet", "https://docs.wiznet.io/Product/Chip/Ethernet/W5500/W5500-EVB-Pico-PoE", 0, 0, True),
                                      prop("Description", "WIZnet W5500-EVB-Pico-PoE (RP2040 + W5500 + PoE), Pico pinout. GP16-GP21 used by the W5500.", 0, 0, True)])
    # resistor / capacitor / LED
    S["R"] = symdef("R", "R", rect_g(-1.016, 2.54, 1.016, -2.54, "none"),
                    [P_("passive", 0, 3.81, 270, 1, length=1.27), P_("passive", 0, -3.81, 90, 2, length=1.27)],
                    [prop("Reference", "R", 2.032, 1.27), prop("Value", "R", 2.032, -1.27), prop("Footprint", "", 0, 0, True),
                     prop("Datasheet", "", 0, 0, True), prop("Description", "Resistor", 0, 0, True)],
                    pin_names_hidden=True, pin_numbers_hidden=True, offset=0)
    S["C"] = symdef("C", "C", poly_g([(-2.032, 0.762), (2.032, 0.762)], w=0.508) + " " + poly_g([(-2.032, -0.762), (2.032, -0.762)], w=0.508),
                    [P_("passive", 0, 3.81, 270, 1, length=3.048), P_("passive", 0, -3.81, 90, 2, length=3.048)],
                    [prop("Reference", "C", 2.794, 1.27), prop("Value", "C", 2.794, -1.27), prop("Footprint", "", 0, 0, True),
                     prop("Datasheet", "", 0, 0, True), prop("Description", "Capacitor", 0, 0, True)],
                    pin_names_hidden=True, pin_numbers_hidden=True, offset=0)
    S["Fuse"] = symdef("Fuse", "F", rect_g(-0.762, 2.54, 0.762, -2.54, "none") + " " + poly_g([(0, 2.54), (0, -2.54)]),
                       [P_("passive", 0, 3.81, 270, 1, length=1.27), P_("passive", 0, -3.81, 90, 2, length=1.27)],
                       [prop("Reference", "F", 2.032, 1.27), prop("Value", "Fuse", 2.032, -1.27), prop("Footprint", "", 0, 0, True),
                        prop("Datasheet", "https://www.setfuse.com/Pr_d_gci_92_id_128.html", 0, 0, True),
                        prop("Description", "Fuse", 0, 0, True)],
                       pin_names_hidden=True, pin_numbers_hidden=True, offset=0)
    S["SW_Push"] = symdef("SW_Push", "SW", "(circle (center 0 2.032) (radius 0.508) (stroke (width 0) (type default)) (fill (type none))) "
                          "(circle (center 0 -2.032) (radius 0.508) (stroke (width 0) (type default)) (fill (type none))) "
                          + poly_g([(-1.27, 2.54), (-1.27, -2.54)]) + " " + poly_g([(-1.27, 0), (-3.048, 0)]),
                          [P_("passive", 0, 5.08, 270, 1, length=2.54), P_("passive", 0, -5.08, 90, 2, length=2.54)],
                          [prop("Reference", "SW", 2.032, 1.27), prop("Value", "SW_Push", 2.032, -1.27), prop("Footprint", "", 0, 0, True),
                           prop("Datasheet", "", 0, 0, True), prop("Description", "Push button, normally open", 0, 0, True)],
                          pin_names_hidden=True, pin_numbers_hidden=True, offset=0)
    S["LED"] = symdef("LED", "D", poly_g([(-1.27, -1.27), (-1.27, 1.27)]) + " " + poly_g([(1.27, -1.27), (1.27, 1.27), (-1.27, 0), (1.27, -1.27)])
                      + " " + poly_g([(-0.254, 1.524), (0.762, 2.54)]) + " " + poly_g([(0.508, 1.524), (1.524, 2.54)]),
                      [P_("passive", -3.81, 0, 0, 1, "K"), P_("passive", 3.81, 0, 180, 2, "A")],
                      [prop("Reference", "D", 0, 3.81), prop("Value", "LED", 0, -2.54), prop("Footprint", "", 0, 0, True),
                       prop("Datasheet", "", 0, 0, True), prop("Description", "LED, pin 1 = cathode", 0, 0, True)],
                      pin_names_hidden=True, pin_numbers_hidden=True, offset=1.016)
    S["Screw_Terminal_2"] = symdef("Screw_Terminal_2", "J", rect_g(-2.54, 2.54, 2.54, -2.54),
                                   [P_("passive", -5.08, 1.27, 0, 1, "L"), P_("passive", -5.08, -1.27, 0, 2, "N")],
                                   [prop("Reference", "J", 0, 3.81), prop("Value", "Screw_Terminal_2", 0, -3.81),
                                    prop("Footprint", "", 0, 0, True), prop("Datasheet", "", 0, 0, True),
                                    prop("Description", "2-pole screw terminal, 5.08 mm", 0, 0, True)])
    S["LTV-814S"] = symdef("LTV-814S", "U",
                           rect_g(-5.08, 3.81, 5.08, -3.81) + " " + poly_g([(0, 3.81), (0, -3.81)], w=0.152),
                           [P_("passive", -7.62, 2.54, 0, 1, "AC1"), P_("passive", -7.62, -2.54, 0, 2, "AC2"),
                            P_("passive", 7.62, -2.54, 180, 3, "E"), P_("passive", 7.62, 2.54, 180, 4, "C")],
                           [prop("Reference", "U", 0, 5.08), prop("Value", "LTV-814S", 0, -5.08), prop("Footprint", "", 0, 0, True),
                            prop("Datasheet", "https://www.lcsc.com/datasheet/lcsc_datasheet_1810010411_Lite-On-LTV-814S-TA1_C125118.pdf", 0, 0, True),
                            prop("Description", "AC-input optocoupler, 5 kVrms isolation", 0, 0, True)])
    S["74LVC1G14"] = symdef("74LVC1G14", "U",
                            rect_g(-5.08, 5.08, 5.08, -5.08),
                            [P_("no_connect", -7.62, 2.54, 0, 1, "NC"), P_("input", -7.62, 0, 0, 2, "A"),
                             P_("power_in", 0, -7.62, 90, 3, "GND"), P_("output", 7.62, 0, 180, 4, "Y"),
                             P_("power_in", 0, 7.62, 270, 5, "VCC")],
                            [prop("Reference", "U", 2.54, 6.35), prop("Value", "74LVC1G14", 2.54, -6.35), prop("Footprint", "", 0, 0, True),
                             prop("Datasheet", "https://www.ti.com/lit/ds/symlink/sn74lvc1g14.pdf", 0, 0, True),
                             prop("Description", "Single Schmitt-trigger inverter", 0, 0, True)])
    S["TMP1075"] = symdef("TMP1075", "U",
                          rect_g(-7.62, 7.62, 7.62, -7.62),
                          [P_("bidirectional", -10.16, 2.54, 0, 1, "SDA"), P_("input", -10.16, 0, 0, 2, "SCL"),
                           P_("open_collector", -10.16, -2.54, 0, 3, "ALERT"), P_("power_in", 0, -10.16, 90, 4, "GND"),
                           P_("input", 10.16, -2.54, 180, 5, "A2"), P_("input", 10.16, 0, 180, 6, "A1"),
                           P_("input", 10.16, 2.54, 180, 7, "A0"), P_("power_in", 0, 10.16, 270, 8, "V+")],
                          [prop("Reference", "U", 2.54, 8.89), prop("Value", "TMP1075", 2.54, -8.89), prop("Footprint", "", 0, 0, True),
                           prop("Datasheet", "https://www.ti.com/lit/ds/symlink/tmp1075.pdf", 0, 0, True),
                           prop("Description", "I2C temperature sensor, +-1 C, LM75 compatible", 0, 0, True)])
    S["MountingHole"] = symdef("MountingHole", "H", "(circle (center 0 0) (radius 1.27) (stroke (width 1.27) (type default)) (fill (type none)))", [],
                               [prop("Reference", "H", 0, 3.175), prop("Value", "MountingHole", 0, -3.175),
                                prop("Footprint", "MountingHole:MountingHole_3.2mm_M3", 0, 0, True),
                                prop("Datasheet", "", 0, 0, True), prop("Description", "M3 mounting hole, no copper", 0, 0, True)])
    S["GND"] = symdef("GND", "#PWR", poly_g([(0, 0), (0, -1.27), (1.27, -1.27), (0, -2.54), (-1.27, -1.27), (0, -1.27)]),
                      [P_("power_in", 0, 0, 270, 1, "GND", length=0, hide=True)],
                      [prop("Reference", "#PWR", 0, -6.35, True), prop("Value", "GND", 0, -3.81),
                       prop("Footprint", "", 0, 0, True), prop("Datasheet", "", 0, 0, True),
                       prop("Description", "Power symbol GND", 0, 0, True)], power=True)
    S["+3V3"] = symdef("+3V3", "#PWR", poly_g([(-0.762, 1.27), (0, 2.54)]) + " " + poly_g([(0, 0), (0, 2.54)]) + " " + poly_g([(0, 2.54), (0.762, 1.27)]),
                       [P_("power_in", 0, 0, 90, 1, "+3V3", length=0, hide=True)],
                       [prop("Reference", "#PWR", 0, -3.81, True), prop("Value", "+3V3", 0, 3.556),
                        prop("Footprint", "", 0, 0, True), prop("Datasheet", "", 0, 0, True),
                        prop("Description", "Power symbol +3V3", 0, 0, True)], power=True)
    S["PWR_FLAG"] = symdef("PWR_FLAG", "#FLG", poly_g([(0, 0), (0, 1.27), (-1.016, 1.905), (0, 2.54), (1.016, 1.905), (0, 1.27)]),
                           [P_("power_out", 0, 0, 90, 1, "pwr", length=0, hide=True)],
                           [prop("Reference", "#FLG", 0, 1.905, True), prop("Value", "PWR_FLAG", 0, 3.81),
                            prop("Footprint", "", 0, 0, True), prop("Datasheet", "", 0, 0, True),
                            prop("Description", "Power flag", 0, 0, True)], power=True)
    return S


# ---------------------------------------------------------------------------
# Schematic
# ---------------------------------------------------------------------------
class Sch:
    def __init__(self, symbols, root):
        self.S = symbols
        self.root = root
        self.items = []
        self.pwr = 0
        self.used = set()

    def place(self, lib, ref, value, x, y, rot=0, footprint="", props=None, uid=None, hide_value=False, ref_off=None, val_off=None):
        uid = uid or U()
        self.used.add(lib)
        sdef, table = self.S[lib]
        is_pwr = ref.startswith("#")
        # property positions: take from lib definition offsets (rough), shown above/below
        ro = ref_off or (0, -5.08)
        vo = val_off or (0, 5.08)
        ps = [f'(property "Reference" "{ref}" (at {f(x + ro[0])} {f(y + ro[1])} 0){" (hide yes)" if is_pwr else ""} {font()})',
              f'(property "Value" "{value}" (at {f(x + vo[0])} {f(y + vo[1])} 0){" (hide yes)" if hide_value else ""} {font()})',
              f'(property "Footprint" "{footprint}" (at {f(x)} {f(y)} 0) (hide yes) {font()})',
              f'(property "Datasheet" "" (at {f(x)} {f(y)} 0) (hide yes) {font()})']
        for k, v in (props or {}).items():
            ps.append(f'(property "{k}" "{v}" (at {f(x)} {f(y)} 0) (hide yes) {font()})')
        pins = " ".join(f'(pin "{n}" (uuid "{U()}"))' for n in table)
        self.items.append(
            f'(symbol (lib_id "{LIB}:{lib}") (at {f(x)} {f(y)} {rot}) (unit 1) (body_style 1) (exclude_from_sim no) '
            f'(in_bom {"no" if is_pwr or lib == "MountingHole" else "yes"}) (on_board {"no" if is_pwr else "yes"}) (in_pos_files {"no" if is_pwr else "yes"}) (dnp no) (uuid "{uid}") '
            + " ".join(ps) + " " + pins +
            f' (instances (project "{NAME}" (path "/{self.root}" (reference "{ref}") (unit 1)))))')
        return uid

    def pin_xy(self, lib, x, y, rot, num):
        px, py, ang = self.S[lib][1][str(num)]
        vx, vy = px, -py
        a = math.radians(rot)
        rx = vx * math.cos(a) + vy * math.sin(a)
        ry = -vx * math.sin(a) + vy * math.cos(a)
        # outward direction (away from body), sheet coords
        oa = math.radians(ang + 180 + rot)
        ox, oy = round(math.cos(oa)), -round(math.sin(oa))
        return round(x + rx, 4), round(y + ry, 4), (ox, oy)

    def wire(self, pts):
        p = " ".join(f"(xy {f(a)} {f(b)})" for a, b in pts)
        self.items.append(f'(wire (pts {p}) (stroke (width 0) (type default)) (uuid "{U()}"))')

    def label(self, net, x, y, d):
        ang = {(1, 0): 0, (-1, 0): 180, (0, -1): 90, (0, 1): 270}[d]
        just = "left bottom" if ang in (0, 90) else "right bottom"
        self.items.append(f'(label "{net}" (at {f(x)} {f(y)} {ang}) (effects (font (size 1.27 1.27)) (justify {just})) (uuid "{U()}"))')

    def power(self, net, x, y, d):
        self.pwr += 1
        if net == "GND":
            rot = {(0, 1): 0, (0, -1): 180, (1, 0): 90, (-1, 0): 270}[d]
            self.place("GND", f"#PWR{self.pwr:02d}", "GND", x, y, rot, val_off=(0, 3.81), hide_value=rot != 0)
        else:
            self.place("+3V3", f"#PWR{self.pwr:02d}", "+3V3", x, y, 0, val_off=(0, -3.81))

    def junction(self, x, y):
        self.items.append(f'(junction (at {f(x)} {f(y)}) (diameter 0) (color 0 0 0 0) (uuid "{U()}"))')

    def nc(self, x, y):
        self.items.append(f'(no_connect (at {f(x)} {f(y)}) (uuid "{U()}"))')

    def text(self, t, x, y, size=1.27, bold=False):
        b = " (bold yes)" if bold else ""
        t = t.replace("\n", "\\n")
        self.items.append(f'(text "{t}" (exclude_from_sim no) (at {f(x)} {f(y)} 0) (effects (font (size {size} {size}){b}) (justify left bottom)) (uuid "{U()}"))')

    def rect(self, x1, y1, x2, y2):
        self.items.append(f'(rectangle (start {f(x1)} {f(y1)}) (end {f(x2)} {f(y2)}) (stroke (width 0.3) (type dash)) (fill (type none)) (uuid "{U()}"))')

    def connect(self, lib, x, y, rot, nets, stub=2.54, all_pins=True):
        """stub wire + label/power symbol on every pin; NC flag on pins without net.
        Sideways GND pins stacked on one side share a single GND symbol."""
        gnd_side = {}
        for num in self.S[lib][1]:
            px, py, d = self.pin_xy(lib, x, y, rot, num)
            if (nets.get(num) or nets.get(int(num))) == "GND" and d[1] == 0:
                gnd_side.setdefault((d, round(px, 3)), []).append(py)
        grouped = {}
        for (d, px), ys in gnd_side.items():
            ys = sorted(ys)
            if len(ys) > 1 and all(abs(b - a - 2.54) < 0.01 for a, b in zip(ys, ys[1:])):
                ex = round(px + d[0] * stub, 4)
                self.wire([(ex, ys[0]), (ex, ys[-1])])
                for yy in ys[:-1]:
                    self.junction(ex, yy)
                self.wire([(ex, ys[-1]), (ex, ys[-1] + 2.54)])
                self.power("GND", ex, ys[-1] + 2.54, (0, 1))
                for yy in ys:
                    grouped[(px, yy)] = True
        for num in self.S[lib][1]:
            px, py, d = self.pin_xy(lib, x, y, rot, num)
            net = nets.get(num) or nets.get(int(num)) if isinstance(nets, dict) else None
            if not net or net.startswith("unconnected-"):
                if all_pins:
                    self.nc(px, py)
                continue
            st = stub * 3 if net == "+3V3" and d[1] == 0 else stub
            ex, ey = round(px + d[0] * st, 4), round(py + d[1] * st, 4)
            self.wire([(px, py), (ex, ey)])
            if (round(px, 3), py) in grouped:
                continue
            if net in ("GND", "+3V3"):
                self.power(net, ex, ey, d)
            else:
                self.label(net, ex, ey, d)


def make_schematic(symbols):
    root = U()
    sch = Sch(symbols, root)
    uids = {}
    # placement on an A3 sheet, everything on the 1.27 grid
    SPOT = {
        "U1": (80.01, 110.49, 0), "J1": (40.64, 210.82, 0),
        "F1": (66.04, 203.2, 0), "R1": (99.06, 203.2, 0),
        "R2": (66.04, 233.68, 0), "R3": (82.55, 233.68, 0), "R4": (99.06, 233.68, 0),
        "U2": (127.0, 214.63, 0), "R5": (165.1, 201.93, 0), "C1": (180.34, 220.98, 0),
        "U3": (213.36, 209.55, 0), "C3": (236.22, 201.93, 0),
        "R6": (279.4, 201.93, 0), "D1": (279.4, 220.98, 0), "D2": (309.88, 220.98, 0), "R7": (309.88, 201.93, 180),
        "U4": (304.8, 106.68, 0), "C2": (342.9, 106.68, 180), "R8": (261.62, 104.14, 180), "R9": (274.32, 104.14, 180),
        "SW1": (142.24, 120.65, 0),
    }
    # vertical R/C get their reference to the right
    for ref, sym, val, fp, x, y, rot, nets, props in PARTS:
        sx, sy, sr = SPOT[ref]
        lr = "Screw_Terminal_2" if sym == "Screw_Terminal_2" else sym
        ro = (3.81, -1.27) if sym in ("R", "C", "Fuse", "SW_Push") else None
        vo = (3.81, 1.27) if sym in ("R", "C", "Fuse", "SW_Push") else None
        if sym == "W5500-EVB-Pico-PoE":
            ro, vo = (0, -29.21), (0, 29.21)
        if sym == "LED":
            ro, vo = (0, -3.81), (0, 3.81)
        if sym == "74LVC1G14":
            ro, vo = (7.62, -6.35), (7.62, 7.62)
        if sym == "TMP1075":
            ro, vo = (7.62, -11.43), (7.62, 11.43)
        if sym == "LTV-814S":
            ro, vo = (0, -5.08), (0, 6.35)
        uids[ref] = sch.place(lr, ref, val, sx, sy, sr, fp, props, ref_off=ro, val_off=vo)
        sch.connect(lr, sx, sy, sr, {str(k): v for k, v in nets.items()})
    for i, (ref, _, _) in enumerate(HOLES):
        uids[ref] = sch.place("MountingHole", ref, "MountingHole_3.2mm_M3", 355.6 + i * 12.7, 254.0, 0,
                              hole_fpid(ref), ref_off=(0, -3.81), val_off=(0, 3.81), hide_value=True)
    # power flag on GND (the +3V3 rail is driven by the module's 3V3 output)
    sch.place("PWR_FLAG", "#FLG01", "PWR_FLAG", 139.7, 66.04, 0, val_off=(0, -3.81))
    sch.wire([(139.7, 66.04), (139.7, 68.58)])
    sch.power("GND", 139.7, 68.58, (0, 1))

    # frames and notes
    sch.rect(20.32, 50.8, 160.02, 175.26)
    sch.text("W5500-EVB-Pico-PoE (plugs into 2x 1x20 female headers)", 22.86, 55.88, 2, True)
    sch.text("GP16..GP21 are used by the on-board W5500 - do not use.\nGP27 (pin 32) = MAINS_OK input: HIGH = 230VAC present.\nGP4/GP5 = I2C0 SDA/SCL to TMP1075 (address 0x48).\nGP22 (pin 29) = SW1 to GND: hold 10 s for a factory reset (internal pull-up).\n+3V3 comes from the module's 3V3(OUT) pin 36 (PoE powered).", 22.86, 158.75)
    sch.rect(20.32, 182.88, 147.32, 259.08)
    sch.text("HAZARDOUS VOLTAGE - 230VAC MAINS SENSE", 22.86, 187.96, 2, True)
    sch.text("F1 T1A 250VAC in the only current loop. 4x 68k 1206 (>=200V): ~1.2 mA peak, ~50 mW each.\nLTV-814S AC-input optocoupler, 5 kVrms. PCB: >=6.5 mm clearance, >=6.4 mm creepage + routed slot.", 22.86, 254.0)
    sch.rect(152.4, 182.88, 330.2, 259.08)
    sch.text("Mains detect + LED indication (works without firmware)", 154.94, 187.96, 2, True)
    sch.text("Opto pulls SENSE low while mains is present; C1 holds it low over the zero crossings (47k x 1uF = 47 ms).\nMAINS_OK = NOT SENSE: green LED on = mains ON, red LED on = mains OFF.", 154.94, 254.0)
    sch.rect(254.0, 76.2, 365.76, 137.16)
    sch.text("Temperature sensor TMP1075 (I2C 0x48, +-1 C)", 256.54, 81.28, 2, True)
    sch.text("Placed away from the module and the mains resistors.\nALERT not used.", 256.54, 132.08)

    lib_syms = "(lib_symbols " + " ".join(
        sch.S[n][0].replace(f'(symbol "{n}"', f'(symbol "{LIB}:{n}"', 1) for n in sorted(sch.used)) + ")"
    out = (f'(kicad_sch (version 20260101) (generator "eeschema") (generator_version "10.0") (uuid "{root}") (paper "A3") '
           f'(title_block (title "RF-Power-Monitor carrier for the W5500-EVB-Pico-PoE") (date "2026") (rev "1.1") (company "Kenneth Foli Jørgensen") (comment 1 "Mains detect on GP27, LED status, TMP1075 temperature, factory reset button on GP22")) '
           + lib_syms + " " + " ".join(sch.items) +
           ' (sheet_instances (path "/" (page "1"))) (embedded_fonts no))\n')
    return out, uids, root


def sym_lib_file(symbols):
    body = " ".join(symbols[n][0] for n in sorted(symbols))
    return f'(kicad_symbol_lib (version 20251024) (generator "{NAME}-gen") (generator_version "10.0") {body})\n'


def main():
    symbols = build_symbols()
    sch, uids, root = make_schematic(symbols)
    open(os.path.join(OUT, f"{NAME}.kicad_sch"), "w", encoding="utf-8").write(sch)
    open(os.path.join(OUT, f"{NAME}.kicad_pcb"), "w", encoding="utf-8").write(make_board(uids))
    open(os.path.join(OUT, f"{LIB}.kicad_sym"), "w", encoding="utf-8").write(sym_lib_file(symbols))
    os.makedirs(os.path.join(OUT, f"{LIB}.pretty"), exist_ok=True)
    open(os.path.join(OUT, f"{LIB}.pretty", "W5500-EVB-Pico-PoE_Socket.kicad_mod"), "w", encoding="utf-8").write(module_footprint_text())
    open(os.path.join(OUT, f"{LIB}.pretty", f"{HOLE_UNDER_MODULE}.kicad_mod"), "w", encoding="utf-8").write(under_module_hole_text())
    open(os.path.join(OUT, "sym-lib-table"), "w", encoding="utf-8").write(
        f'(sym_lib_table\n\t(version 7)\n\t(lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.kicad_sym")(options "")(descr "Project symbols"))\n)\n')
    open(os.path.join(OUT, "fp-lib-table"), "w", encoding="utf-8").write(
        f'(fp_lib_table\n\t(version 7)\n\t(lib (name "{LIB}")(type "KiCad")(uri "${{KIPRJMOD}}/{LIB}.pretty")(options "")(descr "Project footprints"))\n)\n')
    open(os.path.join(OUT, f"{NAME}.kicad_dru"), "w", encoding="utf-8").write(DRU)
    import json
    pro = json.load(open(os.path.join(STOCK, "template.kicad_pro")))
    pro["meta"]["filename"] = f"{NAME}.kicad_pro"
    pro["sheets"] = [[root, "Root"]]
    pro["boards"] = []
    base = dict(pro["net_settings"]["classes"][0])
    default = dict(base, name="Default", clearance=0.2, track_width=0.25, via_diameter=0.6, via_drill=0.3)
    power = dict(base, name="Power", clearance=0.2, track_width=0.4, via_diameter=0.6, via_drill=0.3, priority=1)
    mains = dict(base, name="Mains", clearance=0.5, track_width=0.5, via_diameter=0.6, via_drill=0.3, priority=0,
                 pcb_color="rgba(255, 64, 64, 0.800)", schematic_color="rgba(200, 0, 0, 1.000)")
    pro["net_settings"]["classes"] = [default, power, mains]
    pro["net_settings"]["netclass_patterns"] = [{"netclass": "Mains", "pattern": "/" + n} for n in MAINS_NETS] + \
        [{"netclass": "Power", "pattern": n} for n in ("GND", "+3V3")]
    r = pro["board"]["design_settings"]["rules"]
    pro["board"]["design_settings"]["rule_severities"]["npth_inside_courtyard"] = "ignore"
    r.update(min_clearance=0.2, min_track_width=0.2, min_via_diameter=0.5, min_via_annular_width=0.1,
             min_through_hole_diameter=0.3, min_copper_edge_clearance=0.5, min_hole_to_hole=0.25)
    pro["libraries"] = {"pinned_footprint_libs": [], "pinned_symbol_libs": []}
    json.dump(pro, open(os.path.join(OUT, f"{NAME}.kicad_pro"), "w", encoding="utf-8"), indent=2)
    print("written to", OUT)


DRU = """(version 1)
# Reinforced isolation between 230VAC and the SELV side (IEC 62368-1, 250 Vrms, PD2):
# >= 6.5 mm clearance and >= 6.4 mm creepage from any mains copper to anything else.
(rule "Mains to SELV clearance"
\t(condition "A.NetClass == 'Mains' && B.NetClass != 'Mains'")
\t(constraint clearance (min 6.5mm)))
(rule "Mains to SELV creepage"
\t(condition "A.NetClass == 'Mains' && B.NetClass != 'Mains'")
\t(constraint creepage (min 6.4mm)))
# Functional spacing between mains nodes (L vs N side of the divider)
(rule "Mains internal"
\t(condition "A.NetClass == 'Mains' && B.NetClass == 'Mains'")
\t(constraint clearance (min 0.7mm)))
"""

if __name__ == "__main__":
    main()
