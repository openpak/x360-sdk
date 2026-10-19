# Milestone 47 step 3: instanced draws in xsdk

docs/HARDWARE.md, "Instanced draws in xsdk, milestone 47 step 3". The M47 branch built clean in full on 2026-10-18,
with vulkan4xenon's M47 branch, unchanged, built against it; run on the console through the console agent: xsdk_test
and its five break builds, then vulkan4xenon_test twice, vkbench and Dusklight, with Dusklight's pack the same.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 459 passed, 2 failed, the controller checks with no pad; every instancing check passed |
| `inst_pervtx.log` | instances fetched at the vertex | the three grids fail |
| `inst_vplusone.log` | 1/S taken for S + 1 | the auto-indexed and 16-bit grids and the points fail |
| `inst_nofirst.log` | pieces after the first without their first instance | the two-piece grid fails |
| `inst_nostep.log` | copies not moved on by S | the 16-bit and 32-bit grids fail |
| `inst_onestrip.log` | strips drawing their first instance alone | the strips fail |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 487 of 487 each |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The break builds come from the branch's `local/m47/step3/make_patches.py` and `breaks.sh`.

Xenia ran xsdk_test first as a smoke check: every instancing check passed there, and the same 46 checks failed as at
step 2.
