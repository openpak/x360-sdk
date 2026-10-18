# Milestone 46 step 1: the loop probe on the console

docs/HARDWARE.md, "Loops on the console, milestone 46 step 1". `tests/probe_loop_break.c` built clean from the M46
branch on xsdk `7f03ce3`, run on the console on 2026-10-18 through the console agent. With no
`game:\loopbrk_start.txt` it ran the pixel cases in order; the start file named each vertex case, run alone after a
reboot.

| Log | Build | Cases | Result |
|---|---|---|---|
| `probe_loop_break.log` | probe_loop_break | 1 to 5, 7 | 19 PASS, case 1 held back; 7a, 7c, 7d, 7e DIFFER |
| `probe_loop_break_6a.log` | probe_loop_break | 6a | PASS, each vertex at its own pass |
| `probe_loop_break_6b.log` | probe_loop_break | 6b | PASS, the gated values; most vertices ran past their own pass |
| `probe_loop_break_6c.log` | probe_loop_break | 6c | PASS |
| `probe_loop_break_6d.log` | probe_loop_break | 6d | PASS |
| `probe_loop_break_nogate.log` | case 2's end-of-pass select a copy | 1 to 5, 7 | 2a, 2b DIFFER |
| `probe_loop_break_invert.log` | the break's condition inverted | 1 to 5, 7 | 1 FAILED, no model; 2a DIFFERS |
| `probe_loop_break_swap.log` | the nested loops' constants swapped | 1 to 5, 7 | 4a to 4d DIFFER |
| `probe_loop_break_hoist.log` | the sample taken before the loop | 1 to 5, 7 | 7b DIFFERS; 7a, 7e every pixel |
| `probe_loop_break_noa0.log` | a0 never loaded in 5a's pass | 1 to 5, 7 | 5a DIFFERS |

7a, 7c, 7d and 7e DIFFER in every build, as in the normal one: they sample at the level setTexLOD gave. The break
builds' vertex cases weren't run on the console; they fail on xost's simulator (6a and 6b inverted, 6b ungated, 6c and
6d swapped).

Xenia ran the probe and the break builds first as a smoke check: every pixel case passed there, and each break failed
the checks it names. Its clock reads 0, so case 3's times there are 0. A first build without 7c to 7e gave the same
results on the console, its vertex cases each from a fresh boot.
