#!/usr/bin/env python3
"""first_grid_model.py [xsdk_first_grid.log]: models the first step 6 grid (vk_loop_sample_grid_first.frag) the way the
console drew it, and counts the pixels of frame 1 each model gets unlike the console, 2 or less per channel being
alike, as xsdk_test checks.

Two rules, each on or off:
- rcp low: rcp(3) and rcp(5) land a hair under a third and a fifth, so mod(x, 3.0) and mod(x, 5.0), x - y * floor(x *
  rcp(y)), give y, not 0, where x is a whole multiple of y.
- quad LOD: a fetch at register gradients takes its LOD from its 2x2 quad's first pixel inside the triangle drawn, top
  left, top right, bottom left, bottom right, not its own; a pixel that left the loop holds its counter, and so its
  gradients.

The log is the diagnostic build's, which logged every wrong pixel of the first frames rather than 8."""
import re
import sys
from pathlib import Path

X0, WIDTH, HEIGHT = 1240, 40, 120
LEVELS_SIDE = 128.0
LOG = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).with_name("xsdk_first_grid.log")


def to_byte(v):
    return int(min(max(v, 0.0), 1.0) * 255.0 + 0.5)


def height_of(column):
    return (column * column + column // 3) % 8 * 3 // 4


def grey(level):
    return 16 + 64 * level


def mod(a, b, rcp_low):
    return b if rcp_low and a and a % b == 0 else a % b


def march(c, r, rcp_low):
    """The first loop's passes, and so i after it."""
    start, n = c + 2 * r, 4 + mod(c + r, 5, rcp_low)
    for i in range(n):
        if (32 * height_of(start + i) + 16) / 255.0 >= 1.0 - (i + 1) * 0.125:
            return i
    return n


def inside(x, y):
    """Which of the grid quad's two triangles, split from (1280, 0) to (1240, 120), a pixel's centre lies in."""
    return x + 0.5 < X0 + WIDTH - (y + 0.5) / 3


def colour(x, y, rcp_low, quad_lod):
    c, r = x % 10, y % 10
    runs = march(c, r, rcp_low)
    sx, sy = x, y
    if quad_lod:
        quad = [(x & ~1, y & ~1), (x | 1, y & ~1), (x & ~1, y | 1), (x | 1, y | 1)]
        sx, sy = next(p for p in quad if inside(*p) == inside(x, y))
    sc, sr = sx % 10, sy % 10
    source_runs = march(sc, sr, rcp_low)
    red = green = blue = 0.0
    for p in range(runs):
        red += grey((p + c + r) % 4) / 255.0 * 0.0625
        green += grey((min(p, source_runs) + sc) % 4) / 255.0 * 0.0625 + 3.0 * (p + 1) / 255.0
    for k in range(mod(r // 2, 3, rcp_low) + 2):
        n = (k + c // 2) % 4
        blue += grey(n) / 255.0 * 0.125 + (1 << n) / LEVELS_SIDE * 2.0
    return to_byte(red + runs * 0.03125), to_byte(green), to_byte(blue)


def main():
    console = {}
    pattern = re.compile(r"frame 1: a loop grid's pixel at \((\d+), (\d+)\).*held ([0-9A-F]{8})")
    for line in LOG.read_text(errors="replace").splitlines():
        m = pattern.search(line)
        if m:
            held = int(m[3], 16)
            console[(int(m[1]), int(m[2]))] = ((held >> 8) & 0xFF, (held >> 16) & 0xFF, held >> 24)
    print(f"frame 1: {len(console)} of {WIDTH * HEIGHT} pixels wrong on the console")
    for rcp_low in (False, True):
        for quad_lod in (False, True):
            unlike = 0
            for y in range(HEIGHT):
                for x in range(X0, X0 + WIDTH):
                    got = console.get((x, y), colour(x, y, False, False))
                    unlike += any(abs(a - b) > 2 for a, b in zip(got, colour(x, y, rcp_low, quad_lod)))
            print(f"rcp low {rcp_low!s:5}, quad LOD {quad_lod!s:5}: {unlike} unlike the console")


if __name__ == "__main__":
    main()
