# a0 in a pixel shader's predicated arms

Console logs from 2026-10-17; what came of them is in docs/HARDWARE.md, "a0 and the predicate's execs".

- **`xsdk_test_hung.log`:** xsdk_test with xost at `3f35da3` (branch `a0-fixes`). Its raster test drew
  `vk_dynamic_if.frag`, compiled on the console, as a grid of 280 quads in two draws: 585 of 1120 quads wrong over four
  frames, then frame 5's fence didn't land, and every test after it failed to open the GPU.
- **`probe_a0_predicated.log`:** `tests/probe_a0_predicated.c` at `8461602`, built with
  `make build/probe_a0_predicated.xex`, run once. Its cases come from `tools/gen_a0_predicated_probe.py`. Each draws
  128 by 72 quads, with the arms' condition true, false, and true over the top half then false below. Each runs once
  with one set of indices for every quad and once with each quad's own. 26 cases, 240 draws: 64 PASS, 176 DIFFERS, no
  hang.

"Exec on the predicate" is `cond_exec_pred`. xost's encoding puts each arm in one and predicates each instruction too.
A quad "wrote nothing" when it held the marker its register starts with.

| Case | Shape | Result |
|---|---|---|
| 1 | control: plain reads in the arms, no a0 | right |
| 2 | control: a read at a0 after the setp, no arms | right |
| 3 | (p0) arm loads j and reads at a0 | taken: about 3% wrote nothing |
| 4 | (!p0) arm loads j and reads at a0 | right but 1 quad in the split draws |
| 5, 6 | an arm reads at the a0 loaded before the arms | taken: every quad wrong, about a third not drawn |
| 7 | (p0) arm loads j alone, read after the arms | taken: 2 to 3% wrote nothing; skipped: every quad wrong |
| 8 | (p0) arm loads j and reads, read after the arms | taken: right, 201 wrong with quads' own indices; skipped: all wrong |
| 9, 10 | as 7 and 8, a0 loaded again after the arms | right |
| 11 | both arms load, j and l, read after the arms | (p0): about 3.5% wrote nothing |
| 12 | vk_dynamic_if.frag's arms: (p0) loads j and reads, (!p0) reads at i | (p0) about 3%; (!p0) every quad wrong |
| 13 | as 12, (p0) reading at i, no load | every quad wrong |
| 14, 16 | as 12, (!p0) loading i or l | (p0) about 3%; (!p0) right |
| 15, 17 | as 12, (!p0) first; no read at a0 before the arms | as 12 |
| 18, 19 | vk_dynamic_if.frag without the reload (`d8206eb`) and with it (`3f35da3`) | as 12 |
| 20 | as 12, plain execs, each instruction predicated | right |
| 21 | as 12, execs on the predicate, no instruction predicated | (p0) right; (!p0) every quad wrong |
| 22 | as 3, a plain exec, each instruction predicated | right |
| 23 | as 3, an exec on the predicate, no instruction predicated | right |
| 24, 25 | as 12 and 3, the condition each quad's own | wrong in large numbers |
| 26 | as 19, 15 rounds | as 19 every round |
