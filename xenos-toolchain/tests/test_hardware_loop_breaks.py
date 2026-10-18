#!/usr/bin/env python3
"""Breaks and continues on data in a hardware loop a block's int counts (milestone 46 step 3,
xsdk:docs/scope/46-loops.md, "The design"; xsdk:src/xsc_unroll.c, src/xsc_lower_flow.c).

xsc refused them: LOOP_END breaks only when all 64 vertices or pixels of a group agree (xenia:src/xenia/gpu/ucode.h:
372-373), so a pixel that leaves while others stay runs on. They're now gated as in an unrolled loop: each value keeps
a stopped copy where its pixel stops, running and alive flags say whether it runs the pass and is still in the loop,
and the pass's end selects. alive is a register the loop carries from 1; a setp_eq on it at the pass's end sets p0
where the pixel has left, and LOOP_END breaks on p0, so the group leaves early once none is left. The counter read
after the loop counts alive, so it holds the pass a pixel broke at. Nothing in the pass is on the predicate.

Each shader runs on xdis/simulate.py in both models: each pixel or vertex alone, its own LOOP_END breaking, as Xenia's
translator does, and all of them in one group, LOOP_END breaking once every one agrees, as the console does; each
gives the GLSL's result in both, for pixels leaving at every pass and never, with the optimiser off and on.

- vk_loop_count_grid.frag, xsdk_test's raster grid: a continue on data, then a break on data in an arm of an if/else,
  then one ending the pass, and the counter after the loop, at every place in a 10-pixel cell. With every pixel
  leaving at the first pass, the group runs one pass of 255, xsdk_test's timed check.
- vk_loop_count_break.frag: a break on data at the top of the pass, a continue on data, and a break ending the pass on
  a value every pixel shares, gated as the others are.
- vk_loop_count_stop.frag: a continue on data, then a break ending the pass on a value every pixel shares, which a
  pixel that continued doesn't reach, so it's gated too.
- vk_loop_count_continue.frag: continues on data alone, counted by a uniform buffer: no alive, no setp, LOOP_END
  unpredicated.
- vk_loop_break_data.frag: a break on data ending the pass, which was refused.
- vk_vertex_loop_count_break.vert: a vertex shader's continue on data and break on data in an arm, and its counter
  after the loop.

Today's if (c) break; ending a pass, on a value every pixel shares, keeps its setp on c and its LOOP_END
(tests/test_hardware_loops.py, vk_loop_count_if.frag).
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xdis.xenos_ucode as xu  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_hardware_loops import uniform_block  # noqa: E402
from test_local_stores import close, module, push_constants  # noqa: E402

PARAM_GEN_POS_SHIFT = 8  # SQ_CONTEXT_MISC (xenia:src/xenia/gpu/registers.h:234)
OPOS = int(xu.ExportRegister.kVSPosition)
PIXEL_LOOP = 16
VERTEX_LOOP = 0
STEP = [0.125, 0.0625, 0.25, 0.03125]
# vk_loop_count_grid.frag's place: the cell's column and row as they are, and every pixel leaving at the first pass.
GRID = [1.0, 1.0, 0.0, 0.0]
FIRST_PASS = [0.0, 0.0, 9.0, 0.0]


def count_grid(column, row, count, place):
    x, y = column * place[0] + place[2], row * place[1] + place[3]
    total, i = [0.0, 0.0, 0.0], 0
    while i < count:
        total[0] += 0.0625
        if i == x - 2:
            i += 1
            continue
        if x < 5:
            total[1] += 0.0625
            if i > x:
                break
        total[2] += (i + 1) * 0.015625
        if i >= y:
            break
        i += 1
    return [total[0] + i * 0.03125, total[1], total[2], 1.0]


def count_break(x, y, stop, count):
    total = [0.0, 0.0, 0.0, 1.0]
    for i in range(count):
        if i >= x:
            break
        total = [t + s for t, s in zip(total, STEP)]
        if i == y:
            continue
        total[3] -= 0.0625
        if i >= stop:
            break
    return total


def count_stop(x, stop, count):
    total = [0.0, 0.0, 0.0, 1.0]
    for i in range(count):
        if i == x:
            continue
        total = [t + s for t, s in zip(total, STEP)]
        total[3] -= 0.0625
        if i >= stop:
            break
    return total


def count_continue(x, y, count):
    total = [0.0] * 4
    for i in range(count):
        if i == x:
            continue
        total = [t + s for t, s in zip(total, STEP)]
        if i == y:
            continue
        total[3] += 0.0625
    return total


def break_data(x, count):
    total = [0.0] * 4
    for _ in range(count):
        total = [t + s for t, s in zip(total, STEP)]
        if total[0] > x:
            break
    return total


def vertex_count_break(x, y, z, count):
    total, i = [0.0] * 4, 0
    while i < count:
        if i == y:
            i += 1
            continue
        total = [t + s for t, s in zip(total, STEP)]
        if z > 0.5:
            if i >= x:
                break
        else:
            total[3] += 0.0625
        i += 1
    return total[:3] + [total[3] + i * 0.125]


def lowered(name, optimize):
    blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
    return blob, {entry[0]: list(entry[1]) for entry in manifest if entry[0] != "block_loop"}


def shape(blob):
    """Whether anything in the program is on the predicate, its setps, and each LOOP_END's predicated break: its
    condition, or None."""
    parsed = container.Container(blob)
    predicated, setps, ends = False, [], []
    for rec in xu._decode_program(parsed.ucode_dwords, parsed.shader_type)[0]:
        if rec.kind == "cf" and isinstance(rec.detail, xu.ParsedExecInstruction):
            predicated = predicated or rec.detail.type == xu._ExecType.kPredicated
        elif rec.kind == "cf" and isinstance(rec.detail, xu.ParsedLoopEndInstruction):
            ends.append(rec.detail.predicate_condition if rec.detail.is_predicated_break else None)
        elif rec.kind == "alu":
            op = xu.AluInstruction(*rec.raw)
            predicated = predicated or op.is_predicated()
            if xu.AluScalarOpcode.kSetpEq <= op.scalar_opcode() <= xu.AluScalarOpcode.kSetpGe:
                setps.append(xu.AluScalarOpcode(op.scalar_opcode()).name)
    return predicated, setps, ends


def check_shape(name, want_setps, want_ends):
    got = shape(lowered(name, False)[0])
    if got != (False, want_setps, want_ends):
        raise SystemExit(f"  FAIL {name}: predicated, setps and LOOP_ENDs {got}, not {(False, want_setps, want_ends)}")
    print(f"  PASS {name}: nothing on the predicate, setps {want_setps}, LOOP_END breaks {want_ends}")


def check_models(label, blob, lanes, constants, loops, wants, export=0, vertex_fetch=None):
    """Every lane's result run alone and in one group; the group's passes."""
    alone = [simulate.run(blob, r, constants, vertex_fetch, loops=loops) for r in lanes]
    group = simulate.run_group(blob, lanes, constants, vertex_fetch, loops=loops)
    for model, results in (("alone", alone), ("in a group", group)):
        wrong = [i for i, r in enumerate(results) if not close(r.exports[export], wants[i])]
        if wrong:
            got = [round(x, 5) for x in results[wrong[0]].exports[export]]
            raise SystemExit(f"  FAIL {label}, {model}: {len(wrong)} of {len(lanes)} wrong, the first lane "
                             f"{wrong[0]} {got} where the GLSL gives {wants[wrong[0]]}")
    print(f"  PASS {label}: all {len(lanes)} alone and in a group, which ran {group[0].passes} passes")
    return group[0].passes


def check_grid(optimize, tag):
    name = "vk_loop_count_grid.frag"
    blob, literals = lowered(name, optimize)
    param = container.Container(blob).context_misc >> PARAM_GEN_POS_SHIFT & 0xFF
    places = [(column, row) for row in range(10) for column in range(10)]
    lanes = [{0: [0.0] * 4, param: [float(640 + column), float(370 + row), 0.0, 0.0]} for column, row in places]
    for place, count in ((GRID, 0), (GRID, 3), (GRID, 8), (GRID, 12), (FIRST_PASS, 1), (FIRST_PASS, 255)):
        constants = push_constants(name, [place])
        constants.update(literals)
        constants[native.FRAG_COORD_CONSTANT] = [0.5, 0.5, 0.0, 0.0]
        wants = [count_grid(column, row, count, place) for column, row in places]
        passes = check_models(f"{name}, place {place}, {count} passes, {tag}", blob, lanes, constants,
                              {PIXEL_LOOP: count}, wants)
        if place == FIRST_PASS and passes != 1:
            raise SystemExit(f"  FAIL {name}: every pixel left at the first pass, but the group ran {passes} of "
                             f"{count}")


def check_break(optimize, tag):
    name = "vk_loop_count_break.frag"
    blob, literals = lowered(name, optimize)
    places = [(x, y) for x in (0.0, 1.0, 2.5, 4.0, 9.0) for y in (-1.0, 0.0, 1.0, 2.0, 3.0)]
    lanes = [{0: [x, y, 0.0, 0.0]} for x, y in places]
    for stop in (2.0, 99.0):
        for count in (0, 3, 6):
            constants = push_constants(name, [STEP, [stop, 0.0, 0.0, 0.0]])
            constants.update(literals)
            wants = [count_break(x, y, stop, count) for x, y in places]
            check_models(f"{name}, stop {stop}, {count} passes, {tag}", blob, lanes, constants,
                         {PIXEL_LOOP: count}, wants)


def check_stop(optimize, tag):
    name = "vk_loop_count_stop.frag"
    blob, literals = lowered(name, optimize)
    places = (-1.0, 0.0, 1.0, 2.0)
    lanes = [{0: [x, 0.5, 0.0, 0.0]} for x in places]
    for stop in (1.0, 99.0):
        for count in (0, 3, 5):
            constants = push_constants(name, [STEP, [stop, 0.0, 0.0, 0.0]])
            constants.update(literals)
            check_models(f"{name}, stop {stop}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count},
                         [count_stop(x, stop, count) for x in places])


def check_continue(optimize, tag):
    name = "vk_loop_count_continue.frag"
    blob, literals = lowered(name, optimize)
    places = [(x, y) for x in (-1.0, 0.0, 2.0) for y in (-1.0, 1.0, 2.0, 3.0)]
    lanes = [{0: [x, y, 0.0, 0.0]} for x, y in places]
    for count in (0, 4):
        constants = uniform_block(name, [STEP])
        constants.update(literals)
        wants = [count_continue(x, y, count) for x, y in places]
        check_models(f"{name}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count}, wants)


def check_break_data(optimize, tag):
    name = "vk_loop_break_data.frag"
    blob, literals = lowered(name, optimize)
    places = (0.0, 0.2, 0.3, 0.45, 1.0, 5.0)
    lanes = [{0: [x, 0.5, 0.0, 0.0]} for x in places]
    for count in (0, 1, 4, 8):
        constants = push_constants(name, [STEP])
        constants.update(literals)
        check_models(f"{name}, {count} passes, {tag}", blob, lanes, constants, {PIXEL_LOOP: count},
                     [break_data(x, count) for x in places])


def check_vertex(optimize, tag):
    name = "vk_vertex_loop_count_break.vert"
    blob, literals = lowered(name, optimize)
    places = [(x, y, z) for x in (0.0, 1.0, 3.0, 9.0) for y in (-1.0, 0.0, 2.0) for z in (0.0, 1.0)]
    position = [0.25, -0.5, 0.5, 1.0]
    lanes = [{0: [float(n), 0.0, 0.0, 0.0]} for n in range(len(places))]

    def fetched(n, fetch, index):
        x, y, z = places[int(index)]
        return [[x, y, z, 0.0], position][n]

    for count in (0, 2, 5):
        constants = push_constants(name, [STEP])
        constants.update(literals)
        label = f"{name}, {count} passes, {tag}"
        check_models(label + ": v_color", blob, lanes, constants, {VERTEX_LOOP: count},
                     [vertex_count_break(x, y, z, count) for x, y, z in places], vertex_fetch=fetched)
        check_models(label + ": gl_Position", blob, lanes, constants, {VERTEX_LOOP: count},
                     [position] * len(places), export=OPOS, vertex_fetch=fetched)


def main():
    for name in ("vk_loop_count_grid.frag", "vk_loop_count_break.frag", "vk_loop_count_stop.frag",
                 "vk_loop_break_data.frag", "vk_vertex_loop_count_break.vert"):
        check_shape(name, ["kSetpEq"], [True])
    check_shape("vk_loop_count_continue.frag", [], [None])
    for optimize in (False, True):
        for each in (check_grid, check_break, check_stop, check_continue, check_break_data, check_vertex):
            each(optimize, f"optimiser {'on' if optimize else 'off'}")
    print("OK")


if __name__ == "__main__":
    main()
