# Milestone 46 step 4: loops whose count is data

docs/HARDWARE.md, "Loops whose count is data, milestone 46 step 4". The M46 branches of xsdk and vulkan4xenon built
clean in full on 2026-10-18, run on the console through the console agent: xsdk_test and its break build, then
vulkan4xenon_test twice and its break build, vkbench, and Dusklight built against both branches, with Dusklight's pack
rebuilt by the branch's xost and the same byte for byte.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 426 passed, 2 failed, the controller checks with no pad; data loop grid 0 of 755,200 wrong |
| `xsdk_fixed_zero.log` | xsdk_test writing the fixed loop constant as 0 | data loop grid 755,200 of 755,200 wrong |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 447 of 447 each |
| `v4x_fixed_zero.log` | the layer writing the fixed loop constant as 0 | 443 passed, 4 failed: the count-from-data scene's colour checks, from the bundle and compiled on the console |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The break builds come from the xsdk branch's `local/m46/step4/make_patch.py` and `step4_breaks.sh`, which patch scratch
copies of both branches (xsdk_test's `make_data`, the layer's `apply_loops`).

vulkan4xenon_test's first run on the console, before vulkan4xenon 836044a, failed the shader cache's count of shaders
compiled on the console, 8, as the new scene compiles a ninth; the count was fixed and both runs above are after it.

Xenia ran xsdk_test first as a smoke check: the data loop grid 0 wrong, and the same 45 checks failing as at step 3.
