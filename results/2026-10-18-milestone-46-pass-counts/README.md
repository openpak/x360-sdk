# Milestone 46: a block loop's passes from its start and step

docs/HARDWARE.md, "A block loop's passes from its start and step, milestone 46". The M46 branches of xsdk and
vulkan4xenon built clean in full on 2026-10-18, run on the console through the console agent: xsdk_test and its break
build, then vulkan4xenon_test twice and its break build, vkbench, and Dusklight built against both branches, with
Dusklight's pack rebuilt by the branch's xost and the same but for the two fields each resource gained.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 436 passed, 2 failed, the controller checks with no pad; step loop grid 0 of 283,200 wrong |
| `xsdk_plain_count.log` | xsdk_test writing the plain n | step loop grid 283,200 of 283,200 wrong |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 455 of 455 each |
| `v4x_plain_count.log` | the layer writing the plain n | 451 passed, 4 failed: the from-100-and-by-2 scene's colour checks, from the bundle and compiled on the console |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The break builds come from the xsdk branch's `local/m46/countfix/make_patch.py` and `countfix_breaks.sh`, which patch
scratch copies of both branches (xsdk_test's `set_loops`, the layer's `loop_count`).

Xenia ran xsdk_test first as a smoke check: the step loop grid 0 wrong, and the same 45 checks failing as at step 5.
