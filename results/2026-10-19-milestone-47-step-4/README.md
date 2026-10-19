# Milestone 47 step 4: instanced draws through the layer

docs/HARDWARE.md, "Instanced draws through the layer, milestone 47 step 4". The M47 branches of xsdk and vulkan4xenon
built clean in full on 2026-10-19, vulkan4xenon's bundles regenerated optimiser off with every vertex shader's
instanced variant; run on the console through the console agent: vulkan4xenon_test twice and its five break builds,
then xsdk_test, vkbench and Dusklight built against both branches, with Dusklight's pack the same.

| Log | Build | Result |
|---|---|---|
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 492 of 492 each |
| `v4x_nofirst.log` | `firstInstance` left out of the group | 485 passed, the 7 rows past instance 0 failed |
| `v4x_plain.log` | the plain variant where the shader lacks `gl_InstanceIndex` | 484 passed, 8 failed: its row, the 7-vertex list's, the variant's count, the shader cache's, and the indirect and strip rows its stretched triangles cover |
| `v4x_plain_first.log` | the same, before the test let its scene draw without the variant | 479 passed, 13 failed, the scene skipped |
| `v4x_firstindex.log` | copies from the index buffer's start | 489 passed, the 2 indexed rows and the image failed |
| `v4x_seven.log` | the 7-vertex list concatenated whole | 490 passed, its row and the image failed |
| `v4x_perinst.log` | a draw an instance for lists | 491 passed, the 1,000 instances' dwords failed |
| `xsdk_test.log` | xsdk_test | 459 passed, 2 failed, the controller checks with no pad |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The break builds come from the xsdk branch's `local/m47/step4/make_patches.py` and `breaks.sh`, which patch a scratch
copy of the vulkan4xenon branch.
