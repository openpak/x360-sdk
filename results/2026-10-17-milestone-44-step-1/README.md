# Milestone 44 step 1: the tiling probe on the console

docs/HARDWARE.md, "Tiling's registers and packets, milestone 44 step 1". `tests/probe_tiles.c` built clean from the
M44 branch on xsdk `3c74037`, run on the console on 2026-10-17 through the console agent. `game:\tiles_case.txt`
named each case from 5 on, and the console was rebooted before each of those.

| Log | Build | Cases | Result |
|---|---|---|---|
| `probe_tiles.log`, `probe_tiles_2.log` | probe_tiles | 1 to 4 | 9 PASS each; the second after case 10 and 9 |
| `probe_tiles_flip.log` | the offset's sign flipped | 1 to 4 | cases 1 and 2's 4 checks DIFFER, the other 5 PASS |
| `probe_tiles_nomove.log` | the resolve's base not moved | 1 to 4 | case 3's 3 checks DIFFER, the other 6 PASS |
| `probe_tiles_case9.log` | probe_tiles | 9 | PASS: a predicated draw lands before any bin packet |
| `probe_tiles_case5.log` | probe_tiles | 5 | 3 PASS: 0x50 and 0x51, high word first |
| `probe_tiles_case6.log` | probe_tiles | 6 | 3 PASS: low word first |
| `probe_tiles_case7.log` | probe_tiles | 7 | the fence never landed: 0x60 to 0x63 wedge the GPU |
| `probe_tiles_case8.log` | probe_tiles | 8 | 3 PASS: in a recording run from the ring |
| `probe_tiles_nopred.log` | no predicate bit | 5 | 3 DIFFER: every draw lands |
| `probe_tiles_case10.log` | probe_tiles | 10 | PASS, both registers left all ones at exit |
| `probe_tiles_case9_after_case10.log` | probe_tiles | 9 | PASS, run next without a reboot |

Xenia ran every case as a smoke check first, all passing, case 7 among them; its clock reads 0, so case 4's times
there are 0.
