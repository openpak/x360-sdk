# A fetch's result read in a later exec

docs/HARDWARE.md, "Slow fetches across execs". Three of Dusklight's pixel shaders drew a different picture in every
run of `tests/probe_shader_cost.c` at milestone 45's end (`results/2026-10-17-milestone-45/probe_together.log`), each
reading a fetch's result, unserialized, in the exec after the fetch.

## The probe runs

`probe_shader_cost_1.log` and `probe_shader_cost_2.log`: two runs on the console on 2026-10-17 of `probe_shader_cost`
built from xost with what fetches wrote tracked across execs, cleared only by a serialized instruction, from
Dusklight's pack:

```
make XSC_PACK=/mnt/z/dusklight-xenon/platforms/xenon/glsl_pack.c build/probe_shader_cost.xex
```

- **Pictures:** all 103 hash the same in both runs. `1510c05add37f2b3.frag`, `29fab4bb2516d607.frag` and
  `2eefbbd89e53921c.frag` hash `BE5FB0A3`, `5FB6B0A3` and `891EC12B`; the other 100 as at milestone 45's end.
- **Time:** 65,487.23 and 65,497.89 us for a draw of each pixel shader against 62,913 at milestone 45's end, 4.1%
  more; 13,355.46 and 13,386.18 us for a draw of each vertex shader against 13,349.
- **xost's rule as committed** compiles every program of the pack byte for byte as the build these runs used.

## Serialized instructions

Each program's microcode, compiled on the PC at xsdk's split with the optimiser on, before (xsdk `d75322c`) and after:

| | Programs | Changed | Serialized ALU | Serialized fetches |
|---|---|---|---|---|
| Dusklight's pixel shaders | 103 | 93 | 4 -> 100 | 5 -> 36 |
| Dusklight's vertex shaders | 47 | 47 | 0 -> 47 | 0 -> 0 |
| The corpus's pixel shaders | 249 | 162 | 9 -> 174 | 14 -> 58 |
| The corpus's vertex shaders | 123 | 123 | 0 -> 123 | 5 -> 5 |

The corpus is milestone 45's (`results/2026-10-17-milestone-45/README.md`), at vulkan4xenon's current shaders. No
program changes but in its exec sequences' serialize bits: its instructions, registers and control flow are as before.

`check_optimize.txt` is `tools/check_optimize.py` over that corpus with the rule, xsdk_test's `vk_slow_fetch.frag`
among it, run as milestone 45's README says with `--baseline ../results/2026-10-17-milestone-45/baseline.json`: 389
shaders, 0 differ. The simulator runs one
instruction at a time, so serializing changes nothing it computes.

## The branch as merged

On the console on 2026-10-17, built clean from the branch rebased on xsdk `012a002` (`local/fetchwait/final_build.sh`,
`v4x_build.sh` and `dusk_build.sh`, not in the repository):

- **`probe_shader_cost_ordered_1.log`, `_2.log`:** with fetch grouping putting a fetch that waits for the ALU's work
  first in its run. 62,917.31 and 63,045 us for a draw of each pixel shader, against milestone 45's 62,913; 13,360.70
  and 13,348 us for each vertex shader. All 103 pictures as `probe_shader_cost_1.log` and `_2.log`.
- **`xsdk_test.log`:** 390 passed, 2 failed, the controller checks with no pad connected. `vk_slow_fetch.frag`'s grid
  had 0 of 755,200 pixels wrong.
- **`xsdk_test_nofix.log`:** the same title with what fetches wrote reset at each exec, as `d75322c` did, its shader
  headers regenerated: 389 passed, 3 failed, the third the grid, 710,754 of 755,200 pixels wrong.
- **`vulkan4xenon_test_1.log`, `_2.log`, `vkbench.log`:** vulkan4xenon `6efda94` against the branch, its three
  generated shader sources compiled with `--optimize`: 426 of 426 twice, 14 of 14.
- **`dusklight.log`:** Dusklight with its pack rebuilt by the branch's xost, its title scene's 600th frame captured, at
  the frame rates milestone 45's end ran at.
