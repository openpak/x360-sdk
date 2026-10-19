# Milestone 47 step 5: instancing measured

docs/HARDWARE.md, "Instancing measured, milestone 47 step 5". The M47 branches of xsdk and vulkan4xenon built clean
in full on 2026-10-19; run on the console through the console agent: the probe's case 6 alone from its start file,
vkbench, then xsdk_test, vulkan4xenon_test twice and Dusklight built against both branches, with Dusklight's pack the
same.

| Log | Build | Result |
|---|---|---|
| `probe_instancing_case6.log` | probe_instancing, `game:\instancing_start.txt` holding 6 | the cube at 0.25 of the layer's GPU time an instance and 0.015 of its CPU, the quad at 0.09 and 0.0006: MEETS both |
| `vkbench.log` | vkbench | 17 of 17; `instances_4000` drew `draws_4000`'s picture through both backends |
| `xsdk_test.log` | xsdk_test | 459 passed, 2 failed, the controller checks with no pad |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 492 of 492 each |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |
