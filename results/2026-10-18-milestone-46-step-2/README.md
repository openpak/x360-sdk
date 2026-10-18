# Milestone 46 step 2: breaks and continues on data in unrolled loops

docs/HARDWARE.md, "Breaks and continues on data in unrolled loops, milestone 46 step 2". The M46 branch built clean
in full on 2026-10-18, run on the console through the console agent: xsdk_test and its two break builds, then
vulkan4xenon_test twice, vkbench and Dusklight from a copy of vulkan4xenon built against the branch, with Dusklight's
pack rebuilt by the branch's xost and the same byte for byte.

| Log | Build | Result |
|---|---|---|
| `xsdk_test.log` | xsdk_test | 413 passed, 2 failed, the controller checks with no pad; loop grid 0 wrong |
| `xsdk_loop_folded.log` | a folded break ignored | loop grid 472,000 of 472,000 wrong; compiles apart (a) |
| `xsdk_loop_continue.log` | a continue lowered as a break | loop grid 202,960 of 472,000 wrong; compiles apart (a) |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 439 of 439 each |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

(a) vk_loop_grid.frag and vk_vertex_loop_break.vert, compiled on the console, aren't the PC's.

The break builds come from `local/m46/step2/step2_breaks.patch` through `step2_breaks.sh` (the hooks in
`src/xsc_unroll.c` behind `XSDK_XSC_BREAK_FOLDED` and `XSDK_XSC_BREAK_CONTINUE`, kept out of the library). On the PC's
simulator the folded break's compiler gets 22 of the grid's 100 places wrong; on the console every pixel came out wrong,
its alpha too, from that compiler built for the console. Not chased, as it's the break build alone: the shipped
compiler's console compiles match the PC's (the xsc checks in `xsdk_test.log`).

Xenia ran xsdk_test first as a smoke check: the loop grid 0 wrong, and the same 44 checks failing as main's, which
Xenia fails for what CLAUDE.md lists.
