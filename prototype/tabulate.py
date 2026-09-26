"""Turn sim_results.json into the markdown table in docs/DESIGN.md."""
import json
import sys

import numpy as np

import glissando as g


def threshold(pts, level):
    """SNR where decode probability first crosses `level` (linear interpolation)."""
    pts = sorted(pts)
    for (s0, p0), (s1, p1) in zip(pts, pts[1:]):
        if p0 < level <= p1:
            return s0 + (level - p0) / (p1 - p0) * (s1 - s0)
    return None


def main(path="sim_results.json"):
    t = json.load(open(path))
    rows = {}
    for k, d in t.items():
        gi, c, snr = k.split("/")
        rows.setdefault((int(gi), c), []).append((int(snr), d["ok"] / d["n"], d["false"]))
    chans = sorted({c for _, c in rows}, key=["awgn", "good", "moderate", "poor", "flutter"].index)
    print("| Gear | " + " | ".join(f"{c} 50% / 90%" for c in chans) + " |")
    print("|---|" + "---|" * len(chans))
    for gi in sorted({gi for gi, _ in rows}):
        cells = []
        for c in chans:
            pts = [(s, p) for s, p, _ in rows.get((gi, c), [])]
            a, b = threshold(pts, 0.5), threshold(pts, 0.9)
            f = lambda v: "n/a" if v is None else f"{v:.1f}"
            cells.append(f"{f(a)} / {f(b)}")
        gear = g.GEARS[gi]
        print(f"| G{gi} {gear.tempo} | " + " | ".join(cells) + " |")
    false = sum(d["false"] for d in t.values())
    n = sum(d["n"] for d in t.values())
    print(f"\nfalse decodes: {false} of {n} trials")


if __name__ == "__main__":
    main(*sys.argv[1:])
