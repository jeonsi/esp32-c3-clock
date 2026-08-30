#!/usr/bin/env python3
"""Remove segment F (upper-left vertical) from the digit 7 of a DSEG7 TTF.

DSEG's stock 7 lights A, B, C and F; real seven-segment clocks (and the CYD
clock fonts) show only A, B, C. usage: make_no7f.py in.ttf out.ttf
Needs fontTools (pip install fonttools).
"""
import sys
from array import array
from fontTools.ttLib import TTFont
from fontTools.ttLib.tables._g_l_y_f import GlyphCoordinates

src, dst = sys.argv[1], sys.argv[2]
f = TTFont(src)
glyf = f['glyf']
name = f.getBestCmap()[ord('7')]
g = glyf[name]
assert g.numberOfContours > 0, 'composite glyph?'
coords, ends, flags = g.coordinates, g.endPtsOfContours, g.flags
boxes = []
start = 0
for e in ends:
    pts = coords[start:e + 1]
    xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
    boxes.append((start, e, min(xs), min(ys), max(xs), max(ys)))
    start = e + 1
gy0 = min(b[3] for b in boxes); gy1 = max(b[5] for b in boxes); mid = (gy0 + gy1) / 2
# F: tall-and-narrow contour (height > width) whose centre is in the upper half, leftmost of those
verticals = [b for b in boxes if (b[5] - b[3]) > (b[4] - b[2]) and (b[3] + b[5]) / 2 > mid]
F = min(verticals, key=lambda b: b[2])
for b in boxes:
    print(('F ' if b is F else '  ') + f'contour pts {b[0]}..{b[1]} bbox x {b[2]}..{b[4]} y {b[3]}..{b[5]}', file=sys.stderr)
s, e = F[0], F[1]
n = e - s + 1
new_coords = GlyphCoordinates([c for i, c in enumerate(coords) if not (s <= i <= e)])
new_flags = array('B', [fl for i, fl in enumerate(flags) if not (s <= i <= e)])
new_ends = [x if x < s else x - n for x in ends if not (s <= x <= e)]
g.coordinates, g.flags, g.endPtsOfContours = new_coords, new_flags, new_ends
g.numberOfContours = len(new_ends)
g.recalcBounds(glyf)
f.save(dst)
print(f'{dst}: removed contour {s}..{e} from "{name}"', file=sys.stderr)
