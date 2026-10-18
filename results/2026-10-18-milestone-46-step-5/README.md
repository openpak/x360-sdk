# Milestone 46 step 5: nested loops

docs/HARDWARE.md, "Nested loops, milestone 46 step 5". The M46 branch built clean in full on 2026-10-18, with
vulkan4xenon's M46 branch, unchanged, built against it; run on the console through the console agent: xsdk_test and its
break build, then vulkan4xenon_test twice, vkbench and Dusklight, with Dusklight's pack rebuilt by the branch's xost and
the same byte for byte.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 434 passed, 2 failed, the controller checks with no pad; nested loop grid 0 of 1,132,800 wrong |
| `xsdk_nest_swap.log` | the outer and inner loop constants swapped | nested loop grid 996,864 of 1,132,800 wrong |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 447 of 447 each |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The break build comes from the branch's `local/m46/step5/make_patch.py` and `step5_breaks.sh`, which swap the counts
xsdk_test writes into the grid's two block-counted loops' constants while its CPU reference keeps them.

Xenia ran xsdk_test first as a smoke check: the nested loop grid 0 wrong, and the same 45 checks failing as at step 4.
