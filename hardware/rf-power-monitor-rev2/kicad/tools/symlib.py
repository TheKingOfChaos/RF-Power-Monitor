"""Builds the project symbol library RFPM_Rev2.kicad_sym: flattened copies of the KiCad 10 stock
symbols the design uses, plus project symbols (RJ45, transformer, WIZPoE-P1, TMP1075, power)."""
import os
import re

from sexp import parse, dump, q, uq, find, findall

KICAD_SYM = os.environ.get("KICAD10_SYMBOL_DIR", "/mnt/c/Program Files/KiCad/10.0/share/kicad/symbols")
HERE = os.path.dirname(os.path.abspath(__file__))
REV1_SYM = os.path.join(HERE, "..", "..", "..", "pico-poe-mains-carrier", "PicoPoE_Carrier.kicad_sym")

_libcache = {}


def _lib(path):
    if path not in _libcache:
        _libcache[path] = parse(open(path, encoding="utf-8").read())
    return _libcache[path]


def _sym_node(lib_node, name):
    for s in findall(lib_node, "symbol"):
        if uq(s[1]) == name:
            return s
    raise KeyError(name)


def _rename(node, old, new):
    """rename a symbol node and its unit sub-symbols"""
    node[1] = q(new)
    for s in findall(node, "symbol"):
        n = uq(s[1])
        if n.startswith(old + "_"):
            s[1] = q(new + n[len(old):])
    return node


def stock(libname, name):
    """flattened copy of a stock symbol, named `name`"""
    lib = _lib(os.path.join(KICAD_SYM, libname + ".kicad_sym"))
    node = _sym_node(lib, name)
    ext = find(node, "extends")
    if ext is None:
        return _rename(parse(dump(node)), name, name)
    base = parse(dump(_sym_node(lib, uq(ext[1]))))
    base_name = uq(base[1])
    _rename(base, base_name, name)
    # child's properties override the base's
    child_props = {uq(p[1]): p for p in findall(node, "property")}
    out = []
    for x in base:
        if isinstance(x, list) and x and x[0] == "property" and uq(x[1]) in child_props:
            out.append(parse(dump(child_props.pop(uq(x[1])))))
        else:
            out.append(x)
    # remaining child properties go before the first sub-symbol
    idx = next(i for i, x in enumerate(out) if isinstance(x, list) and x and x[0] == "symbol")
    for p in child_props.values():
        out.insert(idx, parse(dump(p)))
    return out


def rev1(name):
    return parse(dump(_sym_node(_lib(REV1_SYM), name)))


# ---------------------------------------------------------------------------------------------
# project symbols
# ---------------------------------------------------------------------------------------------
def _font(s=1.27):
    return f"(effects (font (size {s} {s})))"


def _prop(k, v, x=0, y=0, hide=False):
    return f'(property {q(k)} {q(v)} (at {x} {y} 0){" (hide yes)" if hide else ""} {_font()})'


def _pin(t, x, y, ang, num, name, length=2.54, hide=False):
    return (f'(pin {t} line (at {x} {y} {ang}) (length {length}){" (hide yes)" if hide else ""} '
            f'(name {q(name)} {_font()}) (number {q(num)} {_font()}))')


def box(name, ref, value, w, left, right, top=(), bottom=(), descr="", fp="", ds="", pitch=2.54):
    """rectangular symbol; left/right: [(num, name, type)], top/bottom same. Origin at the centre."""
    rows = max(len(left), len(right), 1)
    h = (rows + 1) * pitch
    h = max(h, (max(len(top), len(bottom)) + 1) * pitch if (top or bottom) else h)
    hw, hh = w / 2, round(h / 2 / 1.27) * 1.27
    pins = []
    y0 = hh - pitch
    for i, (n, nm, t) in enumerate(left):
        if n is None:
            continue
        pins.append(_pin(t, -hw - 2.54, round(y0 - i * pitch, 4), 0, n, nm))
    for i, (n, nm, t) in enumerate(right):
        if n is None:
            continue
        pins.append(_pin(t, hw + 2.54, round(y0 - i * pitch, 4), 180, n, nm))
    for i, (n, nm, t) in enumerate(top):
        x = round(-((len(top) - 1) * pitch) / 2 + i * pitch, 4)
        pins.append(_pin(t, x, hh + 2.54, 270, n, nm))
    for i, (n, nm, t) in enumerate(bottom):
        x = round(-((len(bottom) - 1) * pitch) / 2 + i * pitch, 4)
        pins.append(_pin(t, x, -hh - 2.54, 90, n, nm))
    body = (f'(rectangle (start {-hw} {hh}) (end {hw} {-hh}) (stroke (width 0.254) (type default)) '
            f'(fill (type background)))')
    s = (f'(symbol {q(name)} (pin_names (offset 1.016)) (exclude_from_sim no) (in_bom yes) (on_board yes) '
         + _prop("Reference", ref, 0, hh + 1.27) + " " + _prop("Value", value, 0, -hh - 1.27) + " "
         + _prop("Footprint", fp, 0, 0, True) + " " + _prop("Datasheet", ds, 0, 0, True) + " "
         + _prop("Description", descr, 0, 0, True) + " "
         + f'(symbol {q(name + "_0_1")} {body}) (symbol {q(name + "_1_1")} {" ".join(pins)}))')
    return parse(s)


def power(name, kind="up"):
    """global power symbol; kind up (arrow), gnd, chassis"""
    if kind == "gnd":
        g = ("(polyline (pts (xy 0 0) (xy 0 -1.27) (xy 1.27 -1.27) (xy 0 -2.54) (xy -1.27 -1.27) (xy 0 -1.27)) "
             "(stroke (width 0) (type default)) (fill (type none)))")
        vy, ry = -3.81, -6.35
    elif kind == "chassis":
        g = ("(polyline (pts (xy 0 0) (xy 0 -1.27)) (stroke (width 0) (type default)) (fill (type none))) "
             "(polyline (pts (xy -1.905 -1.27) (xy 1.905 -1.27)) (stroke (width 0.254) (type default)) (fill (type none))) "
             "(polyline (pts (xy -1.905 -1.27) (xy -2.54 -2.54)) (stroke (width 0.254) (type default)) (fill (type none))) "
             "(polyline (pts (xy 0 -1.27) (xy -0.635 -2.54)) (stroke (width 0.254) (type default)) (fill (type none))) "
             "(polyline (pts (xy 1.905 -1.27) (xy 1.27 -2.54)) (stroke (width 0.254) (type default)) (fill (type none)))")
        vy, ry = -3.81, -6.35
    else:
        g = ("(polyline (pts (xy -0.762 1.27) (xy 0 2.54)) (stroke (width 0) (type default)) (fill (type none))) "
             "(polyline (pts (xy 0 0) (xy 0 2.54)) (stroke (width 0) (type default)) (fill (type none))) "
             "(polyline (pts (xy 0 2.54) (xy 0.762 1.27)) (stroke (width 0) (type default)) (fill (type none)))")
        vy, ry = 3.556, -3.81
    s = (f'(symbol {q(name)} (power global) (pin_names (offset 0)) (exclude_from_sim no) (in_bom no) (on_board no) '
         f'(property "Reference" "#PWR" (at 0 {ry} 0) (hide yes) {_font()}) '
         f'(property "Value" {q(name)} (at 0 {vy} 0) {_font()}) '
         f'(property "Footprint" "" (at 0 0 0) (hide yes) {_font()}) (property "Datasheet" "" (at 0 0 0) (hide yes) {_font()}) '
         f'(property "Description" {q("Power symbol " + name)} (at 0 0 0) (hide yes) {_font()}) '
         f'(symbol {q(name + "_0_1")} {g}) '
         f'(symbol {q(name + "_1_1")} (pin power_in line (at 0 0 {"270" if kind != "up" else "90"}) (length 0) (hide yes) '
         f'(name {q(name)} {_font()}) (number "1" {_font()}))))')
    return parse(s)


def pwr_flag():
    return stock("power", "PWR_FLAG")


def project_symbols():
    S = {}
    S["KH-RJ45-56-8P8C-D"] = box(
        "KH-RJ45-56-8P8C-D", "J", "RJ45", 15.24,
        [("1", "1 TX+", "passive"), ("2", "2 TX-", "passive"), ("3", "3 RX+", "passive"), ("6", "6 RX-", "passive"),
         ("4", "4", "passive"), ("5", "5", "passive"), ("7", "7", "passive"), ("8", "8", "passive")],
        [("12", "LED L+", "passive"), ("11", "LED L-", "passive"), ("10", "LED R+", "passive"), ("9", "LED R-", "passive")],
        bottom=[("13", "SHIELD", "passive"), ("14", "SHIELD", "passive")],
        descr="RJ45 8P8C jack, shielded, 2 LEDs, no magnetics (Kinghelm KH-RJ45-56-8P8C-D)",
        ds="https://www.lcsc.com/product-detail/C2828082.html")
    S["HR601680"] = box(
        "HR601680", "T", "HR601680", 15.24,
        [("1", "TD+", "passive"), ("2", "TCT", "passive"), ("3", "TD-", "passive"), ("4", "NC", "no_connect"),
         ("5", "NC", "no_connect"), ("6", "RD+", "passive"), ("7", "RCT", "passive"), ("8", "RD-", "passive")],
        [("16", "TX+", "passive"), ("15", "TXCT", "passive"), ("14", "TX-", "passive"), ("13", "NC", "no_connect"),
         ("12", "NC", "no_connect"), ("11", "RX+", "passive"), ("10", "RXCT", "passive"), ("9", "RX-", "passive")],
        descr="10/100BASE-T single port transformer 1CT:1CT, 350 uH, 1500 Vrms (HanRun HR601680)",
        ds="https://www.lcsc.com/product-detail/C242560.html")
    S["WIZPoE-P1"] = box(
        "WIZPoE-P1", "U", "WIZPoE-P1", 15.24,
        [("1", "VC1(+)", "passive"), ("2", "VC1(-)", "passive"), ("3", "VC2(+)", "passive"), ("4", "VC2(-)", "passive")],
        [("5", "+5V", "power_out"), ("6", "+5V", "passive"), ("7", "GND", "power_in"), ("8", "GND", "power_in")],
        descr="WIZnet WIZPoE-P1 isolated IEEE 802.3af PD module, 5 V 1.5 A; inputs not polarity sensitive",
        ds="https://docs.wiznet.io/Product/Modules/PoE/WIZPoE-P1")
    S["TMP1075"] = rev1("TMP1075")
    return S


# netlist "lib:name" -> project symbol name; (pin renames)
def build(netlist):
    syms = {}
    for c in netlist["components"]:
        lib, name = c["sym"].split(":")
        if name in syms:
            continue
        if lib == "RFPM_Rev2":
            continue
        syms[name] = stock(lib, name)
    syms.update(project_symbols())
    for n in ("GND",):
        syms[n] = power(n, "gnd")
    syms["CHASSIS"] = power("CHASSIS", "chassis")
    for n in ("+3V3", "VSYS", "VBUS", "3V3A", "+1V1"):
        syms[n] = power(n, "up")
    syms["PWR_FLAG"] = pwr_flag()
    return syms


def pins_of(sym):
    """{number: (x, y, angle, length, type, hidden)} in symbol coords (y up), for all units"""
    out = {}
    for sub in findall(sym, "symbol"):
        for p in findall(sub, "pin"):
            at = find(p, "at")
            num = uq(find(p, "number")[1])
            ln = float(find(p, "length")[1]) if find(p, "length") else 0
            hidden = any(isinstance(x, list) and x[0] == "hide" for x in p) or "hide" in p
            out.setdefault(num, (float(at[1]), float(at[2]), int(float(at[3])) if len(at) > 3 else 0, ln, p[1], hidden))
    return out


def lib_file(syms, prefix=""):
    body = " ".join(dump(s) for s in syms.values())
    return f'(kicad_symbol_lib (version 20251024) (generator "rfpm-rev2-gen") (generator_version "10.0") {body})\n'


def embedded(syms, libname):
    """lib_symbols block for the schematic: names prefixed with the library"""
    out = []
    for name, s in syms.items():
        c = parse(dump(s))
        c[1] = q(f"{libname}:{name}")
        out.append(dump(c))
    return "(lib_symbols " + " ".join(out) + ")"
