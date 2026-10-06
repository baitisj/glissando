#!/usr/bin/env python3
"""Writes app/src/gui/glissando/BorderLines.h, the borders between countries
the console's map ball can draw.

The source is the same Natural Earth 1:110m countries as
gen_land_outlines.py (public domain, https://www.naturalearthdata.com/):

    python3 prototype/globe/gen_border_lines.py path/to/naturalearth_lowres.shp --write

A border is an edge two countries' outlines share. The shared edges between
each pair of countries are joined into lines, and the lines smoothed
(Douglas-Peucker, to SMOOTH_DEGREES of latitude, longitude shrunk by the
cosine of the latitude) so they read as a few clean strokes rather than
every wiggle. Ends where three countries meet stay put, so the lines still
join up, and a border that closes on itself (Lesotho) keeps at least three
corners.

Smoothing must not make countries that aren't there. A straightened border
could cut across another border, or itself, or a stretch of coast, and
fence off a scrap of land; or cut across a bay and leave a piece of sea
inside the land. So every straightened stretch is checked: it may meet
another line only at an end they share, and it must run over land. A
stretch that fails gets its farthest dropped point back, and the check runs
again until nothing fails. Then, with no lines crossing, the borders divide
the land into exactly the countries the unsmoothed borders do.

Long edges are then split as for the coastlines, and the points go in
hundredths of a degree.
"""

import argparse
import math
import os
import sys

from gen_land_outlines import MAX_EDGE_DEGREES, SCALE, read_shapes

SMOOTH_DEGREES = 0.25

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "app", "src", "gui", "glissando", "BorderLines.h")


def key(point):
    return (round(point[0] * SCALE), round(point[1] * SCALE))


def shared_edges(shapes):
    """For each pair of countries, the edges their outlines share."""
    owners = {}
    for country, rings in enumerate(shapes):
        for ring in rings:
            for a, b in zip(ring, ring[1:] + ring[:1]):
                a, b = key(a), key(b)
                if a != b:
                    owners.setdefault((min(a, b), max(a, b)), set()).add(country)
    pairs = {}
    for edge, countries in owners.items():
        if len(countries) >= 2:
            pair = tuple(sorted(countries)[:2])
            pairs.setdefault(pair, []).append(edge)
    return pairs


def coast_edges(shapes):
    """The edges only one country's outline has."""
    count = {}
    for rings in shapes:
        for ring in rings:
            for a, b in zip(ring, ring[1:] + ring[:1]):
                a, b = key(a), key(b)
                if a != b:
                    edge = (min(a, b), max(a, b))
                    count[edge] = count.get(edge, 0) + 1
    return sorted(edge for edge, n in count.items() if n == 1)


def chained(edges):
    """The edges joined into lines, each from an end (where the border
    branches or stops) to the next, and any closed loops."""
    links = {}
    for a, b in edges:
        links.setdefault(a, []).append(b)
        links.setdefault(b, []).append(a)
    for point in links:
        links[point].sort()
    used = set()

    def walk(start):
        line = [start]
        at = start
        while True:
            onward = [p for p in links[at] if (min(at, p), max(at, p)) not in used]
            if not onward:
                return line
            step = onward[0]
            used.add((min(at, step), max(at, step)))
            line.append(step)
            at = step
            if len(links[at]) != 2:
                return line

    def unused_from(point):
        return any((min(point, p), max(point, p)) not in used for p in links[point])

    lines = []
    ends = sorted(p for p in links if len(links[p]) != 2)
    for start in ends + sorted(links):
        while unused_from(start):
            lines.append(walk(start))
    return lines


def farthest_between(line, i, j):
    """The point of line strictly between i and j farthest from the straight
    line from i to j, longitude shrunk by the cosine of the latitude so a
    degree is about as far either way: (its index, how far in degrees)."""
    (x0, y0), (x1, y1) = line[i], line[j]
    shrink = math.cos(math.radians((y0 + y1) / 2.0 / SCALE))
    ax, ay, bx, by = x0 * shrink, y0, x1 * shrink, y1
    dx, dy = bx - ax, by - ay
    length = math.hypot(dx, dy)
    farthest, at = -1.0, i
    for k in range(i + 1, j):
        px, py = line[k][0] * shrink, line[k][1]
        if length > 0.0:
            d = abs(dx * (ay - py) - dy * (ax - px)) / length
        else:
            d = math.hypot(px - ax, py - ay)
        if d > farthest:
            farthest, at = d, k
    return at, farthest / SCALE


def smoothed(line, tolerance):
    """Douglas-Peucker: the indices of the points of line kept."""
    def keep(i, j):
        if j - i < 2:
            return [i]
        at, distance = farthest_between(line, i, j)
        if distance <= tolerance:
            return [i]
        return keep(i, at) + keep(at, j)

    last = len(line) - 1
    if line[0] == line[-1] and last >= 3:
        # A closed loop keeps three corners, so it still goes round.
        anchors = [0, last // 3, 2 * last // 3, last]
    else:
        anchors = [0, last]
    kept = []
    for i, j in zip(anchors, anchors[1:]):
        kept += keep(i, j)
    return kept + [last]


def orientation(a, b, c):
    v = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
    return (v > 0) - (v < 0)


def within(a, b, p):
    return min(a[0], b[0]) <= p[0] <= max(a[0], b[0]) and min(a[1], b[1]) <= p[1] <= max(a[1], b[1])


def meet(p, q, r, s):
    """Whether the segments pq and rs meet anywhere but at an end they
    share. Points are whole hundredths of a degree, so this is exact."""
    shared = {p, q} & {r, s}
    if len(shared) == 2:
        return True
    if shared:
        x = shared.pop()
        a = q if p == x else p
        b = s if r == x else r
        # Only by running on together from it, the same way.
        return orientation(x, a, b) == 0 and (a[0] - x[0]) * (b[0] - x[0]) + (a[1] - x[1]) * (b[1] - x[1]) > 0
    o1, o2 = orientation(p, q, r), orientation(p, q, s)
    o3, o4 = orientation(r, s, p), orientation(r, s, q)
    if o1 != o2 and o3 != o4:
        return True
    return ((o1 == 0 and within(p, q, r)) or (o2 == 0 and within(p, q, s)) or
            (o3 == 0 and within(r, s, p)) or (o4 == 0 and within(r, s, q)))


CELL = 100   # hundredths of a degree: a degree square


def cells(p, q):
    x0, x1 = sorted((p[0] // CELL, q[0] // CELL))
    y0, y1 = sorted((p[1] // CELL, q[1] // CELL))
    return [(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)]


class Land:
    """Whether a point is on land: the winding of the countries' outlines
    round it, as gen_land_outlines.py writes them and Globe::isLand reads
    them."""

    def __init__(self, shapes):
        self.bands = {}
        for rings in shapes:
            for ring in rings:
                for a, b in zip(ring, ring[1:] + ring[:1]):
                    a, b = key(a), key(b)
                    for band in range(min(a[1], b[1]) // CELL, max(a[1], b[1]) // CELL + 1):
                        self.bands.setdefault(band, []).append((a, b))

    def has(self, x, y):
        winding = 0
        for (x0, y0), (x1, y1) in self.bands.get(int(y // CELL), []):
            side = (x1 - x0) * (y - y0) - (x - x0) * (y1 - y0)
            if y0 <= y < y1 and side > 0:
                winding += 1
            elif y1 <= y < y0 and side < 0:
                winding -= 1
        return winding != 0


def untangled(lines, kept, coast, land):
    """Puts back points until no straightened stretch meets another line,
    or itself, but at a shared end, and every one runs over land. Returns
    how many points it put back."""
    restored = 0
    while True:
        grid = {}
        segments = []
        for p, q in coast:
            segments.append((p, q, None))
        for l, line in enumerate(lines):
            for i, j in zip(kept[l], kept[l][1:]):
                segments.append((line[i], line[j], (l, i, j)))
        for n, (p, q, _) in enumerate(segments):
            for cell in cells(p, q):
                grid.setdefault(cell, []).append(n)

        failing = set()
        for n, (p, q, owner) in enumerate(segments):
            if owner is None or owner[2] - owner[1] < 2:
                continue    # coast, or a border edge as it was
            seen = set()
            for cell in cells(p, q):
                for m in grid[cell]:
                    if m == n or m in seen:
                        continue
                    seen.add(m)
                    r, s, _ = segments[m]
                    if meet(p, q, r, s):
                        failing.add(owner)
                        other = segments[m][2]
                        if other is not None and other[2] - other[1] >= 2:
                            failing.add(other)
            if owner not in failing:
                for t in (0.25, 0.5, 0.75):
                    if not land.has(p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t):
                        failing.add(owner)
                        break
        if not failing:
            return restored
        for l, i, j in failing:
            at, _ = farthest_between(lines[l], i, j)
            kept[l] = sorted(set(kept[l]) | {at})
            restored += 1


def densified(line):
    out = [line[0]]
    for (x0, y0), (x1, y1) in zip(line, line[1:]):
        pieces = math.ceil(max(abs(x1 - x0), abs(y1 - y0)) / MAX_EDGE_DEGREES)
        for k in range(1, pieces):
            t = k / pieces
            out.append((x0 + (x1 - x0) * t, y0 + (y1 - y0) * t))
        out.append((x1, y1))
    return out


def header(lines, raw_points):
    starts = []
    values = []
    for line in lines:
        starts.append(len(values) // 2)
        for lon, lat in line:
            values.append(int(round(lon * SCALE)))
            values.append(int(round(lat * SCALE)))
    starts.append(len(values) // 2)

    def wrap(numbers, per_line):
        rows = []
        for i in range(0, len(numbers), per_line):
            rows.append("    " + ", ".join(str(v) for v in numbers[i:i + per_line]) + ",")
        return "\n".join(rows)

    return f"""//=========================================================================
// Name:            BorderLines.h
// Purpose:         The borders between countries on the console's map ball.
//                  Generated by prototype/globe/gen_border_lines.py from
//                  Natural Earth's 1:110m countries (public domain); do not
//                  edit by hand.
//
// {len(lines)} lines, smoothed to {SMOOTH_DEGREES} degrees from {raw_points} points. Smoothing
// makes no line cross another, itself or a coast, nor run over the sea, so
// the borders divide the land into the map's countries and no more. Line l is
// POINTS[LINE_START[l]] up to LINE_START[l + 1], each point a longitude then
// a latitude in hundredths of a degree. A line that closes on itself ends
// where it began.
//=========================================================================

#ifndef GUI_GLISSANDO__BORDER_LINES_H
#define GUI_GLISSANDO__BORDER_LINES_H

#include <cstdint>

namespace BorderLines
{{

constexpr int LINE_COUNT = {len(lines)};
constexpr int POINT_COUNT = {len(values) // 2};
constexpr double UNITS_PER_DEGREE = {SCALE}.0;

constexpr uint32_t LINE_START[LINE_COUNT + 1] = {{
{wrap(starts, 12)}
}};

constexpr int16_t POINTS[POINT_COUNT * 2] = {{
{wrap(values, 16)}
}};

}} // namespace BorderLines

#endif // GUI_GLISSANDO__BORDER_LINES_H
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("shapefile")
    parser.add_argument("--write", action="store_true", help="write BorderLines.h rather than report")
    args = parser.parse_args()

    shapes = read_shapes(args.shapefile)
    pairs = shared_edges(shapes)
    raw = [line for pair in sorted(pairs) for line in chained(pairs[pair])]
    raw_points = sum(len(line) for line in raw)
    kept = [smoothed(line, SMOOTH_DEGREES) for line in raw]
    coast = coast_edges(shapes)
    restored = untangled(raw, kept, coast, Land(shapes))
    lines = [densified([(raw[l][i][0] / SCALE, raw[l][i][1] / SCALE) for i in kept[l]]) for l in range(len(raw))]
    text = header(lines, raw_points)
    points = sum(len(line) for line in lines)
    smooth_points = sum(len(k) for k in kept)
    print(f"{len(pairs)} borders, {len(lines)} lines, {raw_points} points smoothed to {smooth_points} "
          f"({restored} put back so no line crosses another or leaves the land), {points} after splitting "
          f"long edges, {len(text)} bytes of header", file=sys.stderr)
    if args.write:
        with open(OUT, "w") as f:
            f.write(text)
        print(f"wrote {os.path.normpath(OUT)}", file=sys.stderr)


if __name__ == "__main__":
    main()
