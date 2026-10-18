#!/usr/bin/env python3
"""A scalar interpolant or vertex input read as a value (xsdk:src/xsc_lower.c, load_operand).

Such an input holds its value in x alone: a scalar interpolant takes whatever the vertex shader's register held in y, z
and w, and a float attribute's fetch fills them with the format's other lanes. xsc read the whole register as the value,
as if the scalar filled every component, so whatever read it there, arithmetic, an int conversion and the comparisons
and loop counts made of them, worked each component out apart: a loop counted by int(v_count) gated each component of
what it carries on that component's count. The input is now spread from x over a register of its own first, as a
scalar a block holds is.

Each shader runs on xdis/simulate.py in both models, each pixel or vertex alone and all in one group, with the optimiser
off and on, the interpolant's y, z and w holding 9, -7 and 5, and the attribute fetched as (w, 0, 0, 1):

- vk_scalar_input_loop.frag: a loop counted by int(v_count), a scalar interpolant, carrying a vec4.
- vk_scalar_input.frag: outside a hardware loop, a scalar interpolant scaled, chosen on as an int, and the int an
  unrolled loop breaks at.
- vk_vertex_scalar_input.vert: a float attribute scaled and as a loop's count carrying a vec4.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

from test_data_loops import MAX_PASSES, lowered  # noqa: E402
from test_hardware_loop_breaks import OPOS, PIXEL_LOOP, VERTEX_LOOP, check_models  # noqa: E402

# What a scalar interpolant's register holds past x, and each pass's step.
REST = [9.0, -7.0, 5.0]
STEP = [0.125, 0.25, 0.5, 0.0625]
VALUES = (-0.5, 0.0, 0.5, 1.5, 2.5, 3.5, 6.0)


def passes(value):
    return min(max(int(value), 0), MAX_PASSES)


def check_loop(optimize, tag):
    name = "vk_scalar_input_loop.frag"
    blob, literals, _ = lowered(name, optimize)
    lanes = [{0: [v] + REST, 1: STEP} for v in VALUES]
    check_models(f"{name}, counts {VALUES}, {tag}", blob, lanes, literals, {PIXEL_LOOP: MAX_PASSES},
                 [[s * passes(v) for s in STEP] for v in VALUES])


def outside(s):
    taken = 0.5 if int(s) > 1 else 0.125
    broken = int(s) if 0 <= int(s) < 4 else 4
    return [s * 0.25 + taken + 0.0625 * broken] * 4


def check_outside(optimize, tag):
    name = "vk_scalar_input.frag"
    blob, literals, _ = lowered(name, optimize)
    check_models(f"{name}, values {VALUES}, {tag}", blob, [{0: [v] + REST} for v in VALUES], literals, None,
                 [outside(v) for v in VALUES])


def check_vertex(optimize, tag):
    name = "vk_vertex_scalar_input.vert"
    blob, literals, _ = lowered(name, optimize)
    position = [0.25, -0.5, 0.5, 1.0]
    lanes = [{0: [float(n), 0.0, 0.0, 0.0]} for n in range(len(VALUES))]

    def fetched(n, fetch, index):
        return [[VALUES[int(index)], 0.0, 0.0, 1.0], position][n]

    wants = [[w * 0.5 + 0.125 * passes(w)] * 4 for w in VALUES]
    check_models(f"{name}: v_color, {tag}", blob, lanes, literals, {VERTEX_LOOP: MAX_PASSES}, wants,
                 vertex_fetch=fetched)
    check_models(f"{name}: gl_Position, {tag}", blob, lanes, literals, {VERTEX_LOOP: MAX_PASSES},
                 [position] * len(VALUES), export=OPOS, vertex_fetch=fetched)


def main():
    for optimize in (False, True):
        tag = f"optimiser {'on' if optimize else 'off'}"
        check_loop(optimize, tag)
        check_outside(optimize, tag)
        check_vertex(optimize, tag)
    print("OK")


if __name__ == "__main__":
    main()
