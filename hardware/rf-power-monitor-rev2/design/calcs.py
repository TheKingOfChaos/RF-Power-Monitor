#!/usr/bin/env python3
"""RF-Power-Monitor rev2 design calculations -> calcs.json.

Numbers come from the datasheets in design/datasheets (TLV62569 SLVSDG1C, W5500 v1.1.0,
WIZPoE-P1 v1.0.1, HR601680 B/1) and from rev1 (mains section, unchanged)."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
out = {"items": [], "checks": []}


def item(name, value, unit, note, **kw):
    out["items"].append(dict(name=name, value=value, unit=unit, note=note, **kw))
    return value


def check(name, value, limit, ok, note=""):
    out["checks"].append({"name": name, "value": round(value, 4), "limit": limit, "pass": bool(ok), "note": note})


# --- load on 3V3 ------------------------------------------------------------------------------
i_w5500 = item("I W5500 100M transmitting", 0.132, "A", "W5500 ds 5.4 power dissipation", source="W5500 ds v1.1.0", kind="typ")
i_rp = item("I RP2040 + flash, 125 MHz both cores", 0.030, "A", "RP2040 ds ~25 mA typ + flash active", source="RP2040 datasheet", kind="typ")
i_misc = item("I LEDs, TMP1075, pull-ups", 0.015, "A", "3 LEDs ~3-4 mA each + small loads", kind="typ")
i_3v3 = i_w5500 + i_rp + i_misc
i_design = item("I 3V3 design load", 0.3, "A", "about 1.7x the typical sum, margin for firmware/LED changes")

# --- TLV62569 buck ----------------------------------------------------------------------------
vfb = {"name": "VFB TLV62569", "value": 0.6, "unit": "V", "source": "TLV62569 ds 6.5", "kind": "typ"}
vfb_min = {"name": "VFB min", "value": 0.588, "unit": "V", "source": "TLV62569 ds 6.5", "kind": "min", "worstCase": True}
vfb_max = {"name": "VFB max", "value": 0.612, "unit": "V", "source": "TLV62569 ds 6.5", "kind": "max", "worstCase": True}
out["items"] += [vfb, vfb_min, vfb_max]
R5, R6 = 100e3, 22e3   # stocked 1 % values
vout = 0.6 * (1 + R5 / R6)
item("VOUT nominal", round(vout, 4), "V", "0.6 V x (1 + R5/R6), R5 100k, R6 22k (ds eq. 2; R2 <= 200k)")
tol = 0.01
vmin = vfb_min["value"] * (1 + R5 * (1 - tol) / (R6 * (1 + tol)))
vmax = vfb_max["value"] * (1 + R5 * (1 + tol) / (R6 * (1 - tol)))
check("3V3 min (VFB min, 1 % resistors)", vmin, ">= 3.0 V (RP2040 IOVDD 1.62-3.63, W5500 2.97-3.63)", vmin >= 3.0)
check("3V3 max", vmax, "<= 3.6 V (W5500 3.63 V max)", vmax <= 3.6)
vin_min = item("VSYS min (USB)", 4.75 - 0.45, "V", "USB 4.75 V min minus B5819W ~0.45 V at 0.2 A", kind="min", worstCase=True,
               source="B5819W ds Vf 0.45 V @ 0.2 A")
vin_max = item("VSYS max (PoE)", 5.25, "V", "WIZPoE-P1 max output", source="WIZPoE-P1 ds", kind="max", worstCase=True)
check("VSYS max within TLV62569 VIN 5.5 V", vin_max, "<= 5.5 V", vin_max <= 5.5)
L, fsw = 2.2e-6, 1.5e6
d = vout / vin_max
ripple = vout * (1 - d) / (L * 0.7 * fsw)   # L -30 % tolerance (ds table 4 note 1), worst at max VIN
item("Inductor ripple worst (L -30 %, VIN 5.25)", round(ripple, 3), "A", "dI = VOUT (1-D) / (L f)")
ipk = i_design + ripple / 2
check("Inductor peak current vs SWPA3015S2R2MT Isat", ipk, "< 2.0 A (Isat, Sunlord)", ipk < 2.0 * 0.8)
eff = 0.88
p_buck = vout * i_3v3 * (1 / eff - 1)
item("Buck loss at typical load", round(p_buck, 3), "W", "~88 % at 0.18 A (ds fig. efficiency at 5 V in)")
p_ldo = (vin_max - vout) * i_3v3
item("LDO alternative loss", round(p_ldo, 3), "W", "why a buck: 4x less heat next to the TMP1075")

# --- PoE budget ---------------------------------------------------------------------------------
p5 = vout * i_3v3 / eff
i5 = p5 / 5.0
item("VSYS load (5 V)", round(i5, 3), "A", "3V3 load through the buck")
check("WIZPoE-P1 output current", i5, "<= 1.5 A", i5 <= 1.5)
item("WIZPoE-P1 minimum load", 0.150, "A", "ds 'Minimum Load' 150 mA min: typical VSYS load is near it; WIZnet's own EVB runs at the same load. Bench-check regulation at idle.", source="WIZPoE-P1 ds", kind="min")
p_in = p5 / 0.8
i_poe = p_in / 44.0
item("PoE input current at 44 V", round(i_poe, 4), "A", "module efficiency 80 % typ; this flows in the HR601680 line centre taps (Mode A)")
check("Transformer centre-tap DC", i_poe, "<< 350 mA (802.3af per pair)", i_poe < 0.1,
      "HR601680 is not PoE-rated; its 8 mA OCL bias test is far below, but the DC splits evenly across the two half windings so the core does not bias; 30 mA is far below any winding limit. Mode B (spare pairs) bypasses it.")

# --- W5500 crystal / RP2040 crystal -------------------------------------------------------------
cl, cs = 12e-12, 3e-12
cload = 2 * (cl - cs)
item("W5500 crystal caps", round(cload * 1e12, 1), "pF", "X322525MOB4SI CL 12 pF: C = 2 (CL - Cstray 3 pF) = 18 pF")
item("RP2040 crystal caps", 15, "pF", "ABM8-272-T3 CL 10 pF with 15 pF and 1k series, as on the Pico (RP2040 HW design guide)")

# --- LEDs ---------------------------------------------------------------------------------------
for name, r, vf in (("RJ45 link/act LED", 330, 2.0), ("status LED blue GP25", 470, 2.8)):
    i = (3.3 - vf) / r
    item(f"I {name}", round(i * 1000, 2), "mA", f"(3.3 - Vf {vf}) / {r}")

# --- mains (rev1, unchanged) ---------------------------------------------------------------------
ipk_m = 325 / (4 * 68e3)
item("Mains loop peak current", round(ipk_m * 1000, 2), "mA", "325 V peak / 272k; rev1, about 50 mW per 1206")
item("Mains isolation", 6.5, "mm", "clearance and 6.4 mm creepage to all other copper (rev1 .kicad_dru)")
item("MDI (PoE side) isolation", 2.0, "mm", "RJ45 contacts, line windings, VC nets, Bob Smith network to GND/SELV copper: 1500 Vrms (802.3 / HR601680 hipot)")

out["pass"] = all(c["pass"] for c in out["checks"])
json.dump(out, open(os.path.join(HERE, "calcs.json"), "w"), indent=1)
for c in out["checks"]:
    print(("PASS " if c["pass"] else "FAIL ") + f'{c["name"]}: {c["value"]} ({c["limit"]})')
for i in out["items"]:
    print(f'  {i["name"]}: {i["value"]} {i.get("unit", "")}')
