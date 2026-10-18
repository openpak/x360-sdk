#!/usr/bin/env python3
"""Samples and derivatives in a hardware loop's pass (milestone 46 step 6, xsdk:docs/scope/46-loops.md, "Samples and
derivatives in a pass"; xsdk:src/xsc_lower.c, src/xsc_lower_fragment.c, src/xsc_lower_vertex.c).

xsc refused a sample or derivative of what a pass works out: a pixel shader gathers its samples before its ALU work,
and a vertex shader its samples before its exports. Each is now lowered in the pass, after what it reads, its fetches
serialized and laid out within the pass as elsewhere, a run of setters whole (tests/test_dependent_fetch.py checks every
program of the testdata); one of what the pass doesn't work out goes before the loop as before. An if/else on a
uniform whose arm samples in a pass stays selects, as no fetch goes on the predicate.

Each shader runs on xdis/simulate.py in both models, each pixel or vertex alone and all in one group, with the
optimiser off and on, its fetches counted in and out of its loops. An implicit LOD and a derivative read the pixel's
2x2 quad, so those shaders run as quads (simulate.run_group's quads), alone being each quad alone; and each quad's
pixels take the same passes, as GLSL leaves an implicit LOD and a derivative undefined where they don't:

- vk_loop_sample_lod.frag: textureLod at a LOD the pass works out, left on what it samples, and in an arm of an if/else
  on a push constant, at counts that leave pixels at every pass.
- vk_loop_sample_grad.frag: textureGrad at gradients the pass works out, in a loop whose count is data.
- vk_loop_sample_computed.frag: texture() at the LOD the GPU works out from coordinates the pass scales, quads.
- vk_loop_derivative.frag: dFdx and dFdy of a value the pass works out, quads.
- vk_loop_parallax.frag: parallax occlusion's march, its step count from the view angle, a depth map sampled at
  gradients worked out before the loop, left once the ray passes the depth, quads for the gradients; a quad's pixels may
  leave at different passes, as no implicit LOD or derivative is in the loop.
- vk_vertex_loop_sample.vert: a vertex shader's textureLod at coordinates and a LOD the pass works out, left on what it
  samples.
- vk_loop_sample_grid.frag: xsdk_test's raster grid, every place in a 10-pixel cell: a march along a height map read
  at a LOD of 0, left at each pixel's own pass, textureLod and textureGrad in it and a tint read at the loop's counter
  through a0; then a loop whose count is the same across a quad, with texture() at the computed LOD and a dFdx. The
  mip chain's levels are flat, so a LOD shows as a colour.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xsc.resolve import resolve_interface  # noqa: E402
from test_data_loops import MAX_PASSES, loops_of, lowered  # noqa: E402
from test_hardware_loop_breaks import OPOS, PARAM_GEN_POS_SHIFT, PIXEL_LOOP, VERTEX_LOOP, check_models  # noqa: E402
from test_local_stores import module, push_constants  # noqa: E402

# A quad's pixels a step of 1/64 apart in v_uv, so every difference is exact.
STEP = 1.0 / 64.0
# vk_loop_parallax.frag's fewest steps, most and depth scale.
MARCH = [8.0, 32.0, 0.25, 0.0]
# vk_loop_sample_grid.frag's height map, 64 columns each a level of 8 (xsdk:tests/test_raster.c), its mip chain's
# texture side, and its tints, 3/255 apart.
HEIGHTS = [(c * c + c // 3) % 8 * 3 // 4 for c in range(64)]
LEVELS_SIDE = 128
TINTS = [3.0 * (i + 1) / 255.0 for i in range(8)]


def fail(message):
    print(f"  FAIL {message}")
    raise SystemExit("FAILED")


def fetches(blob):
    """How many texture fetches, setters and getGradients among them, lie inside loops and outside."""
    parsed = container.Container(blob)
    depth, inside, outside = 0, 0, 0
    for rec in xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0]:
        if rec.kind == "cf" and isinstance(rec.detail, xu.ParsedLoopStartInstruction):
            depth += 1
        elif rec.kind == "cf" and isinstance(rec.detail, xu.ParsedLoopEndInstruction):
            depth -= 1
        elif rec.kind == "tfetch":
            inside += depth > 0
            outside += depth == 0
    return inside, outside


def check_shape(name, want_fetches):
    blob, _, _ = lowered(name, False)
    got = (fetches(blob), loops_of(blob)[0])
    if got != (want_fetches, False):
        fail(f"{name}: fetches in and out of loops, and anything on the predicate, {got}, not {(want_fetches, False)}")
    print(f"  PASS {name}: {want_fetches[0]} fetches in its loops and {want_fetches[1]} before, nothing on the "
          "predicate")


def level(lod, side):
    """The level a fetch takes, rounded: its bias and register LOD, and where it computes one, the longer gradient's
    log2 in texels of a texture side texels wide."""
    total = lod.bias + (lod.register or 0.0)
    if lod.computed:
        total += math.log2(max(math.hypot(g[0], g[1]) for g in lod.gradients) * side)
    return math.floor(total + 0.5)


def lod_texel(total):
    """vk_loop_sample_lod.frag's texel on the simulator: its LOD, a quarter of it, 1 and a half."""
    return [total, 0.25 * total, 1.0, 0.5]


def texel_of_lod(slot, coordinates, lod):
    return lod_texel(lod.bias + (lod.register or 0.0))


def gradients_of(slot, coordinates, lod):
    """The horizontal and vertical gradients the fetch computes its LOD from, x and y each."""
    if not lod.computed or not lod.gradients:
        fail(f"a fetch without a computed LOD from gradients: {lod}")
    return [lod.gradients[0][0], lod.gradients[0][1], lod.gradients[1][0], lod.gradients[1][1]]


def quad_lanes(places):
    """Each place's 2x2 quad: v_uv a step apart from the place's u and v, top left, top right, bottom left and bottom
    right, and r1 the place's own."""
    return [{0: [u + STEP * dx, v + STEP * dy, 0.0, 0.0], 1: list(rest) + [0.0] * (4 - len(rest))}
            for u, v, *rest in places for dy in (0, 1) for dx in (0, 1)]


def sample_lod(x, y, mode, count):
    total, lod = [0.0] * 4, x
    for _ in range(count):
        texel = lod_texel(lod)
        total = [t + s for t, s in zip(total, texel)]
        if mode > 0.5:
            total[3] += lod_texel(lod + 0.5)[0]
        if texel[1] > y:
            break
        lod += 1.0
    return total


def check_lod(optimize, tag):
    name = "vk_loop_sample_lod.frag"
    blob, literals, _ = lowered(name, optimize)
    places = [(x, y) for x in (0.0, 0.5, 1.25) for y in (0.2, 0.55, 0.95, 9.0)]
    lanes = [{0: [x, y, 0.0, 0.0]} for x, y in places]
    for mode in (0.0, 1.0):
        for count in (0, 1, 3, 6):
            constants = push_constants(name, [[mode, 0.0, 0.0, 0.0]])
            constants.update(literals)
            check_models(f"{name}, mode {mode}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count},
                         [sample_lod(x, y, mode, count) for x, y in places], texture_lod=texel_of_lod)


def sample_grad(x, y):
    total, g = [0.0] * 4, 0.0625
    for _ in range(min(int(x), MAX_PASSES)):
        total = [total[0] + g, total[1], total[2], total[3] + g * y]
        g *= 2.0
    return total


def check_grad(optimize, tag):
    name = "vk_loop_sample_grad.frag"
    blob, literals, _ = lowered(name, optimize)
    places = [(x, y) for x in (0.0, 1.0, 2.5, 5.0, 8.0) for y in (0.5, 3.0)]
    check_models(f"{name}, {tag}", blob, [{0: [x, y, 0.0, 0.0]} for x, y in places], literals,
                 {PIXEL_LOOP: MAX_PASSES}, [sample_grad(x, y) for x, y in places], texture_lod=gradients_of)


def sample_computed(count):
    total, scale = [0.0] * 4, [1.0, 0.5]
    for _ in range(count):
        total = [total[0] + STEP * scale[0], total[1], total[2], total[3] + STEP * scale[1]]
        scale = [2.0 * s for s in scale]
    return total


def derivative(count):
    total = [0.0] * 4
    for i in range(count):
        m = 2.0 ** (i + 2) - 1.0
        total = [total[0] + m * STEP, total[1], total[2], total[3] + m * STEP]
    return total


def check_quads(name, model, optimize, tag):
    """Quads at counts 0 to 5, each the same across its quad and differing between them."""
    blob, literals, _ = lowered(name, optimize)
    places = [(0.25 + q * 0.125, 0.5 - q * 0.0625, float(count)) for q, count in enumerate((0, 1, 3, 5, 2))]
    check_models(f"{name}, quads at counts 0 to 5, {tag}", blob, quad_lanes(places), literals,
                 {PIXEL_LOOP: MAX_PASSES}, [model(int(count)) for *_, count in places for _ in range(4)],
                 texture_lod=gradients_of, quads=True)


def depth_of(u, v):
    """vk_loop_parallax.frag's depth map on the simulator: a step at u 0.4 and one at v 0.5."""
    return 0.15 + (0.5 if u >= 0.4 else 0.0) + (0.2 if v >= 0.5 else 0.0)


def parallax(u, v, view):
    length = math.sqrt(sum(c * c for c in view))
    n = [c / length for c in view]
    steps = MARCH[1] + (MARCH[0] - MARCH[1]) * abs(n[2])
    layer = 1.0 / steps
    delta = [n[0] * MARCH[2] / (n[2] * steps), n[1] * MARCH[2] / (n[2] * steps)]
    depth, i = 0.0, 0
    while i < int(steps) and i < MAX_PASSES and depth < depth_of(u, v):
        u, v, depth, i = u - delta[0], v - delta[1], depth + layer, i + 1
    return [u, v, depth, 1.0]


def check_parallax(optimize, tag):
    name = "vk_loop_parallax.frag"
    blob, literals, _ = lowered(name, optimize)
    views = ([0.0, 0.0, 1.0], [0.6, 0.0, 0.8], [0.0, -0.28, 0.96], [0.48, 0.64, 0.6])
    places = [(u, v, *view) for view in views for u, v in ((0.45, 0.6), (0.7, 0.3), (0.2, 0.51))]

    def sample(slot, coordinates, lod):
        if lod.gradients != ([STEP, 0.0, STEP], [0.0, STEP, 0.0]):
            fail(f"{name}: a sample at gradients {lod.gradients}, not dFdx and dFdy of v_uv")
        return [depth_of(coordinates[0], coordinates[1]), 0.0, 0.0, 1.0]

    lanes = quad_lanes(places)
    constants = push_constants(name, [MARCH])
    constants.update(literals)
    wants = [parallax(lane[0][0], lane[0][1], lane[1][:3]) for lane in lanes]
    passes = check_models(f"{name}, 12 quads at 4 view angles, {tag}", blob, lanes, constants,
                          {PIXEL_LOOP: MAX_PASSES}, wants, texture_lod=sample, quads=True)
    if passes >= int(MARCH[1]):
        fail(f"{name}: the group ran {passes} passes, as if no ray left early")


def vertex_texel(coordinates, lod):
    return [coordinates[0] + 0.5 * lod.register, coordinates[1], lod.register, 1.0]


def vertex_sample(x, y, u, v):
    total, i = [0.0] * 4, 0
    while i < int(x):
        texel = [u + 0.5 * i, v, float(i), 1.0]
        total = [t + s for t, s in zip(total, texel)]
        if texel[0] > y:
            break
        u += 0.125
        i += 1
    return total


def check_vertex(optimize, tag):
    name = "vk_vertex_loop_sample.vert"
    blob, literals, _ = lowered(name, optimize)
    places = [(x, y, 0.0625, 0.25) for x in (0.0, 1.0, 3.0, 6.0) for y in (0.3, 1.1, 9.0)]
    position = [0.25, -0.5, 0.5, 1.0]
    lanes = [{0: [float(n), 0.0, 0.0, 0.0]} for n in range(len(places))]

    def fetched(n, fetch, index):
        return [list(places[int(index)]), position][n]

    def sample(slot, coordinates, lod):
        return vertex_texel(coordinates, lod)

    check_models(f"{name}: v_color, {tag}", blob, lanes, literals, {VERTEX_LOOP: MAX_PASSES},
                 [vertex_sample(*place) for place in places], vertex_fetch=fetched, texture_lod=sample)
    check_models(f"{name}: gl_Position, {tag}", blob, lanes, literals, {VERTEX_LOOP: MAX_PASSES},
                 [position] * len(places), export=OPOS, vertex_fetch=fetched, texture_lod=sample)


def level_value(n):
    """The mip chain's level n, flat: levels 0 to 3 grey 16 + 64n, the rest 240."""
    return (16.0 + 64.0 * n if n < 4 else 240.0) / 255.0


def grid(column, row):
    """vk_loop_sample_grid.frag's colour at a place in its cell."""
    red = green = blue = 0.0
    start, i = column + 2 * row, 0
    while i < 4 + (column + row) % 5:
        if (32 * HEIGHTS[min(start + i, 63)] + 16) / 255.0 >= 1.0 - (i + 1) * 0.125:
            break
        red += level_value((i + column + row) % 4) * 0.0625
        green += level_value((i + column) % 4) * 0.0625 + TINTS[i]
        i += 1
    for k in range(row // 2 % 3 + 2):
        n = (k + column // 2) % 4
        blue += level_value(n) * 0.125 + 2.0 ** n / LEVELS_SIDE * 2.0
    return [red + i * 0.03125, green, blue, 1.0]


def check_grid(optimize, tag):
    name = "vk_loop_sample_grid.frag"
    blob, literals, _ = lowered(name, optimize)
    param = container.Container(blob).context_misc >> PARAM_GEN_POS_SHIFT & 0xFF
    heights = next(slot for var, slot in resolve_interface(module(name)).samplers if var.binding == 0)
    places = [(2 * qx + dx, 2 * qy + dy) for qy in range(5) for qx in range(5) for dy in (0, 1) for dx in (0, 1)]
    lanes = [{param: [float(1240 + column), float(row), 0.0, 0.0]} for column, row in places]
    constants = push_constants(name, [[t, 0.0, 0.0, 0.0] for t in TINTS])
    constants.update(literals)
    constants[native.FRAG_COORD_CONSTANT] = [0.5, 0.5, 0.0, 0.0]

    def sample(slot, coordinates, lod):
        if slot == heights:
            return [(32 * HEIGHTS[min(int(coordinates[0] * 64.0), 63)] + 16) / 255.0, 0.0, 0.0, 1.0]
        return [level_value(min(max(level(lod, LEVELS_SIDE), 0), 7))] * 3 + [1.0]

    check_models(f"{name}, every place in a cell, {tag}", blob, lanes, constants, {PIXEL_LOOP: MAX_PASSES},
                 [grid(column, row) for column, row in places], texture_lod=sample, quads=True)


def main():
    check_shape("vk_loop_sample_lod.frag", (4, 0))
    check_shape("vk_loop_sample_grad.frag", (3, 0))
    check_shape("vk_loop_sample_computed.frag", (1, 0))
    check_shape("vk_loop_derivative.frag", (2, 0))
    check_shape("vk_loop_parallax.frag", (3, 2))
    check_shape("vk_vertex_loop_sample.vert", (2, 0))
    check_shape("vk_loop_sample_grid.frag", (8, 0))
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        check_lod(optimize, tag)
        check_grad(optimize, tag)
        check_quads("vk_loop_sample_computed.frag", sample_computed, optimize, tag)
        check_quads("vk_loop_derivative.frag", derivative, optimize, tag)
        check_parallax(optimize, tag)
        check_vertex(optimize, tag)
        check_grid(optimize, tag)
    print("OK")


if __name__ == "__main__":
    main()
