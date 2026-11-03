#!/usr/bin/env python3
"""Close the last few connections the grid router left open, on a routed board.

A small two-layer A* on a 0.05 mm grid: every other net's copper (pads, tracks, vias) is an obstacle
grown by clearance + half the track width; a via needs room on both layers. It writes plan entries
in AI Flow's plan schema (docs/plan-schema.md) so the result goes through `gate` and `land route`
like the router's own plan.

usage: finish_routes.py <routed.kicad_pcb> <out.json> NET:PAD_A:PAD_B [...]
       (PAD_B may be "copper" to reach the net's nearest existing copper)
"""
import heapq
import json
import math
import sys

import numpy as np

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from sexp import parse, find, findall, uq  # noqa: E402

RES = 0.05
CLR = 0.11          # 0.1 mm rule + margin
W = 0.2             # track
STUB_W = 0.15       # first/last 0.6 mm at a fine-pitch pad
VIA, DRILL = 0.6, 0.3
VIA_COST = 40
LAYERS = ("F.Cu", "B.Cu")


def load(path):
    b = parse(open(path).read())
    pads, segs, vias = [], [], []
    for fp in findall(b, "footprint"):
        at = find(fp, "at")
        X, Y = float(at[1]), float(at[2])
        rot = float(at[3]) if len(at) > 3 else 0.0
        ref = uq([p for p in findall(fp, "property") if uq(p[1]) == "Reference"][0][2])
        t = math.radians(rot)
        for p in findall(fp, "pad"):
            a = find(p, "at")
            x, y = float(a[1]), float(a[2])
            pr = float(a[3]) if len(a) > 3 else rot
            gx, gy = X + x * math.cos(t) + y * math.sin(t), Y - x * math.sin(t) + y * math.cos(t)
            sz = find(p, "size")
            lay = [uq(v) for v in find(p, "layers")[1:]]
            ls = [l for l in LAYERS if l in lay or "*.Cu" in lay]
            n = find(p, "net")
            drill = find(p, "drill")
            pads.append(dict(ref=ref, num=uq(p[1]), x=gx, y=gy, w=float(sz[1]), h=float(sz[2]), rot=pr,
                             layers=ls, net=uq(n[1]) if n else "", drill=float(drill[1]) if drill and len(drill) > 1 and drill[1] not in ("oval",) else 0))
    for s in findall(b, "segment"):
        st, en = find(s, "start"), find(s, "end")
        segs.append(dict(a=(float(st[1]), float(st[2])), b=(float(en[1]), float(en[2])), w=float(find(s, "width")[1]),
                         layer=uq(find(s, "layer")[1]), net=uq(find(s, "net")[1])))
    for v in findall(b, "via"):
        a = find(v, "at")
        vias.append(dict(x=float(a[1]), y=float(a[2]), size=float(find(v, "size")[1]), net=uq(find(v, "net")[1])))
    return pads, segs, vias


class Grid:
    def __init__(self, x0, y0, x1, y1):
        self.x0, self.y0 = x0, y0
        self.nx, self.ny = int((x1 - x0) / RES) + 1, int((y1 - y0) / RES) + 1
        xs = x0 + np.arange(self.nx) * RES
        ys = y0 + np.arange(self.ny) * RES
        self.X, self.Y = np.meshgrid(xs, ys)

    def cell(self, x, y):
        return int(round((y - self.y0) / RES)), int(round((x - self.x0) / RES))

    def xy(self, j, i):
        return round(self.x0 + i * RES, 4), round(self.y0 + j * RES, 4)


def dist_rect(G, p):
    """distance from every grid point to the pad (rotated rectangle)"""
    t = math.radians(p["rot"])
    dx, dy = G.X - p["x"], G.Y - p["y"]
    lx = dx * math.cos(t) - dy * math.sin(t)
    ly = dx * math.sin(t) + dy * math.cos(t)
    ex = np.maximum(np.abs(lx) - p["w"] / 2, 0)
    ey = np.maximum(np.abs(ly) - p["h"] / 2, 0)
    return np.hypot(ex, ey)


def dist_seg(G, a, b):
    ax, ay = a
    bx, by = b
    vx, vy = bx - ax, by - ay
    l2 = vx * vx + vy * vy
    t = np.clip(((G.X - ax) * vx + (G.Y - ay) * vy) / l2, 0, 1) if l2 > 0 else 0
    return np.hypot(G.X - ax - t * vx, G.Y - ay - t * vy)


def obstacles(G, pads, segs, vias, net):
    """per layer: distance from each cell to the nearest foreign copper; and to the nearest hole"""
    d = [np.full(G.X.shape, 99.0), np.full(G.X.shape, 99.0)]
    hole = np.full(G.X.shape, 99.0)
    for p in pads:
        if p["net"] == net:
            continue
        dd = dist_rect(G, p)
        for k, l in enumerate(LAYERS):
            if l in p["layers"]:
                d[k] = np.minimum(d[k], dd)
        if p["drill"]:
            hole = np.minimum(hole, np.hypot(G.X - p["x"], G.Y - p["y"]) - p["drill"] / 2)
    for s in segs:
        if s["net"] == net:
            continue
        k = LAYERS.index(s["layer"])
        d[k] = np.minimum(d[k], dist_seg(G, s["a"], s["b"]) - s["w"] / 2)
    for v in vias:
        dd = np.hypot(G.X - v["x"], G.Y - v["y"]) - v["size"] / 2
        if v["net"] != net:
            d[0] = np.minimum(d[0], dd)
            d[1] = np.minimum(d[1], dd)
        hole = np.minimum(hole, np.hypot(G.X - v["x"], G.Y - v["y"]) - 0.15)
    return d, hole


def own_copper(G, pads, segs, vias, net):
    """cells (per layer) on the net's existing copper: targets for 'copper'"""
    m = [np.zeros(G.X.shape, bool), np.zeros(G.X.shape, bool)]
    for s in segs:
        if s["net"] == net:
            k = LAYERS.index(s["layer"])
            m[k] |= dist_seg(G, s["a"], s["b"]) <= s["w"] / 2
    for v in vias:
        if v["net"] == net:
            hit = np.hypot(G.X - v["x"], G.Y - v["y"]) <= v["size"] / 2
            m[0] |= hit
            m[1] |= hit
    for p in pads:
        if p["net"] == net:
            hit = dist_rect(G, p) <= 0
            for k, l in enumerate(LAYERS):
                if l in p["layers"]:
                    m[k] |= hit
    return m


def astar(G, free, via_ok, starts, goal_mask, goal_xy):
    """starts: [(layer, j, i)]; goal_mask[layer] bool arrays"""
    ny, nx = G.X.shape
    gx, gy = goal_xy
    def h(j, i):
        x, y = G.xy(j, i)
        return math.hypot(x - gx, y - gy) / RES
    pq, best, prev = [], {}, {}
    for s in starts:
        best[s] = 0
        heapq.heappush(pq, (h(s[1], s[2]), 0, s))
    steps = [(0, 1, 1), (1, 0, 1), (0, -1, 1), (-1, 0, 1), (1, 1, 1.414), (1, -1, 1.414), (-1, 1, 1.414), (-1, -1, 1.414)]
    while pq:
        f, g, (l, j, i) = heapq.heappop(pq)
        if g > best.get((l, j, i), 1e18):
            continue
        if goal_mask[l][j, i]:
            path = [(l, j, i)]
            while path[-1] in prev:
                path.append(prev[path[-1]])
            return path[::-1]
        for dj, di, c in steps:
            jj, ii = j + dj, i + di
            if 0 <= jj < ny and 0 <= ii < nx and (free[l][jj, ii] or goal_mask[l][jj, ii]):
                n = (l, jj, ii)
                ng = g + c + (0.4 if l == 1 else 0)
                if ng < best.get(n, 1e18):
                    best[n], prev[n] = ng, (l, j, i)
                    heapq.heappush(pq, (ng + h(jj, ii), ng, n))
        if via_ok[j, i]:
            n = (1 - l, j, i)
            ng = g + VIA_COST
            if ng < best.get(n, 1e18):
                best[n], prev[n] = ng, (l, j, i)
                heapq.heappush(pq, (ng + h(j, i), ng, n))
    return None


def to_plan(G, net, path, start_key, end_key):
    pts = []
    cur_l = path[0][0]
    pts.append({"pad": start_key, "layer": LAYERS[cur_l]})
    last_dir = None
    for k in range(1, len(path)):
        l0, j0, i0 = path[k - 1]
        l1, j1, i1 = path[k]
        if l1 != l0:
            x, y = G.xy(j0, i0)
            pts.append({"x": x, "y": y, "layer": LAYERS[l0]})
            pts.append({"x": x, "y": y, "layer": LAYERS[l1]})
            last_dir = None
            continue
        d = (j1 - j0, i1 - i0)
        if last_dir is not None and d != last_dir:
            x, y = G.xy(j0, i0)
            pts.append({"x": x, "y": y, "layer": LAYERS[l0]})
        last_dir = d
    lj, li = path[-1][1], path[-1][2]
    x, y = G.xy(lj, li)
    pts.append(end_key if end_key else {"x": x, "y": y, "layer": LAYERS[path[-1][0]]})
    return {"net": net, "width": STUB_W, "viaSize": VIA, "viaDrill": DRILL, "paths": [pts]}


def main():
    board, out = sys.argv[1], sys.argv[2]
    pads, segs, vias = load(board)
    entries = []
    for job in sys.argv[3:]:
        net, a, b = job.split(":")
        pa = next(p for p in pads if f'{p["ref"]}.{p["num"]}' == a)
        pb = None if b == "copper" else next(p for p in pads if f'{p["ref"]}.{p["num"]}' == b)
        cx = (pa["x"] + (pb["x"] if pb else pa["x"])) / 2
        cy = (pa["y"] + (pb["y"] if pb else pa["y"])) / 2
        span = 14.0 if pb is None else max(8.0, abs(pa["x"] - pb["x"]) + 8, abs(pa["y"] - pb["y"]) + 8)
        G = Grid(cx - span / 2 - 4, cy - span / 2 - 4, cx + span / 2 + 4, cy + span / 2 + 4)
        d, hole = obstacles(G, pads, segs, vias + [dict(x=e["x"], y=e["y"], size=VIA, net=e["net"]) for e in []], net)
        free = [d[k] >= CLR + STUB_W / 2 for k in range(2)]
        via_ok = (d[0] >= CLR + VIA / 2) & (d[1] >= CLR + VIA / 2) & (hole >= 0.25 + DRILL / 2)
        own = own_copper(G, pads, segs, vias, net)
        sj, si = G.cell(pa["x"], pa["y"])
        starts = [(LAYERS.index(l), sj, si) for l in pa["layers"]]
        if pb:
            goal = [np.zeros(G.X.shape, bool), np.zeros(G.X.shape, bool)]
            hit = dist_rect(G, pb) <= 0
            for k, l in enumerate(LAYERS):
                if l in pb["layers"]:
                    goal[k] |= hit
            gxy = (pb["x"], pb["y"])
        else:
            # any of the net's copper not on the starting pad itself
            mine = dist_rect(G, pa) <= 0.3
            goal = [own[k] & ~mine for k in range(2)]
            ys, xs = np.nonzero(goal[0] | goal[1])
            k = np.argmin(np.hypot(G.X[ys, xs] - pa["x"], G.Y[ys, xs] - pa["y"]))
            gxy = (G.X[ys[k], xs[k]], G.Y[ys[k], xs[k]])
        # the start pad's own cells are free for the net
        for k in range(2):
            free[k] |= dist_rect(G, pa) <= 0
        path = astar(G, free, via_ok, starts, goal, gxy)
        if not path:
            print(f"{job}: no path")
            continue
        e = to_plan(G, net, path, a, b if pb else None)
        nvia = sum(1 for k in range(1, len(path)) if path[k][0] != path[k - 1][0])
        print(f"{job}: {len(path) * RES:.1f} mm, {nvia} via(s)")
        entries.append(e)
        # later jobs see this path as copper
        for k in range(1, len(path)):
            (l0, j0, i0), (l1, j1, i1) = path[k - 1], path[k]
            if l0 == l1:
                segs.append(dict(a=G.xy(j0, i0), b=G.xy(j1, i1), w=STUB_W, layer=LAYERS[l0], net=net))
            else:
                x, y = G.xy(j0, i0)
                vias.append(dict(x=x, y=y, size=VIA, net=net))
    json.dump({"nets": entries}, open(out, "w"), indent=1)


if __name__ == "__main__":
    main()
