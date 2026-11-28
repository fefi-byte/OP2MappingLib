"""Cliff tests and quality measurement for op2maplib (via the Python binding).

Run from the repository root after building the native library (see the
README; or set OP2MAPLIB_PATH to the built library):

    python -I bindings/python/tests/test_cliffs.py [--show]

Uses profiles/op2.json and tests/vectors/cliff_l.json from the repository; no
game files are needed. Exit code: 0 if all checks pass, 1 if any fail.

Draws a set of test walls (straight, sloped, L/U shapes, loops; every high
side; heights 3-6; on mud and on sand) on blank maps and measures, per the
checks op2-codegen-editor uses for its own cliff tool:

  rock to open   cliff tiles showing rock on a side that faces open ground
  edge clash     neighbouring cliff tiles whose edge codes disagree
  holes          neighbouring cliff tiles that are both ground along an edge
  unseen pairs   neighbouring cliff tiles never seen side by side in the
                 original maps (op2-codegen-editor: about 25-28 % on mud)
  same tile      cliff tiles with an identical direct neighbour
                 (original maps: 3.9 %)

--show prints each wall as letters: H = cliff high side, L = low side,
N = north cliff, c = other cliff tile, . = ground.

Besides the measurements it checks snapping, the shared cliff vector hash
(same as the C++ and .NET tests), determinism, that cliff cells stay fixed
when painting next to them, and that invalid input is refused.
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

# Import the binding from this checkout rather than any installed copy.
REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "bindings" / "python"))

from op2maplib import BlendMap, CliffSide, Profile, snap_cliff_point, version  # noqa: E402

# Tile sides in the order of the profile's cliff "edges" string, the cell
# offset towards each side's neighbour (y grows southwards), and the side of
# the neighbour that faces back.
SIDES = ("n", "e", "s", "w")
DX = {"n": 0, "e": 1, "s": 0, "w": -1}
DY = {"n": -1, "e": 0, "s": 1, "w": 0}
OPP = {"n": "s", "e": "w", "s": "n", "w": "e"}
FAILURES = 0


def check(cond: bool, what: str) -> None:
    """Records and prints a failed check; the run continues."""
    global FAILURES
    if not cond:
        FAILURES += 1
        print(f"  FAILED: {what}")


def load_cliff_lookup(profile_json: dict) -> tuple[dict, list]:
    """Maps standard mapping indices to cliff tiles.

    Returns (lookup, tiles): lookup is mapping index -> index into tiles, and
    tiles is the profile's cliffs.tiles list. The test maps use the standard
    mapping table, so this identifies every cliff tile the library places.
    """
    std = profile_json["standardMapping"]
    by_ref = {(t["ts"], t["img"]): i for i, t in enumerate(profile_json["cliffs"]["tiles"])}
    lookup = {}
    for m, (ts, img) in enumerate(std):
        i = by_ref.get((ts, img))
        if i is not None:
            lookup[m] = i
    return lookup, profile_json["cliffs"]["tiles"]


def edges_of(tile: dict) -> dict:
    """A cliff tile's edge codes per side: 4 digits each (0 = ground, 1 = rock,
    2 = matches either; see docs/profile-format.md)."""
    parts = tile["edges"].split()
    return {s: [int(c) for c in parts[i]] for i, s in enumerate(SIDES)}


def fill_map(profile: Profile, w: int, h: int, ground: str) -> BlendMap:
    """A w x h map filled with one ground (by id) and resolved."""
    bm =BlendMap(profile, w, h, profile.standard_mapping(), [0] * (w * h))
    bm.paint(profile.find_ground(ground), [(x, y) for y in range(h) for x in range(w)])
    bm.resolve(seed=1)
    return bm


# Test walls on a 48x48 map. Every one is drawn at heights 3-6 with seeds 1
# and 2, on mud and on sand.
SCENARIOS = [
    # name, points (cells), high side
    ("flat, high north (wall)", [(8, 20), (40, 20)], CliffSide.NORTH),
    ("flat, high south (north edge)", [(8, 20), (40, 20)], CliffSide.SOUTH),
    ("vertical, high left", [(24, 6), (24, 36)], CliffSide.LEFT),
    ("vertical, high right", [(24, 6), (24, 36)], CliffSide.RIGHT),
    ("45 deg, high north", [(8, 30), (28, 10)], CliffSide.NORTH),
    ("45 deg, high south", [(8, 10), (28, 30)], CliffSide.SOUTH),
    ("2:1 slope, high north", [(6, 26), (38, 10)], CliffSide.NORTH),
    ("2:1 slope, high south", [(6, 10), (38, 26)], CliffSide.SOUTH),
    ("3:1 slope, high left", [(4, 12), (40, 24)], CliffSide.LEFT),
    ("steep 1:3, high left", [(18, 4), (26, 28)], CliffSide.LEFT),
    ("L shape (screenshot)", [(20, 6), (20, 20), (40, 20)], CliffSide.LEFT),
    ("L shape, high right", [(20, 6), (20, 20), (40, 20)], CliffSide.RIGHT),
    ("U shape", [(10, 8), (10, 24), (36, 24), (36, 8)], CliffSide.LEFT),
    ("closed loop (plateau)", [(12, 10), (34, 10), (34, 28), (12, 28), (12, 10)], CliffSide.RIGHT),
    ("zigzag", [(6, 20), (16, 12), (26, 20), (36, 12)], CliffSide.NORTH),
]


def measure(bm: BlendMap, changes: list, lookup: dict, tiles: list) -> dict:
    """Counts the quality measures of one drawn cliff (see the module docstring).

    Returns a dict of counts: cells (cliff tiles), rock_open (rock edges facing
    non-cliff cells; rock_open_ns of those are north-cliff line ends), pairs
    (neighbouring cliff pairs), clash, invalid (clashing and never seen
    together), holes, unseen and same.
    """
    # Only the cells the draw changed: a few plain ground tiles are also in the
    # cliff set (all-ground edges), so the map's fill must not count as cliff.
    cliff = {}
    for c in changes:
        i = lookup.get(c.new_mapping)
        if i is not None:
            cliff[(c.x, c.y)] = i
    out = {"cells": len(cliff), "rock_open": 0, "rock_open_ns": 0, "clash": 0, "invalid": 0, "holes": 0,
           "pairs": 0, "unseen": 0, "same": 0}
    for (x, y), i in cliff.items():
        e = edges_of(tiles[i])
        same = False
        for s in SIDES:
            q = (x + DX[s], y + DY[s])
            j = cliff.get(q)
            if j is None:
                if 1 in e[s]:
                    out["rock_open"] += 1
                    # North-cliff line ends deliberately run to the cell edge on the west/east side.
                    if tiles[i]["cell"] == 8 and s in ("w", "e"):
                        out["rock_open_ns"] += 1
                continue
            if j == i:
                same = True
            if s in ("e", "s"):            # count each pair once
                f = edges_of(tiles[j])[OPP[s]]
                a = e[s]
                out["pairs"] += 1
                # Edge code 2 matches anything, so only a 0 facing a 1 clashes.
                clash = any(not (u == v or u == 2 or v == 2) for u, v in zip(a, f))
                if clash:
                    out["clash"] += 1
                    # A clash between tiles seen side by side in the original
                    # maps is how the game itself joins them; only an unseen
                    # clashing pair is invalid.
                    if j not in tiles[i]["seen"][s]:
                        out["invalid"] += 1
                # Ground on both sides along most of the edge: a gap in the wall.
                if sum(1 for u, v in zip(a, f) if u == 0 and v == 0) >= 3:
                    out["holes"] += 1
                if j not in tiles[i]["seen"][s]:
                    out["unseen"] += 1
        out["same"] += same
    return out


def cliff_state(bm: BlendMap, lookup: dict, tiles: list, w: int, h: int) -> dict:
    """(x, y) -> cliff tile index for every cell holding a cliff tile that is not
    all ground (plain ground tiles that are also in the cliff set don't count;
    tiles coded "either" on some edges do)."""
    out = {}
    for y in range(h):
        for x in range(w):
            i = lookup.get(bm.tile_at(x, y))
            if i is not None and tiles[i]["edges"].replace("0", "").strip():
                out[(x, y)] = i
    return out


def rock_open(cliff: dict, tiles: list) -> tuple[int, int]:
    """(rock edges facing non-cliff cells, holes) of a whole cliff state."""
    ro = holes = 0
    for (x, y), i in cliff.items():
        e = edges_of(tiles[i])
        for s in SIDES:
            j = cliff.get((x + DX[s], y + DY[s]))
            if j is None:
                ro += 1 in e[s]
            elif s in ("e", "s"):
                f = edges_of(tiles[j])[OPP[s]]
                holes += sum(1 for u, v in zip(e[s], f) if u == 0 and v == 0) >= 3
    return ro, holes


def picture(changes: list, lookup: dict, tiles: list, w: int, h: int) -> str:
    """ASCII picture of a drawn cliff (letters: see the module docstring),
    trimmed of empty rows and trailing ground."""
    drawn ={(c.x, c.y): lookup.get(c.new_mapping) for c in changes}
    rows = []
    for y in range(h):
        row = ""
        for x in range(w):
            i = drawn.get((x, y))
            if i is None:
                row += "."
            else:
                # Letters by the tile's default Outpost 2 cell type.
                row += {9: "H", 10: "L", 8: "N"}.get(tiles[i]["cell"], "c")
        rows.append(row.rstrip(".") or "")
    while rows and not rows[-1]:
        rows.pop()
    while rows and not rows[0]:
        rows.pop(0)
    return "\n".join(rows)


def main() -> int:
    """Runs all checks and prints the quality table; returns the process exit code."""
    show = "--show" in sys.argv
    print(f"op2maplib {version()} - cliffs")
    pj = json.loads((REPO / "profiles" / "op2.json").read_text(encoding="utf-8"))
    profile = Profile.from_file(REPO / "profiles" / "op2.json")
    lookup, tiles = load_cliff_lookup(pj)

    mud, san, roc = (profile.find_ground(g) for g in ("mud", "san", "roc"))
    check(profile.cliff_ground(mud) == 1, "mud has its own cliffs")
    check(profile.cliff_ground(san) == 1, "sand has its own cliffs")
    check(profile.cliff_ground(roc) == 0, "rock uses the fallback cliffs")

    # Snapping: a near-45-degree drag snaps to 45 degrees; near-flat to flat.
    check(snap_cliff_point(10, 10, 20, 19) == (20, 20), "snap to 45 degrees")
    check(snap_cliff_point(10, 10, 30, 11) == (30, 10), "snap to flat")
    check(snap_cliff_point(10, 10, 30, 19) == (30, 20), "snap to 2:1")

    w = h = 48
    totals = {}
    for ground in ("mud", "san"):
        t = {"walls": 0, "drawn": 0, "greedy": 0, "cells": 0, "rock_open": 0, "rock_open_ns": 0, "clash": 0,
             "invalid": 0, "holes": 0, "pairs": 0, "unseen": 0, "same": 0, "ms": 0.0}
        stages: dict = {}
        for height in (3, 4, 5, 6):
            for name, pts, side in SCENARIOS:
                for seed in (1, 2):
                    bm = fill_map(profile, w, h, ground)   # a fresh map per wall
                    t0 = time.perf_counter()
                    changes = bm.draw_cliff(pts, side, height, seed)
                    t["ms"] += (time.perf_counter() - t0) * 1000
                    t["walls"] += 1
                    stage = bm.last_cliff_stage
                    stages[stage] = stages.get(stage, 0) + 1
                    if changes:
                        t["drawn"] += 1
                    if stage == -1:
                        t["greedy"] += 1
                    m = measure(bm, changes, lookup, tiles)
                    for k in ("cells", "rock_open", "rock_open_ns", "clash", "invalid", "holes", "pairs",
                              "unseen", "same"):
                        t[k] += m[k]
                    check(all(c.cell_type in (-1,) or 0 <= c.cell_type < 32 for c in changes),
                          f"{ground} {name}: valid cell types")
                    if show and seed == 1 and height == 3:
                        print(f"\n--- {ground}: {name} (stage {stage}, {m['cells']} tiles)")
                        print(picture(changes, lookup, tiles, w, h))
        t["stages"] = dict(sorted(stages.items()))
        totals[ground] = t

    # Shared vector: the same cliff as the C++ and .NET tests.
    vec = json.loads((REPO / "tests" / "vectors" / "cliff_l.json").read_text(encoding="utf-8"))
    vm = BlendMap(profile, vec["width"], vec["height"], profile.standard_mapping(),
                  [vec["initialMapping"]] * (vec["width"] * vec["height"]))
    vm.paint(profile.find_ground(vec["fillGround"]), [(x, y) for y in range(vec["height"]) for x in range(vec["width"])])
    vm.resolve(seed=vec["fillSeed"])
    cl = vec["cliff"]
    vm.draw_cliff([tuple(p) for p in cl["points"]], cl["highSide"], cl["wallHeight"], cl["seed"])
    # 64-bit FNV-1a over every cell's mapping index (little-endian 32-bit),
    # the same hash the C++ tests compute.
    hsh = 1469598103934665603
    for y in range(vec["height"]):
        for x in range(vec["width"]):
            for b in (vm.tile_at(x, y) & 0xFFFFFFFF).to_bytes(4, "little"):
                hsh = ((hsh ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    print(f"cliff vector hash: {hsh:016x}")
    check(f"{hsh:016x}" == vec["fnv1a64"], "cliff vector hash matches C++")

    # Determinism: same input and seed -> same tiles.
    a, b = fill_map(profile, w, h, "mud"), fill_map(profile, w, h, "mud")
    ca = a.draw_cliff([(20, 6), (20, 20), (40, 20)], CliffSide.LEFT, 3, 42)
    cb = b.draw_cliff([(20, 6), (20, 20), (40, 20)], CliffSide.LEFT, 3, 42)
    check([(c.x, c.y, c.new_mapping) for c in ca] == [(c.x, c.y, c.new_mapping) for c in cb], "deterministic")

    # Cliff cells become fixed for terrain blending: painting around keeps them.
    cliff_cells = {(c.x, c.y) for c in ca}
    check(all(a.ground_at(x, y) == -1 for x, y in cliff_cells), "cliff cells are fixed")
    a.paint(san, [(x, y) for y in range(0, 30) for x in range(10, 45) if (x, y) not in cliff_cells])
    a.resolve(seed=3)
    check(all(a.tile_at(c.x, c.y) == c.new_mapping for c in ca), "painting next to a cliff keeps it")

    # Closing cuts: every test wall at heights 3 and 5 on mud and sand, cut
    # across the middle of its first segment (3 cells wide), then closed.
    # Measured on the whole wall before and after: rock edges facing open
    # ground, holes, and determinism.
    cut_totals = {}
    for ground in ("mud", "san"):
        t = {"walls": 0, "closed": 0, "open_before": 0, "open_after": 0, "holes": 0}
        for height in (3, 5):
            for name, pts, side in SCENARIOS:
                results = []
                for _ in range(2):   # twice, for determinism
                    bm = fill_map(profile, w, h, ground)
                    bm.draw_cliff(pts, side, height, 1)
                    wall = cliff_state(bm, lookup, tiles, w, h)
                    (ax, ay), (bx, by) = pts[0], pts[1]
                    px, py = (ax + bx) // 2, (ay + by) // 2
                    if abs(bx - ax) >= abs(by - ay):
                        cut = [(x, y) for (x, y) in wall if abs(x - px) <= 1]
                    else:
                        cut = [(x, y) for (x, y) in wall if abs(y - py) <= 1]
                    bm.paint(profile.find_ground(ground), cut)
                    bm.resolve(seed=2)
                    opened = cliff_state(bm, lookup, tiles, w, h)
                    removed = [0] * (w * h)
                    for (x, y) in cut:
                        if (x, y) not in opened:
                            removed[y * w + x] = 1
                    changes = bm.close_cliff_cut(removed, seed=3)
                    results.append([(c.x, c.y, c.new_mapping) for c in changes])
                check(results[0] == results[1], f"{ground} {name}: closing is deterministic")
                closed = cliff_state(bm, lookup, tiles, w, h)
                t["walls"] += 1
                t["closed"] += bool(changes)
                t["open_before"] += rock_open(opened, tiles)[0]
                ro, holes = rock_open(closed, tiles)
                t["open_after"] += ro
                t["holes"] += holes
        cut_totals[ground] = t
    print()
    print(f"{'cuts':6} {'walls':>5} {'closed':>6} {'rock->open before':>18} {'after':>6} {'holes':>5}")
    for ground, t in cut_totals.items():
        print(f"{ground:6} {t['walls']:5} {t['closed']:6} {t['open_before']:18} {t['open_after']:6} {t['holes']:5}")
        check(t["holes"] == 0, f"{ground}: closing a cut leaves no holes")
        check(t["open_after"] * 5 <= t["open_before"], f"{ground}: closing removes most rock facing the cut")
    check(fill_map(profile, w, h, "mud").close_cliff_cut([0] * (w * h)) == [], "nothing cut, nothing to close")

    # Invalid input: draw_cliff must raise rather than return.
    try:
        a.draw_cliff([(0, 0), (500, 0)], CliffSide.NORTH)
        check(False, "points outside the map are refused")
    except Exception:
        pass

    # Quality table per ground; only drawn/holes/greedy are hard checks.
    print()
    print(f"{'':6} {'walls':>5} {'greedy':>6} {'tiles':>6} {'rock->open':>17} {'clash':>5} {'invalid':>7} "
          f"{'holes':>5} {'unseen pairs':>13} {'same tile':>10} {'ms/wall':>8}  stages")
    for ground, t in totals.items():
        unseen = 100.0 * t["unseen"] / max(1, t["pairs"])
        same = 100.0 * t["same"] / max(1, t["cells"])
        ro = f"{t['rock_open']} ({t['rock_open_ns']} N-end)"
        print(f"{ground:6} {t['walls']:5} {t['greedy']:6} {t['cells']:6} {ro:>17} {t['clash']:5} {t['invalid']:7} "
              f"{t['holes']:5} {unseen:12.1f}% {same:9.1f}% {t['ms'] / t['walls']:8.1f}  {t['stages']}")
        check(t["drawn"] == t["walls"], f"{ground}: every wall drawn")
        check(t["holes"] == 0, f"{ground}: no holes")
        check(t["greedy"] == 0, f"{ground}: no wall needed the greedy fallback")

    if FAILURES:
        print(f"{FAILURES} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
