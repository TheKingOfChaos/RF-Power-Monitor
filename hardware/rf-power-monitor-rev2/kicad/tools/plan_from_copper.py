#!/usr/bin/env python3
"""Turn a routed board's tracks and vias back into an AI Flow plan (docs/plan-schema.md): segments of
one net, layer and width chained into polylines, vias as layer changes. Used to recover the grid
router's variant-1 plan from the board the gate wrote, then merge hand-closed paths into it.

usage: plan_from_copper.py <routed.kicad_pcb> <out.json> [extra-plan.json ...]
"""
import json
import sys
from collections import defaultdict

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sexp import parse, find, findall, uq  # noqa: E402


def key(p):
    return (round(p[0], 4), round(p[1], 4))


def main():
    b = parse(open(sys.argv[1]).read())
    groups = defaultdict(list)
    for s in findall(b, "segment"):
        st, en = find(s, "start"), find(s, "end")
        a, c = key((float(st[1]), float(st[2]))), key((float(en[1]), float(en[2])))
        if a == c:
            continue
        groups[(uq(find(s, "net")[1]), uq(find(s, "layer")[1]), float(find(s, "width")[1]))].append((a, c))
    entries = []
    for (net, layer, w), segs in sorted(groups.items()):
        adj = defaultdict(list)
        for i, (a, c) in enumerate(segs):
            adj[a].append((i, c))
            adj[c].append((i, a))
        used = set()
        paths = []
        # start chains at ends (degree != 2) first, then close loops
        order = sorted(adj, key=lambda p: len(adj[p]) == 2)
        for p0 in order:
            for i, q in adj[p0]:
                if i in used:
                    continue
                used.add(i)
                chain = [p0, q]
                while len(adj[chain[-1]]) == 2:
                    nxt = [(k, r) for k, r in adj[chain[-1]] if k not in used]
                    if not nxt:
                        break
                    k, r = nxt[0]
                    used.add(k)
                    chain.append(r)
                paths.append([{"x": x, "y": y, "layer": layer} for x, y in chain])
        entries.append({"net": net, "width": w, "paths": paths})
    for v in findall(b, "via"):
        a = find(v, "at")
        x, y = round(float(a[1]), 4), round(float(a[2]), 4)
        entries.append({"net": uq(find(v, "net")[1]), "width": 0.2, "viaSize": float(find(v, "size")[1]),
                        "viaDrill": float(find(v, "drill")[1]),
                        "paths": [[{"x": x, "y": y, "layer": "F.Cu"}, {"x": x, "y": y, "layer": "B.Cu"}]]})
    for extra in sys.argv[3:]:
        entries += json.load(open(extra))["nets"]
    json.dump({"nets": entries}, open(sys.argv[2], "w"), indent=1)
    print(f"{len(entries)} entries, {sum(len(e['paths']) for e in entries)} paths")


if __name__ == "__main__":
    main()
