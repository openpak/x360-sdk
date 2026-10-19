# Milestone 47 step 2: instanced vertex shaders in xost

docs/HARDWARE.md, "Instanced vertex shaders in xost, milestone 47 step 2". The M47 branches of xsdk and vulkan4xenon
built clean in full on 2026-10-18, vulkan4xenon's three bundles regenerated with the branch's xost, optimiser off as
they're committed, only `instanced.vert` changing; run on the console through the console agent: xsdk_test, then
vulkan4xenon_test twice, vkbench and Dusklight built against both branches, with Dusklight's pack rebuilt by the
branch's xost and the same.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 447 passed, 2 failed, the controller checks with no pad |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 487 of 487 each, the 9 instancing checks among them |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

Step 2 changes xost alone, so its break, the prologue without its floor, is checked off the console: the branch's
`local/m47/step2/nofloor_break.sh` builds that xost and runs `tests/test_instancing.py` against it, which fails the
simulated draws of all four instanced shaders and the probe's worst counts.

Xenia ran xsdk_test first as a smoke check: 339 passed and 46 failed, as at milestone 46's step 6.
