# Milestone 47 step 1: the instancing probe

docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1". `tests/probe_instancing.c` built clean from the
M47 branch, run on the console on 2026-10-18 through the console agent, every case in order, no start file.

| Log | Build | Result |
|---|---|---|
| `probe_instancing.log` | probe_instancing | 5 of 5 cases PASS |
| `probe_instancing_first.log` | the same, its batches recorded before the layer's way set its streams | case 4 DIFFERS: the layer's way drew no cell; the rest as above |
| `probe_instancing_nofloor.log` | the prologue without its floor | case 2 both halves, 13 of 14 counts, and the 12 instanced ways' drawn checks DIFFER |
| `probe_instancing_pervtx.log` | instances fetched at the vertex | case 2 both halves, 14 of 14 counts, and the 12 instanced ways' drawn checks DIFFER |
| `probe_instancing_nostep.log` | copies not moved on | case 2's indexed half, the 7 indexed counts, and the copies of 8, 64 and 512 DIFFER |

The break builds come from `make instancing_probes`, each a probe-local `-D` define in the Makefile.

Xenia ran the probe first as a smoke check: cases 1 to 3 passed there; its case 4 times say nothing of the console's.
