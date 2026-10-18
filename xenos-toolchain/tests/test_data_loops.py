#!/usr/bin/env python3
"""Loops whose count is data (milestone 46 step 4, xsdk:docs/scope/46-loops.md, "Loops whose count is data";
xsdk:src/xsc_unroll.c, src/xsc_lower_flow.c).

xsc refused a loop counted by anything but a constant or a block's int from 0 by 1: a count from a float or an
interpolant, a while or a do/while on data, a block-counted loop from past 0 or by past 1. SM3 has no such loop either:
a loop on data is a counted loop broken out of on data. Each now runs as a hardware loop of 255 passes, the most a loop
constant counts (xenia:src/xenia/gpu/xenos.h:1086-1097), its condition a break at the top of a pass, or at the end for
a do/while, gated as any break on data is, its counter a value it carries from its start by its step; so a count past
255 stops at 255, as one SM3 loop does. The manifest's ("fixed_loop", loop constant, 255) is one loop constant for every
such loop of the stage. A block-counted loop from other than 0 or by past 1 stays counted by the block's int n, which
the shader can't read to test i against: as SM3's loop takes its count from an integer constant the CPU writes, the
manifest's ("block_loop", loop constant, kind, set, binding, offset, start, step) has the runtime write
ceil((n - start) / step) passes, 0 to 255 (native.loop_passes), each run, i carried from its start by its step. Each
start and step takes a loop constant of its own.

Each shader runs on xdis/simulate.py in both models, each pixel or vertex alone and all in one group, at counts of 0, 1,
counts that differ within the group and past 255, which stop at 255, with the optimiser off and on; a block loop's loop
constant from its need through native.loop_passes:

- vk_loop_float_count.frag: a count from a float uniform, a continue on data, the counter after the loop.
- vk_loop_data_count.frag: a count from an interpolant, left on data too; a group whose pixels all leave early runs as
  many passes as its last needs.
- vk_loop_while.frag: a while on data, left on data too, and a do/while, continued past on data, sharing a loop
  constant.
- vk_loop_count_skip.frag: block-counted loops from -3 and by 2 to the same int, each its own loop constant, the
  second left on data, both counters after, at counts below the start and past 255 passes.
- vk_loop_step_grid.frag: xsdk_test's raster grid, loops from 100 and by 2 to the same int, where the step matters past
  255: to 300 the first runs 200 passes, to 400 the second 200.
- vk_vertex_loop_data.vert: a vertex shader's while on a vertex input, continued past on data, its counter after.
- vk_loop_data_grid.frag: xsdk_test's raster grid, a for, a while and a do/while on counts 0 to 321 from each pixel's
  place, at every place in a 10-pixel cell.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_hardware_loop_breaks import (OPOS, PARAM_GEN_POS_SHIFT, PIXEL_LOOP, STEP, VERTEX_LOOP,  # noqa: E402
                                      check_models)
from test_local_stores import module, push_constants  # noqa: E402

MAX_PASSES = 255
# vk_loop_data_grid.frag's place: the count's scale, the while's bound past the count, and the do/while's scale.
GRID = [3.25, 330.0, 0.5, 0.0]


def add(total, step):
    return [t + s for t, s in zip(total, step)]


def float_count(x, count):
    total, i = [0.0] * 4, 0
    while i < int(count) and i < MAX_PASSES:
        if i != x:
            total = add(total, STEP)
        i += 1
    return total[:3] + [total[3] + i * 0.03125]


def data_count(x, y):
    total, i = [0.0] * 4, 0
    while i < int(x) and i < MAX_PASSES:
        total = add(total, STEP)
        if total[1] > y:
            break
        i += 1
    return total


def while_loops(x, y):
    total, t, passes = [0.0] * 4, 0.0, 0
    while t < x and passes < MAX_PASSES:
        passes += 1
        t += 1.0
        if t == y:
            break
        total[0] += STEP[0]
        total[1] += STEP[1]
    k, passes = 0.0, 0
    while True:
        passes += 1
        k += 1.0
        if k != y:
            total[2] += STEP[2]
            total[3] += STEP[3]
        if not k < x * 0.5 or passes == MAX_PASSES:
            break
    return [total[0] + t * 0.0625, total[1] + k * 0.0625, total[2], total[3]]


def count_skip(x, count):
    """The GLSL's, each loop stopping at 255 passes."""
    sum_x, sum_y, i, j = 0.0, 0.0, -3, 0
    while i < count and i < MAX_PASSES - 3:
        sum_x += STEP[0] * i
        i += 1
    while j < count and j < 2 * MAX_PASSES:
        sum_y += STEP[1]
        if j >= x:
            break
        j += 2
    return [sum_x, sum_y, i * 0.03125, j * 0.03125]


def step_grid(n):
    """The GLSL's, each loop stopping at 255 passes, scaled to a colour."""
    a, i = 0, 100
    while i < n and a < MAX_PASSES:
        a, i = a + 1, i + 1
    b, j = 0, 0
    while j < n and b < MAX_PASSES:
        b, j = b + 1, j + 2
    return [a / 255.0, b / 255.0, (i + j) * 0.25 / 255.0, 1.0]


def vertex_data(x, y):
    total, i = [0.0] * 4, 0
    while i < int(x) and i < MAX_PASSES:
        i += 1
        if i == y:
            continue
        total = add(total, STEP)
    return total[:3] + [total[3] + i * 0.125]


def data_grid(column, row, place):
    n = (column + 10 * row) * place[0]
    i = 0
    while i < int(n) and i < MAX_PASSES:
        i += 1
    t, passes = 0.0, 0
    while t < place[1] - n and passes < MAX_PASSES:
        passes += 1
        t += 1.0
        if t == column * 25.0 + 40.0:
            break
    k, s, passes = 0.0, 0.0, 0
    while True:
        passes += 1
        k += 1.0
        if k != row + 2.0:
            s += 1.0
        if not k < n * place[2] or passes == MAX_PASSES:
            break
    return [i / 255.0, t / 255.0, s / 255.0, 1.0]


def lowered(name, optimize):
    """The program, its literals, and its loops' needs."""
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
    loops = [entry for entry in manifest if entry[0] in ("block_loop", "fixed_loop")]
    return blob, {entry[0]: list(entry[1]) for entry in manifest if entry not in loops}, loops


def loops_of(blob):
    """Whether anything is on the predicate, each LOOP_START's loop constant and each LOOP_END's predicated break: its
    condition, or None."""
    parsed = container.Container(blob)
    predicated, starts, ends = False, [], []
    for rec in xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0]:
        if rec.kind == "cf" and isinstance(rec.detail, xu.ParsedExecInstruction):
            predicated = predicated or rec.detail.type == xu._ExecType.kPredicated
        elif rec.kind == "cf" and isinstance(rec.detail, xu.ParsedLoopStartInstruction):
            starts.append(rec.detail.loop_constant_index)
        elif rec.kind == "cf" and isinstance(rec.detail, xu.ParsedLoopEndInstruction):
            ends.append(rec.detail.predicate_condition if rec.detail.is_predicated_break else None)
        elif rec.kind == "alu":
            predicated = predicated or xu.AluInstruction(*rec.raw).is_predicated()
    return predicated, starts, ends


def check_shape(name, want_loops, want_starts, want_ends):
    blob, _, loops = lowered(name, False)
    got = (loops, loops_of(blob))
    want = (want_loops, (False, want_starts, want_ends))
    if got != want:
        raise SystemExit(f"  FAIL {name}: needs, and predicated, LOOP_STARTs and LOOP_ENDs, {got}, not {want}")
    print(f"  PASS {name}: needs {want_loops}, nothing on the predicate, loop constants {want_starts}, LOOP_END breaks "
          f"{want_ends}")


def fixed(name, optimize):
    """The program and its constants; its loop constant runs 255 passes."""
    blob, literals, _ = lowered(name, optimize)
    return blob, literals


def block_counted(loops, n):
    """Each block loop's loop constant where its int is n, as the runtime writes it."""
    return {entry[1]: native.loop_passes(entry[6], entry[7], n) for entry in loops}


def check_float_count(optimize, tag):
    name = "vk_loop_float_count.frag"
    blob, literals = fixed(name, optimize)
    places = (-1.0, 0.0, 2.0, 7.0, 200.0)
    lanes = [{0: [x, 0.0, 0.0, 0.0]} for x in places]
    for count in (0.0, 1.0, 3.5, 8.0, 255.0, 300.0):
        constants = push_constants(name, [STEP, [count, 0.0, 0.0, 0.0]])
        constants.update(literals)
        check_models(f"{name}, count {count}, {tag}", blob, lanes, constants, {PIXEL_LOOP: MAX_PASSES},
                     [float_count(x, count) for x in places])


def check_data_count(optimize, tag):
    name = "vk_loop_data_count.frag"
    blob, literals = fixed(name, optimize)
    constants = push_constants(name, [STEP])
    constants.update(literals)
    places = [(x, y) for x in (0.0, 1.0, 2.5, 7.0, 255.0, 300.0) for y in (0.1, 0.3, 99.0)]
    lanes = [{0: [x, y, 0.0, 0.0]} for x, y in places]
    check_models(f"{name}, counts 0 to 300 in a group, {tag}", blob, lanes, constants, {PIXEL_LOOP: MAX_PASSES},
                 [data_count(x, y) for x, y in places])
    short = [(x, 99.0) for x in (0.0, 1.0, 2.5, 3.0)]
    passes = check_models(f"{name}, counts 0 to 3 in a group, {tag}", blob, [{0: [x, y, 0.0, 0.0]} for x, y in short],
                          constants, {PIXEL_LOOP: MAX_PASSES}, [data_count(x, y) for x, y in short])
    if passes != 4:
        raise SystemExit(f"  FAIL {name}: every pixel left by its fourth pass, but the group ran {passes} of 255")


def check_while(optimize, tag):
    name = "vk_loop_while.frag"
    blob, literals = fixed(name, optimize)
    constants = push_constants(name, [STEP])
    constants.update(literals)
    places = [(x, y) for x in (-1.0, 0.0, 1.0, 2.5, 9.0, 300.0) for y in (0.0, 1.0, 2.0, 5.0)]
    lanes = [{0: [x, y, 0.0, 0.0]} for x, y in places]
    check_models(f"{name}, {tag}", blob, lanes, constants, {PIXEL_LOOP: MAX_PASSES},
                 [while_loops(x, y) for x, y in places])


def check_skip(optimize, tag):
    name = "vk_loop_count_skip.frag"
    blob, literals, loops = lowered(name, optimize)
    places = (0.0, 1.0, 3.0, 4.0, 99.0, 300.0, 600.0)
    lanes = [{0: [x, 0.0, 0.0, 0.0]} for x in places]
    for count in (-5, -3, -2, 0, 1, 2, 5, 8, 251, 252, 253, 300, 510, 511, 600):
        constants = push_constants(name, [STEP])
        constants.update(literals)
        check_models(f"{name}, count {count}, {tag}", blob, lanes, constants, block_counted(loops, count),
                     [count_skip(x, count) for x in places])


def check_steps(optimize, tag):
    name = "vk_loop_step_grid.frag"
    blob, literals, loops = lowered(name, optimize)
    lanes = [{0: [0.0] * 4}] * 2
    for n in (-5, 0, 100, 101, 102, 300, 355, 356, 400, 510, 511, 600):
        constants = push_constants(name, [[1.0 / 255.0, 0.0, 0.0, 0.0]])
        constants.update(literals)
        check_models(f"{name}, n {n}, {tag}", blob, lanes, constants, block_counted(loops, n), [step_grid(n)] * 2)


def check_vertex(optimize, tag):
    name = "vk_vertex_loop_data.vert"
    blob, literals = fixed(name, optimize)
    constants = push_constants(name, [STEP])
    constants.update(literals)
    places = [(x, y) for x in (0.0, 1.0, 3.5, 6.0, 300.0) for y in (0.0, 1.0, 3.0)]
    position = [0.25, -0.5, 0.5, 1.0]
    lanes = [{0: [float(n), 0.0, 0.0, 0.0]} for n in range(len(places))]

    def fetched(n, fetch, index):
        x, y = places[int(index)]
        return [[x, y, 0.0, 0.0], position][n]

    check_models(f"{name}: v_color, {tag}", blob, lanes, constants, {VERTEX_LOOP: MAX_PASSES},
                 [vertex_data(x, y) for x, y in places], vertex_fetch=fetched)
    check_models(f"{name}: gl_Position, {tag}", blob, lanes, constants, {VERTEX_LOOP: MAX_PASSES},
                 [position] * len(places), export=OPOS, vertex_fetch=fetched)


def check_grid(optimize, tag):
    name = "vk_loop_data_grid.frag"
    blob, literals = fixed(name, optimize)
    param = container.Container(blob).context_misc >> PARAM_GEN_POS_SHIFT & 0xFF
    places = [(column, row) for row in range(10) for column in range(10)]
    lanes = [{0: [0.0] * 4, param: [float(1240 + column), float(400 + row), 0.0, 0.0]} for column, row in places]
    constants = push_constants(name, [GRID])
    constants.update(literals)
    constants[native.FRAG_COORD_CONSTANT] = [0.5, 0.5, 0.0, 0.0]
    check_models(f"{name}, every place in a cell, {tag}", blob, lanes, constants, {PIXEL_LOOP: MAX_PASSES},
                 [data_grid(column, row, GRID) for column, row in places])


def main():
    fixed_loop = [("fixed_loop", PIXEL_LOOP, MAX_PASSES)]
    for name in ("vk_loop_float_count.frag", "vk_loop_data_count.frag"):
        check_shape(name, fixed_loop, [PIXEL_LOOP], [True])
    check_shape("vk_loop_while.frag", fixed_loop, [PIXEL_LOOP] * 2, [True] * 2)
    check_shape("vk_loop_data_grid.frag", fixed_loop, [PIXEL_LOOP] * 3, [True] * 3)
    check_shape("vk_vertex_loop_data.vert", [("fixed_loop", VERTEX_LOOP, MAX_PASSES)], [VERTEX_LOOP], [True])
    check_shape("vk_loop_count_skip.frag", [("block_loop", PIXEL_LOOP, "push", 0, 0, 16, 0, 2),
                                             ("block_loop", PIXEL_LOOP + 1, "push", 0, 0, 16, -3, 1)],
                [PIXEL_LOOP + 1, PIXEL_LOOP], [None, True])
    check_shape("vk_loop_step_grid.frag", [("block_loop", PIXEL_LOOP, "push", 0, 0, 16, 100, 1),
                                           ("block_loop", PIXEL_LOOP + 1, "push", 0, 0, 16, 0, 2)],
                [PIXEL_LOOP, PIXEL_LOOP + 1], [None, None])
    for optimize in (False, True):
        for each in (check_float_count, check_data_count, check_while, check_skip, check_steps, check_vertex,
                     check_grid):
            each(optimize, f"optimiser {'on' if optimize else 'off'}")
    print("OK")


if __name__ == "__main__":
    main()
