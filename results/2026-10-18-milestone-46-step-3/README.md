# Milestone 46 step 3: breaks and continues on data in hardware loops

docs/HARDWARE.md, "Breaks and continues on data in hardware loops, milestone 46 step 3". The M46 branch built clean in
full on 2026-10-18, run on the console through the console agent: xsdk_test and its two break builds, then
vulkan4xenon_test twice, vkbench and Dusklight from a copy of vulkan4xenon built against the branch, with Dusklight's
pack rebuilt by the branch's xost and the same byte for byte.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 419 passed, 2 failed, the controller checks with no pad; hardware loop grid 0 wrong, early exit 1,444,291 clocks under 255 against 1,444,310 under 1 |
| `xsdk_loop_gating.log` | gating off in the hardware loop | grid 554,718 of 660,800 wrong; compiles apart (a) |
| `xsdk_loop_nobreak.log` | the predicated LOOP_END dropped | grid 0 wrong; early exit 226,244,322 clocks against 1,444,168; compiles apart (a) |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 439 of 439 each |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

(a) vk_loop_count_grid.frag, vk_loop_break_data.frag, vk_loop_count_stop.frag and vk_vertex_loop_count_break.vert,
compiled on the console, aren't the PC's.

The break builds come from `local/m46/step3/step3_breaks.patch` through `step3_breaks.sh` (the hooks in
`src/xsc_unroll.c` and `src/xsc_lower_flow.c` behind `XSDK_XSC_BREAK_GATING` and `XSDK_XSC_BREAK_NOBREAK`, kept out of
the library). On the PC, the gating break fails only in the simulator's group model, which runs a group of pixels in
lockstep as the console does; each pixel alone, as Xenia's translator runs it, it passes.

Xenia ran xsdk_test first as a smoke check: the hardware loop grid 0 wrong, and main's 44 failing checks plus the timed
one, as Xenia's GPU clock doesn't count.
