#!/usr/bin/env python3
"""xsc's gl_FragCoord in a tiled pass's band (xsdk:docs/scope/44-tiling-and-msaa.md, step 2).

Under a tiled pass's window offset the pixel parameters give the row within the band, not the target's
(xsdk:docs/HARDWARE.md, "Tiling's registers and packets, milestone 44 step 1"). So gl_FragCoord.xy is the parameters'
|xy| plus the pixel constant xsdk keeps at 0.5 and 0.5 more than the band's top row (xsc/native.py,
FRAG_COORD_CONSTANT), which xsdk writes in the ring before each band runs the pass's recording.

- vk_frag_coord_rows.frag, the pixel's place as its colour, gives on the simulator each pixel's row in the target for
  bands from rows 0, 256 and 384, the constant as xsdk writes it for each. With the constant at 0 it gives the row in
  the band, the half pixel left out, so it reads the constant rather than a 0.5 of its own.
- vk_frag_coord_high.frag reads gl_FragCoord beside a block reaching c252, in the group of 4 xsdk writes whole for the
  constant, and is refused.
"""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tests"))

import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402
from xsc.lower import LowerError, lower_ex  # noqa: E402
from test_local_stores import check, module  # noqa: E402

WIDTH = 1280.0
HEIGHT = 720.0
COLOR = [0.25, 0.5, 0.75, 1.0]
BAND_TOPS = (0, 256, 384)
# A band's rows and the target's columns, from the first to the last.
ROWS = (0, 31, 255)
COLUMNS = (0, 640, 1279)


def run(blob, manifest, column, row, offset):
    constants = {r: list(v) for r, v in manifest}
    if offset is not None:
        constants[native.FRAG_COORD_CONSTANT] = offset
    # The interpolator in r0, the pixel parameters past it in r1.
    return simulate.run(blob, registers={0: COLOR, 1: [float(column), float(row), 0.0, 0.0]},
                        constants=constants).exports[0]


def main():
    blob, manifest = lower_ex(module("vk_frag_coord_rows.frag"), XSDK_REGISTERS)
    for top in BAND_TOPS:
        offset = [0.5, top + 0.5, 0.0, 0.0]
        for row in ROWS:
            for column in COLUMNS:
                want = [(column + 0.5) / WIDTH, (top + row + 0.5) / HEIGHT, COLOR[2], 1.0]
                check(f"vk_frag_coord_rows.frag, band from row {top}, its row {row}, column {column}",
                      run(blob, manifest, column, row, offset), want)
    check("vk_frag_coord_rows.frag with the constant at 0, band row 31, column 640",
          run(blob, manifest, 640, 31, None), [640 / WIDTH, 31 / HEIGHT, COLOR[2], 1.0])
    try:
        lower_ex(module("vk_frag_coord_high.frag"), XSDK_REGISTERS)
    except LowerError as error:
        print(f"  PASS vk_frag_coord_high.frag: refused ({error})")
        print("OK")
        return
    print("  FAIL vk_frag_coord_high.frag: lowered, its block in the group xsdk keeps for gl_FragCoord's constant")
    raise SystemExit("FAILED")


if __name__ == "__main__":
    main()
