# Milestone 46 step 7: loops through the layer

docs/HARDWARE.md, "Loops through the layer, milestone 46 step 7". The M46 branches of xsdk and vulkan4xenon built
clean in full on 2026-10-18, run on the console through the console agent: vulkan4xenon_test twice and its break
build, then xsdk_test, vkbench and Dusklight built against both branches, with Dusklight's pack rebuilt by the
branch's xost and the same but for the two fields each resource gained.

| Log | Build | Result |
|---|---|---|
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 487 of 487 each, the four loop scenes from the bundle and compiled on the console |
| `v4x_fixed_short.log` | the layer writing a loop whose count is data one pass short | 483 passed, 4 failed: the samples scene's colour checks, from the bundle and compiled here |
| `xsdk_test.log` | xsdk_test | 447 passed, 2 failed, the controller checks with no pad |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The break build comes from the xsdk branch's `local/m46/step7/make_patch.py` through `step7_breaks.sh`, which patches a
scratch copy of the vulkan4xenon branch's `src/queue.c`, `apply_loops`. `check_scenes.py` there ran the scenes' probed
pixels on xost's simulator in both its models before the console did.
