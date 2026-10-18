#!/usr/bin/env python3
"""An if/else on data inside a loop's body lowers to a select each pass (xsdk:src/xsc_unroll.c).

The unroller refused a conditional store in a loop body: its environment held one value a local. It now runs both arms
of each pass's if/else from the locals as they were and merges what they leave apart with a select, so
vk_loop_if.frag, which adds a step to its sum on the passes whose threshold the pixel's u passes and an eighth to the
sum's w on the others, gives what the GLSL says either side of each threshold. vk_loop_uniform_if.frag's if/else in its
body is on a push constant, so each pass's arms go in execs on the predicate; it gives what the GLSL says for both
values, the simulator following the control flow.

A break or continue on a condition the unroller folds ends the pass where it stands: vk_loop_folded_break.frag stops
after the pass that breaks, and an inner loop's break leaves the outer loop running; vk_loop_folded_continue.frag skips
the rest of the pass that continues. The unroller once dropped the end of the arm it took and ran every pass.

A break or continue on data is gated (milestone 46 step 2, xsdk:docs/scope/46-loops.md): each value keeps a stopped
copy where the pixel stops, flags say whether it runs the pass and is still in the loop, and the pass's end selects.
vk_loop_if_break.frag breaks at each of its 4 passes and never; vk_loop_grid.frag, xsdk_test's raster grid, breaks in
arms of an if/else on data, continues on data and breaks where the unroller folds it, at every column and row of its
10-pixel cells; vk_vertex_loop_break.vert does the same in a vertex shader, a folded continue and break among them.
Each gives the GLSL's result for every pixel or vertex, with the optimiser off and on.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import lower_ex  # noqa: E402
from test_local_stores import check, module, push_constants  # noqa: E402
from test_vertex_branches import predication  # noqa: E402

PARAM_GEN_POS_SHIFT = 8  # SQ_CONTEXT_MISC (xenia:src/xenia/gpu/registers.h:234)

STEP = [0.125, 0.0625, 0.25, 0.03125]


def loop_if(u):
    total = [0.0] * 4
    for i in range(4):
        if u > i * 0.25:
            total = [t + s for t, s in zip(total, STEP)]
        else:
            total[3] += 0.125
    return total


def loop_uniform_if(uv, mode):
    total = [uv[0], uv[1], 0.0, 1.0]
    for _ in range(3):
        if mode > 0.5:
            total = [t * 0.5 + s for t, s in zip(total, STEP)]
        else:
            total[2] += 0.25
    return total


def loop_folded_break(uv, step):
    total = [uv[0], uv[1], 0.0, 0.0]
    for i in range(4):
        total = [t + s for t, s in zip(total, step)]
        if i == 2:
            break
    for i in range(3):
        for j in range(4):
            if j == i:
                break
            total[3] += 0.0625
    return total


def loop_folded_continue(uv, step):
    total = [uv[0], uv[1], 0.0, 0.0]
    for i in range(4):
        if i == 1:
            continue
        total = [t + s * (i + 1) for t, s in zip(total, step)]
    return total


def loop_if_break(x, step):
    total = [0.0] * 4
    for _ in range(4):
        total = [t + s for t, s in zip(total, step)]
        if total[0] > x:
            break
    return total


def loop_grid(column, row):
    """vk_loop_grid.frag at a pixel's column and row in its cell, as tests/test_raster.c works it out."""
    total = [0.0, 0.0, 0.0, 1.0]
    for i in range(8):
        if i == 6:
            break
        total[0] += 0.0625
        if i == row:
            continue
        if column < 5:
            total[1] += 0.0625
            if i > column:
                break
        elif i + 3 > column:
            break
        total[2] += (i + 1) * 0.03125
    return total


def vertex_loop(x, y):
    total = [0.0, 0.0, 0.0, 1.0]
    for i in range(6):
        if i == 1:
            continue
        total[0] += 0.125
        if i == y:
            continue
        total[1] += (i + 1) * 0.0625
        if i == x:
            break
        total[2] += 0.125
        if i == 4:
            break
    return total


def check_data_breaks():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        name = "vk_loop_if_break.frag"
        blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
        constants = push_constants(name, [STEP])
        constants.update({r: list(v) for r, v in manifest})
        # Past 0.125, 0.25, 0.375 and 0.5 its sum breaks at passes 0 to 3; 1.0 never breaks.
        for x in (0.0, 0.2, 0.3, 0.45, 1.0):
            got = simulate.run(blob, registers={0: [x, 0.0, 0.0, 0.0]}, constants=constants).exports[0]
            check(f"{name} at x {x}, {tag}", got, loop_if_break(x, STEP))

        name = "vk_loop_grid.frag"
        blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
        constants = {r: list(v) for r, v in manifest}
        constants[native.FRAG_COORD_CONSTANT] = [0.5, 0.5, 0.0, 0.0]
        param = container.Container(blob).context_misc >> PARAM_GEN_POS_SHIFT & 0xFF
        wrong = []
        for column in range(10):
            for row in range(10):
                registers = {0: [0.0] * 4, param: [float(640 + column), float(370 + row), 0.0, 0.0]}
                got = simulate.run(blob, registers=registers, constants=constants).exports[0]
                want = loop_grid(column, (370 + row) % 10)
                if any(abs(g - w) > 1e-4 for g, w in zip(got, want)):
                    wrong.append((column, row, got, want))
        if wrong:
            raise SystemExit(f"  FAIL {name}, {tag}: {len(wrong)} of 100 pixels wrong, first {wrong[0]}")
        print(f"  PASS {name}, {tag}: all 100 places in a cell")

        name = "vk_vertex_loop_break.vert"
        blob, manifest = lower_ex(module(name), XSDK_REGISTERS, optimize)
        constants = {r: list(v) for r, v in manifest}
        position = [0.25, -0.5, 0.5, 1.0]
        # Breaks at passes 0, 2, 3 and 4, and never; continues past the same passes, and never.
        for x in (0.0, 2.0, 3.0, 4.0, -1.0):
            for y in (0.0, 2.0, 3.0, 4.0, 9.0):
                fetched = [[x, y, 0.0, 0.0], position]
                exports = simulate.run(blob, registers={0: [0.0] * 4}, constants=constants,
                                       vertex_fetch=lambda n, fetch, index: fetched[n]).exports
                check(f"{name} at x {x}, y {y}, {tag}", exports[0], vertex_loop(x, y))


def check_folded():
    uv = [0.75, 0.25]
    for name, want in (("vk_loop_folded_break.frag", loop_folded_break),
                       ("vk_loop_folded_continue.frag", loop_folded_continue)):
        blob, manifest = lower_ex(module(name), XSDK_REGISTERS)
        constants = push_constants(name, [STEP])
        constants.update({r: list(v) for r, v in manifest})
        got = simulate.run(blob, registers={0: uv + [0.0, 0.0]}, constants=constants).exports[0]
        check(name, got, want(uv, STEP))


def check_uniform():
    execs, setp = predication("vk_loop_uniform_if.frag")
    conditions = [c for c, _ in execs]
    if not setp or conditions.count(True) < 3 or conditions.count(False) < 3 or not all(ok for _, ok in execs):
        raise SystemExit(f"  FAIL vk_loop_uniform_if.frag: execs {conditions}, a setp {setp}")
    print(f"  PASS vk_loop_uniform_if.frag: each pass's arms in execs on the predicate, {conditions}")
    blob, manifest = lower_ex(module("vk_loop_uniform_if.frag"), XSDK_REGISTERS)
    uv = [0.75, 0.25]
    for mode in (0.0, 1.0):
        constants = push_constants("vk_loop_uniform_if.frag", [STEP, [mode, 0.0, 0.0, 0.0]])
        constants.update({r: list(v) for r, v in manifest})
        got = simulate.run(blob, registers={0: uv + [0.0, 0.0]}, constants=constants).exports[0]
        check(f"vk_loop_uniform_if.frag, mode {mode}", got, loop_uniform_if(uv, mode))


def main():
    check_uniform()
    check_folded()
    blob, manifest = lower_ex(module("vk_loop_if.frag"), XSDK_REGISTERS)
    constants = push_constants("vk_loop_if.frag", [STEP])
    constants.update({r: list(v) for r, v in manifest})
    for u in (0.0, 0.125, 0.375, 0.625, 0.875):
        got = simulate.run(blob, registers={0: [u, 0.5, 0.0, 0.0]}, constants=constants).exports[0]
        check(f"vk_loop_if.frag at u {u}", got, loop_if(u))
    check_data_breaks()
    print("OK")


if __name__ == "__main__":
    main()
