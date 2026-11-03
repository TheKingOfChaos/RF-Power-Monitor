#!/usr/bin/env python3
"""Switch-node model of the TLV62569 output stage (ideal synchronous switch at 1.5 MHz, duty from the
3.33 V target) into L1 2.2 uH (-30 %) / C4 22 uF (DC-bias derated to 10 uF, ESR 5 mOhm), worst case
VIN 5.25 V, load step 0.05 -> 0.30 A. It checks the LC choice (ripple, inductor peak), not the loop:
the loop is TI's (table 4 standard LC, internal compensation). Writes results.json."""
import json, os, re, subprocess
HERE = os.path.dirname(os.path.abspath(__file__))
vin, vout, f = 5.25, 3.327, 1.5e6
d = vout / vin
cir = f"""* buck LC
Vsw sw 0 PULSE(0 {vin} 0 2n 2n {d / f:.4e} {1 / f:.4e})
L1 sw out {2.2e-6 * 0.7}
Rl out mid 0.005
C4 mid 0 10u IC={vout}
Rload out 0 {vout / 0.05}
Sstep out sl ctl 0 swmod
Rstep sl 0 {vout / 0.25}
Vctl ctl 0 PWL(0 0 300u 0 300.1u 1)
.model swmod sw(vt=0.5 ron=0.01 roff=1e9)
.ic v(out)={vout}
.tran 5n 500u 0 5n uic
.control
run
meas tran rip_light pp v(out) from=250u to=290u
meas tran rip_full pp v(out) from=450u to=490u
meas tran ilpk max i(L1) from=450u to=490u
meas tran vavg avg v(out) from=450u to=490u
wrdata {os.path.join(HERE, "buck_lc.dat")} v(out)
.endc
.end
"""
open(os.path.join(HERE, "buck_lc.cir"), "w").write(cir)
r = subprocess.run(["ngspice", "-b", os.path.join(HERE, "buck_lc.cir")], capture_output=True, text=True)
vals = {k: float(v) for k, v in re.findall(r"^(\w+)\s+=\s+([-\d.e+]+)", r.stdout, re.M)}
# switching ripple: peak-to-peak within each 1.5 MHz period after removing that period's mean slope
# (the open-loop LC rings at ~40 kHz after a step; the real regulator damps that, so it is not ripple)
data = [tuple(map(float, l.split()[:2])) for l in open(os.path.join(HERE, "buck_lc.dat")) if l.strip()]
def sw_ripple(t0, t1):
    T = 1 / f
    worst = 0.0
    k = t0
    while k + T <= t1:
        seg = [(a, b) for a, b in data if k <= a < k + T]
        if len(seg) > 10:
            (ta, va), (tb, vb) = seg[0], seg[-1]
            slope = (vb - va) / (tb - ta) if tb > ta else 0
            res = [b - (va + slope * (a - ta)) for a, b in seg]
            worst = max(worst, max(res) - min(res))
        k += T
    return worst
vals["rip_full"] = sw_ripple(450e-6, 490e-6)
vals["rip_light"] = sw_ripple(250e-6, 290e-6)
checks = [
    {"name": "3V3 ripple at 0.30 A, VIN 5.25 V (open-loop LC, 1.5 MHz)", "value": vals["rip_full"], "limit": "< 0.030 V", "pass": vals["rip_full"] < 0.030},
    {"name": "3V3 ripple at 0.05 A", "value": vals["rip_light"], "limit": "< 0.030 V", "pass": vals["rip_light"] < 0.030},
    {"name": "inductor peak current at 0.30 A", "value": vals["ilpk"], "limit": "< 1.6 A (Isat 2 A x 0.8)", "pass": vals["ilpk"] < 1.6},
]
out = {"tool": "ngspice " + r.stdout.split("ngspice-")[1].split()[0] if "ngspice-" in r.stdout else "ngspice",
       "model": "ideal switch-node square wave into the output LC; fitted: none", "values": vals, "checks": checks,
       "pass": all(c["pass"] for c in checks)}
json.dump(out, open(os.path.join(HERE, "results.json"), "w"), indent=1)
print(json.dumps(out, indent=1))
