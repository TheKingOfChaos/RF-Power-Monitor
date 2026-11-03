#!/usr/bin/env python3
"""Project library entries for the parts that have no Adom wiki component page yet
(`adom-aiflow parts --project-lib kicad/lib`): copies the KiCad 10 stock footprint and STEP model
each one uses into lib/, and writes lib/provenance.json (mpn -> symbol, footprint, model, source)."""
import hashlib
import json
import os
import re
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
KDIR = os.path.dirname(HERE)
LIBD = os.path.join(KDIR, "lib")          # the KiCad project's own library (symbols built by gen_sch.py)
OUT = os.path.join(KDIR, "projlib")        # aiflow --project-lib: one entry per MPN, nothing else
PRETTY = os.path.join(OUT, "RFPM_Rev2_Parts.pretty")
D3 = os.path.join(OUT, "3d")
PROJ_D3 = os.path.join(LIBD, "3d")
FP_STOCK = os.environ.get("KICAD10_FOOTPRINT_DIR", "/mnt/c/Program Files/KiCad/10.0/share/kicad/footprints")
M_STOCK = os.environ.get("KICAD10_3DMODEL_DIR", "/mnt/c/Users/kenneth/AppData/Local/Programs/KiCad/10.0/share/kicad/3dmodels")
KICAD_LIB = "KiCad 10.0 standard library (CC-BY-SA 4.0 with the KiCad library exception)"

# mpn: (symbol, stock footprint "Lib:Name" or project footprint, model file or None (= stock model), source)
PARTS = {
    "0402B682K500NT": ("C", "Capacitor_SMD:C_0402_1005Metric", None, "FH (Fenghua) 0402B682K500NT, LCSC C1542"),
    "0402WGF1004TCE": ("R", "Resistor_SMD:R_0402_1005Metric", None, "UNI-ROYAL 0402WGF1004TCE, LCSC C26083"),
    "0402WGF1242TCE": ("R", "Resistor_SMD:R_0402_1005Metric", None, "UNI-ROYAL 0402WGF1242TCE, LCSC C11692"),
    "0402WGF499JTCE": ("R", "Resistor_SMD:R_0402_1005Metric", None, "UNI-ROYAL 0402WGF499JTCE, LCSC C25120"),
    "0402WGF5601TCE": ("R", "Resistor_SMD:R_0402_1005Metric", None, "UNI-ROYAL 0402WGF5601TCE, LCSC C25908"),
    "RC0402FR-0727RL": ("R", "Resistor_SMD:R_0402_1005Metric", None, "Yageo RC0402FR-0727RL, LCSC C138021"),
    "RC0402FR-073R3L": ("R", "Resistor_SMD:R_0402_1005Metric", None, "Yageo RC0402FR-073R3L, LCSC C137986"),
    "RC0402FR-07470RL": ("R", "Resistor_SMD:R_0402_1005Metric", None, "Yageo RC0402FR-07470RL, LCSC C114877"),
    "RC0402FR-0775RL": ("R", "Resistor_SMD:R_0402_1005Metric", None, "Yageo RC0402FR-0775RL, LCSC C114757"),
    "RC0402FR-0722KL": ("R", "Resistor_SMD:R_0402_1005Metric", None, "Yageo RC0402FR-0722KL, LCSC C82868"),
    "RC0603FR-07330RL": ("R", "Resistor_SMD:R_0603_1608Metric", None, "Yageo RC0603FR-07330RL, LCSC C105881"),
    "CL10B105KB8NQNC": ("C", "Capacitor_SMD:C_0603_1608Metric", None, "Samsung CL10B105KB8NQNC 1 uF 50 V X7R, LCSC C5199872"),
    "1206W4F6802T5E": ("R", "Resistor_SMD:R_1206_3216Metric", None, "UNI-ROYAL 1206W4F6802T5E 200 V, LCSC C25834"),
    "1808B102K202NT": ("C", "Capacitor_SMD:C_1808_4520Metric", None, "FH 1808B102K202NT 1 nF 2 kV X7R, LCSC C81332"),
    "CC0603KRX7R0BB103": ("C", "Capacitor_SMD:C_0603_1608Metric", None, "Yageo CC0603KRX7R0BB103 10 nF 100 V, LCSC C107059"),
    "CL21A106KAYNNNE": ("C", "Capacitor_SMD:C_0805_2012Metric", None, "Samsung CL21A106KAYNNNE 10 uF 25 V, LCSC C15850"),
    "19-217/BHC-ZL1M2RY/6T": ("LED", "LED_SMD:LED_0603_1608Metric", None, "Everlight 19-217/BHC-ZL1M2RY/6T blue 0603, LCSC C2986058"),
    "B5819W SL": ("D_Schottky", "Diode_SMD:D_SOD-123", None, "Slkor B5819W SL 40 V 1 A SOD-123, LCSC C8598"),
    "LTV-814S-TA1": ("LTV-814", "Package_DIP:SMDIP-4_W9.53mm", None, "Lite-On LTV-814S-TA1, LCSC C125118"),
    "WJ500V-5.08-2P": ("Screw_Terminal_01x02", "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-2-5.08_1x02_P5.08mm_Horizontal",
                       None, "Kangnex WJ500V-5.08-2P 5.08 mm 2-pole (Phoenix MKDS 1,5/2-5,08 land pattern), LCSC C8465"),
    "TYPE-C-31-M-12": ("USB_C_Receptacle_USB2.0_16P", "Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12",
                       "USB-C_SMD-TYPE-C-31-M-12_1.step", "Korean Hroparts TYPE-C-31-M-12, LCSC C165948; STEP from LCSC/EasyEDA"),
    "TS-1187A-B-A-B": ("SW_Push", "Button_Switch_SMD:SW_Push_1P1T_XKB_TS-1187A",
                       "SW-SMD_4P-L5.1-W5.1-P3.70-LS6.5-TL_H1.5.step", "XKB TS-1187A-B-A-B, LCSC C318884; STEP from LCSC/EasyEDA"),
    "SCT1032T1A250V": ("Fuse", "Fuse:Fuse_Schurter_UMT250", "FUSE-SMD_L10.3-W3.3.step",
                       "SETfuse SCT1032T1A250V, LCSC C19712559 (10.3 x 3.3 mm on the Schurter UMT 250 land pattern, as rev1); STEP from LCSC/EasyEDA"),
    "HR601680": ("HR601680", "RFPM_Rev2:SMD-16_L12.7-W7.2-P1.27-LS9.5-BL", "SMD-16_L12.7-W7.2-P1.27-LS9.5-BL.step",
                 "HanRun HR601680, LCSC C242560; footprint and STEP from LCSC/EasyEDA (easyeda2kicad 1.0.1), pads checked against the HanRun drawing (0.420 in rows, 0.050 in pitch)"),
    "KH-RJ45-56-8P8C-D": ("KH-RJ45-56-8P8C-D", "RFPM_Rev2:RJ45-TH_KH-RJ45-56-8P8C-D", "RJ45-TH_KH-RJ45-56-8P8C-D.step",
                          "Kinghelm KH-RJ45-56-8P8C-D, LCSC C2828082; footprint and STEP from LCSC/EasyEDA (easyeda2kicad 1.0.1)"),
    "SWPA3015S2R2MT": ("L", "RFPM_Rev2:IND-SMD_L3.0-W3.0", "IND-SMD_L3.0-W3.0-H1.5.step",
                       "Sunlord SWPA3015S2R2MT 2.2 uH, LCSC C43389; footprint and STEP from LCSC/EasyEDA (easyeda2kicad 1.0.1)"),
}


def sha(p):
    return hashlib.sha256(open(p, "rb").read()).hexdigest()


def sym_name(mpn):
    return re.sub(r"[^A-Za-z0-9_.+-]", "_", mpn)


def mpn_symbols():
    """one symbol per project-library MPN (a copy of the generic one, named after the MPN), so the
    PCM library has a distinct entry per part"""
    import sys
    sys.path.insert(0, HERE)
    from sexp import parse, dump, q, uq, find, findall
    base = parse(open(os.path.join(LIBD, "RFPM_Rev2.kicad_sym"), encoding="utf-8").read())
    by = {uq(s[1]): s for s in findall(base, "symbol")}
    out = []
    for mpn, (sym, fp, model, src) in PARTS.items():
        s = parse(dump(by[sym]))
        new = sym_name(mpn)
        s[1] = q(new)
        for sub in findall(s, "symbol"):
            n = uq(sub[1])
            if n.startswith(sym + "_"):
                sub[1] = q(new + n[len(sym):])
        for pr in findall(s, "property"):
            k = uq(pr[1])
            if k == "Value":
                pr[2] = q(mpn)
            elif k == "Footprint":
                pr[2] = q("RFPM_Rev2_Parts:" + fp.split(":")[1])
            elif k == "Description":
                pr[2] = q(src)
        out.append(dump(s))
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, "RFPM_Rev2_Parts.kicad_sym"), "w", encoding="utf-8").write(
        '(kicad_symbol_lib (version 20251024) (generator "rfpm-rev2-gen") (generator_version "10.0") '
        + " ".join(out) + ")\n")


def main():
    mpn_symbols()
    os.makedirs(PRETTY, exist_ok=True)
    os.makedirs(D3, exist_ok=True)
    os.makedirs(D3, exist_ok=True)
    out = {"parts": []}
    for mpn, (sym, fp, model, src) in PARTS.items():
        lib, name = fp.split(":")
        dst = os.path.join(PRETTY, name + ".kicad_mod")
        if lib != "RFPM_Rev2":
            src_fp = os.path.join(FP_STOCK, lib + ".pretty", name + ".kicad_mod")
            t = open(src_fp, encoding="utf-8").read()
            m = re.search(r'\(model "([^"]+)"', t)
            if model is None:
                stock_model = m.group(1).replace("${KICAD10_3DMODEL_DIR}/", "")
                mfile = os.path.basename(stock_model)
                if not os.path.exists(os.path.join(D3, mfile)):
                    shutil.copyfile(os.path.join(M_STOCK, stock_model), os.path.join(D3, mfile))
            else:
                mfile = model
                shutil.copyfile(os.path.join(PROJ_D3, model), os.path.join(D3, model))
            t = re.sub(r'\(model "[^"]+"', f'(model "${{KIPRJMOD}}/3d/{mfile}"', t) if m else t
            open(dst, "w", encoding="utf-8").write(t)
            fpsrc = f"{KICAD_LIB}: {fp}"
        else:
            mfile = model
            fpsrc = "LCSC/EasyEDA via easyeda2kicad 1.0.1"
            shutil.copyfile(os.path.join(LIBD, "RFPM_Rev2.pretty", name + ".kicad_mod"), dst)
        if lib == "RFPM_Rev2":
            shutil.copyfile(os.path.join(PROJ_D3, mfile), os.path.join(D3, mfile))
        mp = os.path.join(D3, mfile)
        out["parts"].append({"mpn": mpn, "symbol": sym_name(mpn), "footprint": name, "model": f"3d/{mfile}",
                             "source": src, "footprint_source": fpsrc,
                             "model_source": "KiCad 10.0 standard 3D library" if model is None else "LCSC/EasyEDA",
                             "model_sha256": sha(mp), "authoring": "manufacturer-supplied" if model else "source-derived",
                             "checked": "2026-11-02: pads against the datasheet land pattern; 3D binding to be checked in the 3D step"})
    json.dump(out, open(os.path.join(OUT, "provenance.json"), "w"), indent=1)
    print(len(out["parts"]), "project-library parts")


if __name__ == "__main__":
    main()
