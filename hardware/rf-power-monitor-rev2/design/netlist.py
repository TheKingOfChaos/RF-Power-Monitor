#!/usr/bin/env python3
"""RF-Power-Monitor rev2: the single source of connectivity.

Writes netlist.json (components + nets) and bom.csv. The schematic, the board and the
BOM are generated from this file; nothing else retypes a connection.

Circuit sources:
- RP2040 core: Raspberry Pi Pico / "Hardware design with RP2040" (12 MHz crystal, W25Q16 flash,
  BOOTSEL on QSPI_SS, RUN button, USB 27R series).
- W5500 + magnetics: WIZnet W5500-EVB-Pico-PoE V1.1 schematic, with the W5500 datasheet v1.1.0
  where they differ (pin 23 RSVD to GND, 1V2O 10 nF).
- PoE: WIZnet WIZPoE-P1 (isolated 802.3af, 5 V out, own input bridges): VC1 = the two cable-side
  centre taps of a discrete 1CT:1CT transformer, VC2 = spare pairs 4/5 and 7/8 (datasheet pin table).
- Mains sense, TMP1075, factory reset: rev1 carrier (hardware/pico-poe-mains-carrier), unchanged.
Pin map: firmware unchanged (include/pins.h, board = pico).
"""
import csv
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = "RFPM_Rev2"   # project symbol + footprint library

comps = []
nets = {}


def part(ref, value, sym, fp, pins, lcsc=None, mpn=None, mfr=None, block="", note="", bom=True, extra=None):
    """pins: {pad: net}. A pad may appear on several footprint pads (same number)."""
    c = {"ref": ref, "value": value, "sym": sym, "fp": fp, "block": block}
    if lcsc:
        c["lcsc"] = lcsc
    if mpn:
        c["mpn"] = mpn
    if mfr:
        c["mfr"] = mfr
    if note:
        c["note"] = note
    if not bom:
        c["exclude_from_bom"] = True
    if extra:
        c.update(extra)
    c["pins"] = {str(k): v for k, v in pins.items()}
    comps.append(c)
    for pad, net in c["pins"].items():
        if net is None:
            continue
        nets.setdefault(net, []).append(f"{ref}.{pad}")


# --- common parts -------------------------------------------------------------------------
R0402 = "Resistor_SMD:R_0402_1005Metric"
C0402 = "Capacitor_SMD:C_0402_1005Metric"
C0603 = "Capacitor_SMD:C_0603_1608Metric"
C0805 = "Capacitor_SMD:C_0805_2012Metric"
RES = {  # value: (LCSC, MPN) - JLCPCB basic/extended 0402 thick film 1%
    "3.3R": ("C137986", "RC0402FR-073R3L"),
    "27R": ("C138021", "RC0402FR-0727RL"),
    "49.9R": ("C25120", "0402WGF499JTCE"),
    "75R": ("C114757", "RC0402FR-0775RL"),
    "330R": ("C25104", "0402WGF3300TCE"),
    "470R": ("C114877", "RC0402FR-07470RL"),
    "1k": ("C11702", "0402WGF1001TCE"),
    "4.7k": ("C25900", "0402WGF4701TCE"),
    "5.1k": ("C25905", "0402WGF5101TCE"),
    "5.6k": ("C25908", "0402WGF5601TCE"),
    "10k": ("C60490", "RC0402FR-0710KL"),
    "12.4k 1%": ("C11692", "0402WGF1242TCE"),
    "22k": ("C82868", "RC0402FR-0722KL"),
    "100k": ("C25741", "0402WGF1003TCE"),
    "1M": ("C26083", "0402WGF1004TCE"),
}
CAP = {  # value: (LCSC, MPN, footprint)
    "18pF": ("C1549", "0402CG180J500NT", C0402),
    "15pF": ("C1548", "0402CG150J500NT", C0402),
    "10nF": ("C15195", "CL05B103KB5NNNC", C0402),
    "100nF": ("C1525", "CL05B104KO5NNNC", C0402),
    "1uF": ("C52923", "CL05A105KA5NQNC", C0402),
    "4.7uF": ("C19666", "CL10A475KO8NNNC", C0603),
    "10uF": ("C15850", "CL21A106KAYNNNE", C0805),
    "22uF": ("C45783", "CL21A226MAQNNNE", C0805),
    "10nF 100V": ("C107059", "CC0603KRX7R0BB103", C0603),
    "1nF 2kV": ("C81332", "1808B102K202NT", "Capacitor_SMD:C_1808_4520Metric"),
}


def R(ref, value, a, b, block, note=""):
    lc, mpn = RES[value]
    part(ref, value, "Device:R", R0402, {1: a, 2: b}, lc, mpn, block=block, note=note)


def C(ref, value, a, b, block, note=""):
    lc, mpn, fp = CAP[value]
    part(ref, value, "Device:C", fp, {1: a, 2: b}, lc, mpn, block=block, note=note)


# --- power: USB-C, VSYS, 3V3 buck ---------------------------------------------------------
part("J1", "USB-C", "Connector:USB_C_Receptacle_USB2.0_16P", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
     {"A4": "VBUS", "A9": "VBUS", "B4": "VBUS", "B9": "VBUS",
      "A1": "GND", "A12": "GND", "B1": "GND", "B12": "GND", "SH": "GND",
      "A5": "USB_CC1", "B5": "USB_CC2", "A6": "USB_DP", "B6": "USB_DP", "A7": "USB_DN", "B7": "USB_DN",
      "A8": None, "B8": None},
     "C165948", "TYPE-C-31-M-12", "Korean Hroparts", block="power", note="USB 2.0 device: serial menu, flashing, bench power")
R("R1", "5.1k", "USB_CC1", "GND", "power", "UFP Rd")
R("R2", "5.1k", "USB_CC2", "GND", "power", "UFP Rd")
part("D1", "B5819W", "Device:D_Schottky", "Diode_SMD:D_SOD-123", {1: "VSYS", 2: "VBUS"},
     "C8598", "B5819W SL", "Slkor", block="power", note="VBUS to VSYS OR-ing: PoE 5 V never back-feeds USB")
R("R3", "5.6k", "VBUS", "VBUS_SENSE", "power", "VBUS sense to GP24 (as on the Pico)")
R("R4", "10k", "VBUS_SENSE", "GND", "power")
C("C1", "22uF", "VSYS", "GND", "power", "VSYS bulk")
C("C2", "22uF", "VSYS", "GND", "power", "VSYS bulk")
part("U1", "TLV62569DBVR", "Regulator_Switching:TLV62569DBV", "Package_TO_SOT_SMD:SOT-23-5",
     {1: "VSYS", 2: "GND", 3: "BUCK_SW", 4: "VSYS", 5: "BUCK_FB"},
     "C141836", "TLV62569DBVR", "Texas Instruments", block="power", note="2 A sync buck, 1.5 MHz: VSYS 4.4-5.25 V to 3V3")
C("C3", "10uF", "VSYS", "GND", "power", "buck input, at U1 pin 4")
part("L1", "2.2uH", "Device:L", f"{LIB}:IND-SMD_L3.0-W3.0", {1: "BUCK_SW", 2: "+3V3"},
     "C43389", "SWPA3015S2R2MT", "Sunlord", block="power", note="2.2 uH 2 A (TI table 4 standard LC)")
C("C4", "22uF", "+3V3", "GND", "power", "buck output (TI table 4: 10-47 uF)")
R("R5", "100k", "+3V3", "BUCK_FB", "power", "FB top: 0.6 V x (1 + 100k/22k) = 3.327 V")
R("R6", "22k", "BUCK_FB", "GND", "power", "FB bottom")

# --- PoE: RJ45 (no magnetics), discrete 1CT:1CT transformer, WIZPoE-P1 ---------------------
part("J2", "RJ45", f"{LIB}:KH-RJ45-56-8P8C-D", f"{LIB}:RJ45-TH_KH-RJ45-56-8P8C-D",
     {1: "MDI_TX_P", 2: "MDI_TX_N", 3: "MDI_RX_P", 6: "MDI_RX_N", 4: "POE_SP45", 5: "POE_SP45",
      7: "POE_SP78", 8: "POE_SP78", 13: "CHASSIS", 14: "CHASSIS",
      12: "LED_LINK_A", 11: "LED_LINK_K", 10: "LED_ACT_A", 9: "LED_ACT_K"},
     "C2828082", "KH-RJ45-56-8P8C-D", "Kinghelm", block="poe", note="shielded, LEDs, no magnetics; MDI (W5500 has no auto-MDIX)")
part("T1", "HR601680", f"{LIB}:HR601680", f"{LIB}:SMD-16_L12.7-W7.2-P1.27-LS9.5-BL",
     {1: "ETH_TD_P", 2: "3V3A", 3: "ETH_TD_N", 6: "ETH_RD_P", 7: "3V3A", 8: "ETH_RD_N",
      16: "MDI_TX_P", 15: "POE_VC1A", 14: "MDI_TX_N", 11: "MDI_RX_P", 10: "POE_VC1B", 9: "MDI_RX_N"},
     "C242560", "HR601680", "HanRun", block="poe", note="10/100 1CT:1CT 350 uH, 1500 Vrms; line CTs carry PoE (about 30 mA here)")
part("U2", "WIZPoE-P1", f"{LIB}:WIZPoE-P1", f"{LIB}:WIZPoE-P1",
     {1: "POE_VC1A", 2: "POE_VC1B", 3: "POE_SP45", 4: "POE_SP78", 5: "VSYS", 6: "VSYS", 7: "GND", 8: "GND"},
     None, "WIZPoE-P1", "WIZnet", block="poe", bom=False,
     note="isolated 802.3af PD module, 5 V 1.5 A; soldered in or on 2x 1x4 sockets; fitted by hand")
# Bob Smith termination: 75R + 10 nF (100 V, DC block for PoE) per pair to one node, 1 nF 2 kV to chassis
R("R7", "75R", "POE_VC1A", "BS1", "poe")
R("R8", "75R", "POE_VC1B", "BS2", "poe")
R("R9", "75R", "POE_SP45", "BS3", "poe")
R("R10", "75R", "POE_SP78", "BS4", "poe")
C("C5", "10nF 100V", "BS1", "BS_COM", "poe")
C("C6", "10nF 100V", "BS2", "BS_COM", "poe")
C("C7", "10nF 100V", "BS3", "BS_COM", "poe")
C("C8", "10nF 100V", "BS4", "BS_COM", "poe")
C("C9", "1nF 2kV", "BS_COM", "CHASSIS", "poe", "Bob Smith to chassis")
C("C10", "1nF 2kV", "CHASSIS", "GND", "poe", "chassis to logic GND (as WIZnet C3)")

# --- W5500 Ethernet -------------------------------------------------------------------------
w = {1: "W_TXN", 2: "W_TXP", 5: "W_RXN", 6: "W_RXP",
     4: "3V3A", 8: "3V3A", 11: "3V3A", 15: "3V3A", 17: "3V3A", 21: "3V3A",
     3: "GND", 9: "GND", 14: "GND", 16: "GND", 19: "GND", 48: "GND", 29: "GND", 23: "GND",
     28: "+3V3", 43: "+3V3", 44: "+3V3", 45: "+3V3",
     10: "W_EXRES", 20: "W_TOCAP", 22: "W_1V2O", 30: "W_XI", 31: "W_XO",
     32: "SPI_CS", 33: "SPI_SCK", 34: "SPI_MISO", 35: "SPI_MOSI", 36: "W_INT", 37: "W_RST",
     25: "LED_LINK_K", 27: "LED_ACT_K",
     7: None, 12: None, 13: None, 18: None, 24: None, 26: None, 38: None, 39: None, 40: None,
     41: None, 42: None, 46: None, 47: None}
part("U3", "W5500", "Interface_Ethernet:W5500", "Package_QFP:LQFP-48_7x7mm_P0.5mm", w,
     "C32843", "W5500", "WIZnet", block="eth", note="PMODE 111: all capable, auto-negotiation")
part("FB1", "120R@100MHz", "Device:FerriteBead_Small", "Inductor_SMD:L_0603_1608Metric", {1: "+3V3", 2: "3V3A"},
     "C12391", "GZ1608D121TF", "Sunlord", block="eth", note="3V3 to W5500 analog supply")
C("C11", "4.7uF", "3V3A", "GND", "eth", "AVDD bulk")
for i, ref in enumerate(["C12", "C13", "C14", "C15", "C16", "C17"]):
    C(ref, "100nF", "3V3A", "GND", "eth", "AVDD, one per pin 4/8/11/15/17/21")
C("C18", "100nF", "+3V3", "GND", "eth", "VDD pin 28")
C("C19", "4.7uF", "W_TOCAP", "GND", "eth", "TOCAP: datasheet 4.7 uF, short trace")
C("C20", "10nF", "W_1V2O", "GND", "eth", "1V2O: datasheet 10 nF")
R("R11", "12.4k 1%", "W_EXRES", "GND", "eth", "EXRES1 to AGND, datasheet")
part("Y1", "25MHz", "Device:Crystal_GND24", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
     {1: "W_XI", 2: "GND", 3: "W_XO_C", 4: "GND"}, "C9006", "X322525MOB4SI", "YXC", block="eth",
     note="CL 12 pF, +-10 ppm: 2 x 18 pF + ~3 pF stray")
R("R12", "1M", "W_XI", "W_XO", "eth", "crystal bias (WIZnet R24)")
part("R13", "0R", "Device:R", R0402, {1: "W_XO", 2: "W_XO_C"}, "C17168", "0402WGF0000TCE", block="eth",
     note="drive-level option (WIZnet R23)")
C("C21", "18pF", "W_XI", "GND", "eth")
C("C22", "18pF", "W_XO_C", "GND", "eth")
R("R14", "4.7k", "+3V3", "SPI_CS", "eth", "SCSn idle high while RP2040 boots")
R("R15", "4.7k", "+3V3", "W_INT", "eth")
R("R16", "4.7k", "+3V3", "W_RST", "eth")
# TX: 3.3R series, 49.9R to the transformer centre-tap supply; RX: 6.8 nF AC coupling, 49.9R to a 100 nF node
R("R17", "3.3R", "W_TXP", "ETH_TD_P", "eth")
R("R18", "3.3R", "W_TXN", "ETH_TD_N", "eth")
R("R19", "49.9R", "ETH_TD_P", "3V3A", "eth")
R("R20", "49.9R", "ETH_TD_N", "3V3A", "eth")
part("C23", "6.8nF", "Device:C", C0402, {1: "ETH_RD_P", 2: "ETH_RXC_P"}, "C1542", "0402B682K500NT", block="eth")
part("C24", "6.8nF", "Device:C", C0402, {1: "ETH_RD_N", 2: "ETH_RXC_N"}, "C1542", "0402B682K500NT", block="eth")
R("R21", "3.3R", "ETH_RXC_P", "W_RXP", "eth")
R("R22", "3.3R", "ETH_RXC_N", "W_RXN", "eth")
R("R23", "49.9R", "ETH_RXC_P", "ETH_RX_TERM", "eth")
R("R24", "49.9R", "ETH_RXC_N", "ETH_RX_TERM", "eth")
C("C25", "100nF", "ETH_RX_TERM", "GND", "eth")
C("C26", "100nF", "3V3A", "GND", "eth", "transformer centre taps")
R("R25", "330R", "+3V3", "LED_LINK_A", "eth", "link LED (RJ45 left)")
R("R26", "330R", "+3V3", "LED_ACT_A", "eth", "activity LED (RJ45 right)")

# --- RP2040, flash, crystal, buttons, SWD ---------------------------------------------------
u = {1: "+3V3", 10: "+3V3", 22: "+3V3", 33: "+3V3", 42: "+3V3", 49: "+3V3",
     43: "+3V3", 44: "+3V3", 48: "+3V3", 45: "+1V1", 23: "+1V1", 50: "+1V1", 57: "GND",
     19: "GND", 20: "XIN", 21: "XOUT", 24: "SWCLK", 25: "SWDIO", 26: "RUN",
     46: "USB_DN_MCU", 47: "USB_DP_MCU",
     51: "QSPI_SD3", 52: "QSPI_SCLK", 53: "QSPI_SD0", 54: "QSPI_SD2", 55: "QSPI_SD1", 56: "QSPI_SS",
     6: "I2C_SDA", 7: "I2C_SCL", 27: "SPI_MISO", 28: "SPI_CS", 29: "SPI_SCK", 30: "SPI_MOSI",
     31: "W_RST", 32: "W_INT", 34: "FACTORY_RESET", 36: "VBUS_SENSE", 37: "LED_STATUS", 39: "MAINS_OK"}
for p in (2, 3, 4, 5, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 35, 38, 40, 41):
    u[p] = None
part("U4", "RP2040", "MCU_RaspberryPi:RP2040", "Package_DFN_QFN:QFN-56-1EP_7x7mm_P0.4mm_EP3.2x3.2mm", u,
     "C2040", "RP2040", "Raspberry Pi", block="mcu", note="firmware board = pico")
for ref in ["C27", "C28", "C29", "C30", "C31", "C32"]:
    C(ref, "100nF", "+3V3", "GND", "mcu", "IOVDD" if ref == "C27" else "")
C("C33", "100nF", "+3V3", "GND", "mcu", "ADC_AVDD")
C("C34", "100nF", "+3V3", "GND", "mcu", "USB_VDD")
C("C35", "1uF", "+3V3", "GND", "mcu", "VREG_VIN")
C("C36", "1uF", "+1V1", "GND", "mcu", "VREG_VOUT")
C("C37", "100nF", "+1V1", "GND", "mcu", "DVDD pin 23")
C("C38", "100nF", "+1V1", "GND", "mcu", "DVDD pin 50")
C("C39", "10uF", "+3V3", "GND", "mcu", "3V3 bulk near RP2040")
R("R27", "27R", "USB_DP_MCU", "USB_DP", "mcu", "USB series (Pico)")
R("R28", "27R", "USB_DN_MCU", "USB_DN", "mcu", "USB series (Pico)")
part("Y2", "12MHz", "Device:Crystal_GND24", "Crystal:Crystal_SMD_3225-4Pin_3.2x2.5mm",
     {1: "XIN", 2: "GND", 3: "XOSC_C", 4: "GND"}, "C20625731", "ABM8-272-T3", "Abracon", block="mcu",
     note="as on the Pico (CL 10 pF, 2 x 15 pF, 1k series)")
R("R29", "1k", "XOUT", "XOSC_C", "mcu", "XOUT series, Pico")
C("C40", "15pF", "XIN", "GND", "mcu")
C("C41", "15pF", "XOSC_C", "GND", "mcu")
part("U5", "W25Q16JVSSIQ", "Memory_Flash:W25Q16JVSS", "Package_SO:SOIC-8_5.3x5.3mm_P1.27mm",
     {1: "QSPI_SS", 2: "QSPI_SD1", 3: "QSPI_SD2", 4: "GND", 5: "QSPI_SD0", 6: "QSPI_SCLK", 7: "QSPI_SD3", 8: "+3V3"},
     "C131025", "W25Q16JVSSIQ", "Winbond", block="mcu", note="2 MB, matches board = pico")
C("C42", "100nF", "+3V3", "GND", "mcu", "flash")
TS1187 = "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A"
R("R30", "1k", "QSPI_SS", "BOOTSEL_SW", "mcu", "BOOTSEL: hold at reset for USB boot (Pico)")
part("SW1", "BOOTSEL", "Switch:SW_Push", TS1187, {1: "BOOTSEL_SW", 2: "GND"}, "C318884", "TS-1187A-B-A-B", "XKB",
     block="mcu")
R("R31", "1k", "RUN", "RUN_SW", "mcu", "RUN: internal pull-up in RP2040")
part("SW2", "RESET", "Switch:SW_Push", TS1187, {1: "RUN_SW", 2: "GND"}, "C318884", "TS-1187A-B-A-B", "XKB",
     block="mcu")
part("J3", "SWD", "Connector_Generic:Conn_01x03", "Connector_PinHeader_2.54mm:PinHeader_1x03_P2.54mm_Vertical",
     {1: "SWCLK", 2: "GND", 3: "SWDIO"}, None, None, block="mcu", bom=False, note="Pico SWD order; not fitted")
R("R32", "470R", "LED_STATUS", "LED_STATUS_A", "mcu", "GP25 status (LED_BUILTIN)")
part("D2", "Blue", "Device:LED", "LED_SMD:LED_0603_1608Metric", {1: "GND", 2: "LED_STATUS_A"},
     "C2986058", "19-217/BHC-ZL1M2RY/6T", "Everlight", block="mcu", note="status, GP25")

# --- mains sense (rev1, unchanged values) ---------------------------------------------------
R1206 = "Resistor_SMD:R_1206_3216Metric"
part("J4", "230VAC L/N", "Connector:Screw_Terminal_01x02",
     "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2-5.08_1x02_P5.08mm_Horizontal",
     {1: "AC_L", 2: "AC_N"}, "C8465", "WJ500V-5.08-2P", "Kangnex", block="mains")
part("F1", "T1A 250VAC", "Device:Fuse", "Fuse:Fuse_Schurter_UMT250", {1: "L_FUSED", 2: "AC_L"},
     "C19712559", "SCT1032T1A250V", "SETfuse", block="mains")
for ref, a, b in (("R33", "OPTO_A", "L_FUSED"), ("R34", "N_MID1", "AC_N"), ("R35", "N_MID2", "N_MID1"),
                  ("R36", "OPTO_K", "N_MID2")):
    part(ref, "68k 1206 200V", "Device:R", R1206, {1: a, 2: b}, "C25834", "1206W4F6802T5E", block="mains",
         note="four in series: 1.2 mA peak, 50 mW each")
part("U6", "LTV-814S", "Isolator:LTV-814", "Package_DIP:SMDIP-4_W9.53mm",
     {1: "OPTO_A", 2: "OPTO_K", 3: "GND", 4: "SENSE"}, "C125118", "LTV-814S-TA1", "Lite-On", block="mains",
     note="AC input optocoupler, 5 kVrms")
part("C43", "1uF", "Device:C", C0603, {1: "SENSE", 2: "GND"}, "C5199872", "CL10B105KB8NQNC", block="mains",
     note="bridges zero crossings with R37")
part("R37", "47k", "Device:R", "Resistor_SMD:R_0603_1608Metric", {1: "SENSE", 2: "+3V3"}, "C25819",
     "0603WAF4702T5E", block="mains")
part("U7", "SN74LVC1G14DBVR", "74xGxx:74LVC1G14", "Package_TO_SOT_SMD:SOT-23-5",
     {1: None, 2: "SENSE", 3: "GND", 4: "MAINS_OK", 5: "+3V3"}, "C7835", "SN74LVC1G14DBVR", "Texas Instruments",
     block="mains", note="Schmitt inverter: HIGH = mains present")
C("C44", "100nF", "+3V3", "GND", "mains", "U7")
part("R38", "100R", "Device:R", "Resistor_SMD:R_0603_1608Metric", {1: "LED_G", 2: "MAINS_OK"}, "C22775",
     "0603WAF1000T5E", block="mains")
part("D3", "Green", "Device:LED", "LED_SMD:LED_0805_2012Metric", {1: "GND", 2: "LED_G"}, "C2297",
     "KT-0805G", block="mains", note="mains ON")
part("D4", "Red", "Device:LED", "LED_SMD:LED_0805_2012Metric", {1: "MAINS_OK", 2: "LED_R"}, "C84256",
     "NCD0805R1", block="mains", note="mains OFF")
part("R39", "330R", "Device:R", "Resistor_SMD:R_0603_1608Metric", {1: "+3V3", 2: "LED_R"}, "C105881",
     "RC0603FR-07330RL", block="mains")

# --- ambient temperature, factory reset -----------------------------------------------------
part("U8", "TMP1075DR", "RFPM_Rev2:TMP1075", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm",
     {1: "I2C_SDA", 2: "I2C_SCL", 3: None, 4: "GND", 5: "GND", 6: "GND", 7: "GND", 8: "+3V3"},
     "C2878381", "TMP1075DR", "Texas Instruments", block="sensor", note="I2C0 0x48, away from U1/U3/U4 heat")
C("C45", "100nF", "+3V3", "GND", "sensor", "U8")
R("R40", "4.7k", "+3V3", "I2C_SDA", "sensor")
R("R41", "4.7k", "+3V3", "I2C_SCL", "sensor")
part("SW3", "FACTORY RESET", "Switch:SW_Push", TS1187, {1: "FACTORY_RESET", 2: "GND"}, "C318884",
     "TS-1187A-B-A-B", "XKB", block="sensor", note="GP22 to GND, hold 10 s (internal pull-up)")

# --- test pads (aiflow test plan, design/tests.json): adom/testpoint-pad-0-5mm --------------
_tests = json.load(open(os.path.join(HERE, "tests.json")))
_tp = 0
for _t in _tests:
    if _t["kind"] in ("dc", "ripple", "waveform"):
        _tp += 1
        part(f"TP{_tp}", "TP_0.5mm", "Connector:TestPoint", f"{LIB}:TestPoint_Pad_D0.5mm", {1: _t["net"]},
             None, "Adom_TestPoint_D0.5mm", block="test", bom=False, note=f'probe spot for {_t["net"]} (test plan {_t["id"]})',
             extra={"page": "adom/testpoint-pad-0-5mm"})

# --- mechanics ------------------------------------------------------------------------------
for i in range(1, 5):
    part(f"H{i}", "M3", "Mechanical:MountingHole", "MountingHole:MountingHole_3.2mm_M3", {}, bom=False,
         block="mech")

out = {"design": "rf-power-monitor-rev2", "lib": LIB, "components": comps,
       "nets": {k: v for k, v in sorted(nets.items())}}
json.dump(out, open(os.path.join(HERE, "netlist.json"), "w"), indent=1)

with open(os.path.join(HERE, "bom.csv"), "w", newline="") as fh:
    wr = csv.writer(fh)
    snap = {}
    sp = os.path.join(HERE, "sourcing", "lcsc_2026-11-02.json")
    if os.path.exists(sp):
        snap = json.load(open(sp))
    wr.writerow(["ref", "value", "function", "mpn", "manufacturer", "package", "source", "lcsc",
                 "stock_checked_2026-11-02", "mpn_url", "note"])
    for c in comps:
        if c.get("exclude_from_bom") or not c.get("mpn") and not c.get("lcsc"):
            continue
        wr.writerow([c["ref"], c["value"], c["block"], c.get("mpn", ""), c.get("mfr", ""),
                     c["fp"].split(":")[1], "LCSC" if c.get("lcsc") else "", c.get("lcsc", ""),
                     (snap.get(c.get("lcsc"), {}) or {}).get("stock", ""),
                     f'https://www.lcsc.com/product-detail/{c["lcsc"]}.html' if c.get("lcsc") else "", c.get("note", "")])

single = [n for n, p in nets.items() if len(p) < 2]
print(f"{len(comps)} parts, {len(nets)} nets, {sum(len(v) for v in nets.values())} pins")
if single:
    print("single-pin nets:", single)
