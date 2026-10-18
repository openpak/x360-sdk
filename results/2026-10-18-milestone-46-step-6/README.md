# Milestone 46 step 6: samples in a loop's pass

docs/HARDWARE.md, "Samples in a loop's pass, milestone 46 step 6". The M46 branch built clean in full on 2026-10-18,
with vulkan4xenon's M46 branch, unchanged, built against it; run on the console through the console agent: xsdk_test
and its break build, then vulkan4xenon_test twice, vkbench and Dusklight, with Dusklight's pack rebuilt by the branch's
xost and the same but for the two fields each resource gained.

| Log | Build | Result |
|---|---|---|
| `xsdk_first_grid.log` | the first grid, `vk_loop_sample_grid_first.frag`, every wrong pixel of the first frames logged | grid 148,680 of 283,200 wrong |
| `xsdk_test.log` | xsdk_test | 447 passed, 2 failed, the controller checks with no pad; sample grid 0 of 283,200 wrong |
| `xsdk_s6_gathered.log` | samples in a pass gathered before the loop | sample grid 283,200 of 283,200 wrong; the seven shaders sampling in a pass compile apart |
| `vulkan4xenon_test_1.log`, `_2.log` | vulkan4xenon_test | 455 of 455 each |
| `vkbench.log` | vkbench | 14 of 14 |
| `dusklight.log` | Dusklight | its title scene at the frame rates before, its capture written |

The first grid sampled at gradients that differed within a 2x2 quad and took a mod by 3 and by 5 at whole multiples.
It drew the same wrong pixels in every frame of two runs, and `first_grid_model.py` gives every pixel of a frame as
the console drew it with two rules, a LOD from register gradients taken once a quad and rcp landing a hair low:
`python3 first_grid_model.py` prints how many pixels each model gets unlike the console. The grid xsdk_test now draws
keeps register gradients the same across a quad in the loop its pixels leave apart, and takes such mods of values half
past a whole.

The break build comes from the branch's `local/m46/step6/make_patch.py` through `rebuild_break.sh`.

Xenia ran xsdk_test first as a smoke check: the sample grid wrong in every pixel, in blue where logged, as CLAUDE.md
says, and the other 45 checks failing as at step 5.
