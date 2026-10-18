#!/usr/bin/env python3
"""Nested hardware loops, and hardware loops in an if/else or an unrolled loop (milestone 46 step 5,
xsdk:docs/scope/46-loops.md, "Nested loops"; xsdk:src/xsc_unroll.c, src/xsc_lower_flow.c, src/xsc_program.c).

xsc refused a hardware loop inside another loop or an if/else on data. LOOP_START nests 4 deep, each level its own
count (xenia:src/xenia/gpu/ucode.h:131; xsdk:docs/HARDWARE.md, "Loops on the console, milestone 46 step 1"), so loops
a block's int or data counts now nest to 4 deep and a fifth is refused. Each level keeps its own gating; an inner
loop's alive starts from the running of the pass it's in, so a pixel that doesn't run that pass starts the inner loop
out of it. Where two loops open at one stage the outer LOOP_START comes first, and the inner LOOP_END first. A hardware
loop inside an if/else on data runs for every pixel, as both arms do, merged by selects; one inside an if/else on a
uniform makes the if/else selects too, as a loop isn't an exec; one inside an unrolled loop is copied once a pass, the
copies sharing a loop constant. Loop constants still go one per count, so nested loops of one count share one.

Each shader runs on xdis/simulate.py in both models, each pixel or vertex alone and all in one group, the group's
pixels diverging at every level, with the optimiser off and on:

- vk_loop_nest2.frag: a loop a push constant counts, continued past on data, around a while on data left on data,
  which leaves the while alone, then left on data after the while, so its alive comes after the while's carried
  values; both counters read after.
- vk_loop_nest4.frag: 4 deep, loops a push constant counts at the first and third levels and loops on data at the
  second and fourth, each pair sharing a loop constant; a continue on data at the third level, a break on data at the
  fourth and one ending the second's pass; the fourth reads the push block's rows at the third's counter and the third
  at the first's, a0 loaded in each pass.
- vk_loop_nest_grid.frag, xsdk_test's raster grid: 3 deep, a push constant's loop continued past on data, a while on
  data left on data, and in its pass an if/else on data, a checkerboard, holding a loop another push constant counts,
  left on data, and a do/while on data sharing the while's loop constant.
- vk_loop_in_data_if.frag: hardware loops in both arms of an if/else on data.
- vk_loop_in_uniform_if.frag: a hardware loop in an arm of an if/else on a push constant: selects, nothing on the
  predicate.
- vk_loop_in_unrolled.frag: a hardware loop inside an unrolled loop left on data, copied 3 times, one loop constant.
- vk_vertex_loop_nest.vert: a vertex shader's while on an input around a loop a push constant counts, each left on
  data.
- vk_loop_nest5.frag, 5 deep: refused.
"""
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_data_loops import MAX_PASSES, check_shape, lowered  # noqa: E402
from test_hardware_loop_breaks import (OPOS, PARAM_GEN_POS_SHIFT, PIXEL_LOOP, STEP, VERTEX_LOOP,  # noqa: E402
                                      check_models)
from test_local_stores import module, push_constants  # noqa: E402

FIXED_LOOP = PIXEL_LOOP + 1
# vk_loop_nest_grid.frag's place: each pixel's column and row in its cell, as xsdk_test draws it.
GRID = [1.0, 1.0, 0.0, 0.0]
# vk_loop_nest4.frag's rows, each its own.
ROWS = [[0.125 * (r + 1), 0.0625, 0.03125 * r, 0.25 + 0.0625 * r] for r in range(4)]


def add(total, step, scale=1.0):
    return [t + s * scale for t, s in zip(total, step)]


def nest2(x, y, z, w, count):
    total, i = [0.0] * 4, 0
    while i < count:
        if i == y:
            i += 1
            continue
        t, passes = 0.0, 0
        while t < x + i and passes < MAX_PASSES:
            passes += 1
            t += 1.0
            if t == z:
                break
            total = add(total, STEP)
        total[3] += t * 0.0625
        if total[0] > w:
            break
        i += 1
    return total[:3] + [total[3] + i * 0.03125]


def nest4(x, y, z, w, count):
    total = [0.0] * 4
    for i in range(count):
        t, t_passes = 0.0, 0
        while t < x and t_passes < MAX_PASSES:
            t_passes += 1
            t += 1.0
            for j in range(count):
                if j == y + i:
                    continue
                k, k_passes = 0.0, 0
                while True:
                    k_passes += 1
                    k += 1.0
                    if k > z + t:
                        break
                    total = add(total, ROWS[j])
                    if not k < j + 2 or k_passes == MAX_PASSES:
                        break
                total[3] += ROWS[i][3]
            if t >= w:
                break
        total[0] += t * 0.125
    return total


def nest_grid(column, row, outer, inner, place):
    x, y = column * place[0] + place[2], row * place[1] + place[3]
    s = x + y
    skip, checker = s - 4.0 * math.floor(s * 0.25), s - 2.0 * math.floor(s * 0.5)
    a = b = c = 0.0
    for i in range(outer):
        if i == skip:
            continue
        t, t_passes = 0.0, 0
        while t < x * 0.5 + i and t_passes < MAX_PASSES:
            t_passes += 1
            t += 1.0
            if t == y - i:
                break
            if checker > 0.5:
                for j in range(inner):
                    if j >= y - t:
                        break
                    c += 1.0
            else:
                k, k_passes = 0.0, 0
                while True:
                    k_passes += 1
                    k += 1.0
                    b += 1.0
                    if not k < t * 0.25 + x * 0.25 - y * 0.5 or k_passes == MAX_PASSES:
                        break
        a += t
    return [a * 0.03125, b * 0.015625, c * 0.015625, 1.0]


def in_data_if(x, y, z, count):
    total = [0.0] * 4
    if x > 2.5:
        for i in range(count):
            total = add(total, STEP)
            if i >= y:
                break
        return total
    t, passes = 0.0, 0
    while t < y and passes < MAX_PASSES:
        passes += 1
        t += 1.0
        if t == z:
            continue
        total = add(total, STEP, 2.0)
    total[3] += t * 0.0625
    return total


def in_uniform_if(place, mode, count):
    if mode <= 0.5:
        return [p * 0.25 for p in place]
    total = [0.5] * 4
    for i in range(count):
        if i >= place[0]:
            break
        total = add(total, STEP)
    return total


def in_unrolled(x, y, count):
    total = [0.0] * 4
    for k in range(3):
        if x < k:
            break
        for i in range(count):
            if i >= y + k:
                break
            total = add(total, STEP, k + 1.0)
    return total


def vertex_nest(x, y, z, w, count):
    total, t, passes = [0.0] * 4, 0.0, 0
    while t < x and passes < MAX_PASSES:
        passes += 1
        t += 1.0
        for i in range(count):
            if i == y:
                continue
            total = add(total, STEP)
            if i + t >= z:
                break
        if t == w:
            break
    return total[:3] + [total[3] + t * 0.125]


def constants_of(name, values, optimize):
    blob, literals, _ = lowered(name, optimize)
    constants = push_constants(name, values)
    constants.update(literals)
    return blob, constants


def check_nest2(optimize, tag):
    name = "vk_loop_nest2.frag"
    blob, constants = constants_of(name, [STEP], optimize)
    places = [(x, y, z, w) for x in (0.0, 0.5, 1.0, 2.5, 4.0) for y in (-1.0, 0.0, 1.0, 2.0) for z in (0.0, 1.0, 3.5)
              for w in (0.2, 9.0)]
    lanes = [{0: list(p)} for p in places]
    for count in (0, 1, 3, 5):
        check_models(f"{name}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count, FIXED_LOOP: 255},
                     [nest2(*p, count) for p in places])
    # Every pixel continuing past the outer loop's one pass starts the inner loop out of it: one pass, not its count.
    skipped = [(x, 0.0, 9.0, 9.0) for x in (1.0, 2.5, 4.0)]
    passes = check_models(f"{name}, every pixel continuing past the one pass, {tag}", blob,
                          [{0: list(p)} for p in skipped], constants, {PIXEL_LOOP: 1, FIXED_LOOP: 255},
                          [nest2(*p, 1) for p in skipped])
    if passes != 2:
        raise SystemExit(f"  FAIL {name}: every pixel skipped the outer pass, but the group ran {passes} passes, not 2")


def check_nest4(optimize, tag):
    name = "vk_loop_nest4.frag"
    blob, constants = constants_of(name, ROWS, optimize)
    places = [(x, y, z, w) for x in (0.0, 1.0, 2.5) for y in (-1.0, 0.0, 1.0) for z in (-1.0, 0.5, 2.0)
              for w in (1.0, 2.0, 9.0)]
    lanes = [{0: list(p)} for p in places]
    for count in (0, 1, 3):
        check_models(f"{name}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count, FIXED_LOOP: 255},
                     [nest4(*p, count) for p in places])


def check_grid(optimize, tag):
    name = "vk_loop_nest_grid.frag"
    blob, constants = constants_of(name, [GRID], optimize)
    constants[native.FRAG_COORD_CONSTANT] = [0.5, 0.5, 0.0, 0.0]
    param = container.Container(blob).context_misc >> PARAM_GEN_POS_SHIFT & 0xFF
    places = [(column, row) for row in range(10) for column in range(10)]
    lanes = [{0: [0.0] * 4, param: [float(column), float(120 + row), 0.0, 0.0]} for column, row in places]
    for outer, inner in ((4, 3), (3, 4), (0, 3), (4, 0), (1, 1)):
        loops = {PIXEL_LOOP: outer, FIXED_LOOP: 255, PIXEL_LOOP + 2: inner}
        check_models(f"{name}, counts {outer} and {inner}, {tag}", blob, lanes, constants, loops,
                     [nest_grid(column, row, outer, inner, GRID) for column, row in places])


def check_data_if(optimize, tag):
    name = "vk_loop_in_data_if.frag"
    blob, constants = constants_of(name, [STEP], optimize)
    places = [(x, y, z) for x in (0.0, 3.0) for y in (-1.0, 0.0, 1.0, 2.5, 4.0) for z in (0.0, 1.0, 2.0)]
    lanes = [{0: [x, y, z, 0.0]} for x, y, z in places]
    for count in (0, 2, 5):
        check_models(f"{name}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count, FIXED_LOOP: 255},
                     [in_data_if(x, y, z, count) for x, y, z in places])


def check_uniform_if(optimize, tag):
    name = "vk_loop_in_uniform_if.frag"
    places = [[x, 0.5, 0.25, 1.0] for x in (-1.0, 0.0, 1.0, 2.5, 6.0)]
    lanes = [{0: p} for p in places]
    for mode in (0.0, 1.0):
        blob, constants = constants_of(name, [STEP, [mode, 0.0, 0.0, 0.0]], optimize)
        for count in (0, 3):
            check_models(f"{name}, mode {mode}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count},
                         [in_uniform_if(p, mode, count) for p in places])


def check_unrolled(optimize, tag):
    name = "vk_loop_in_unrolled.frag"
    blob, constants = constants_of(name, [STEP], optimize)
    places = [(x, y) for x in (-1.0, 0.0, 1.5, 2.0) for y in (-2.0, 0.0, 1.0, 2.5)]
    lanes = [{0: [x, y, 0.0, 0.0]} for x, y in places]
    for count in (0, 2, 5):
        check_models(f"{name}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count},
                     [in_unrolled(x, y, count) for x, y in places])


def check_vertex(optimize, tag):
    name = "vk_vertex_loop_nest.vert"
    blob, constants = constants_of(name, [STEP], optimize)
    places = [(x, y, z, w) for x in (0.0, 1.0, 3.5) for y in (-1.0, 0.0, 1.0) for z in (1.0, 2.0, 9.0)
              for w in (1.0, 2.0)]
    position = [0.25, -0.5, 0.5, 1.0]
    lanes = [{0: [float(n), 0.0, 0.0, 0.0]} for n in range(len(places))]

    def fetched(n, fetch, index):
        return [list(places[int(index)]), position][n]

    for count in (0, 2, 4):
        loops = {VERTEX_LOOP: 255, VERTEX_LOOP + 1: count}
        label = f"{name}, {count} passes, {tag}"
        check_models(label + ": v_color", blob, lanes, constants, loops,
                     [vertex_nest(*p, count) for p in places], vertex_fetch=fetched)
        check_models(label + ": gl_Position", blob, lanes, constants, loops, [position] * len(places), export=OPOS,
                     vertex_fetch=fetched)


def check_refused(name, words):
    try:
        lower_ex(module(name), XSDK_REGISTERS)
    except LowerError as error:
        if words not in str(error):
            raise SystemExit(f"  FAIL {name} refused, but as: {error}")
        print(f"  PASS {name} refused: {error}")
        return
    raise SystemExit(f"  FAIL {name} compiled")


def main():
    block = ("block_loop", PIXEL_LOOP, "push", 0, 0, 16, 0, 1)
    fixed = ("fixed_loop", FIXED_LOOP, MAX_PASSES)
    check_shape("vk_loop_nest2.frag", [block, fixed], [PIXEL_LOOP, FIXED_LOOP], [True, True])
    check_shape("vk_loop_nest4.frag", [("block_loop", PIXEL_LOOP, "push", 0, 0, 64, 0, 1), fixed],
                [PIXEL_LOOP, FIXED_LOOP] * 2, [True, True, True, None])
    check_shape("vk_loop_nest_grid.frag", [block, fixed, ("block_loop", PIXEL_LOOP + 2, "push", 0, 0, 20, 0, 1)],
                [PIXEL_LOOP, FIXED_LOOP, FIXED_LOOP, PIXEL_LOOP + 2], [True, True, True, None])
    check_shape("vk_loop_in_data_if.frag", [block, fixed], [PIXEL_LOOP, FIXED_LOOP], [True, True])
    check_shape("vk_loop_in_uniform_if.frag", [("block_loop", PIXEL_LOOP, "push", 0, 0, 32, 0, 1)], [PIXEL_LOOP],
                [True])
    check_shape("vk_loop_in_unrolled.frag", [block], [PIXEL_LOOP] * 3, [True] * 3)
    check_shape("vk_vertex_loop_nest.vert", [("fixed_loop", VERTEX_LOOP, MAX_PASSES),
                                             ("block_loop", VERTEX_LOOP + 1, "push", 0, 0, 16, 0, 1)],
                [VERTEX_LOOP, VERTEX_LOOP + 1], [True, True])
    check_refused("vk_loop_nest5.frag", "nested more than 4 deep")
    for optimize in (False, True):
        for each in (check_nest2, check_nest4, check_grid, check_data_if, check_uniform_if, check_unrolled,
                     check_vertex):
            each(optimize, f"optimiser {'on' if optimize else 'off'}")
    print("OK")


if __name__ == "__main__":
    main()
