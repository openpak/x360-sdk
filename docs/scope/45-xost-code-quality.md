# Milestone 45: xost's code quality

Agreed on 2026-10-17.

## Why

Every shader a title draws through xsdk or vulkan4xenon is xost's microcode, so its instruction count and registers
set what a pixel or a vertex costs the GPU. Measured with xost's own disassembler (`xenos-toolchain/xdis`) over
Dusklight's 150 GX shaders and vkbench's 7, compiled at xsdk's register split with the optimiser on, as Dusklight's
pack is built:
- **6641 ALU instructions,** 4930 in the 47 vertex shaders (up to 245 in one) and 1687 in the 103 pixel shaders.
- **2344 of them, 35%, are copies:** a `max` of a value with itself. A component computed into a temporary (a `dp4`
  into `r3.x`) is copied into the vector being built (`max r2.x, r3, r3`). The optimiser propagates copies into their
  readers (`src/xsc_optimize.c`, `propagate_copies`), but not a computed value into its copy's place.
- **583 slots, 9%, co-issue a scalar operation** beside the vector one, though every ALU instruction has room for both
  (xenia:src/xenia/gpu/ucode.h:1854-1878).
- **Registers:** pixel shaders take 6.2 GPRs on average and up to 17, vertex shaders 9.8 and up to 15, each component
  computed in a temporary of its own. `SQ_PROGRAM_CNTL`'s register counts size every thread
  (xenia:src/xenia/gpu/registers.h:134-136), and the more a thread takes, the fewer run to hide fetch latency.
- **Constants built in ALU:** a position's w of 1 is made with a `max` from a literal, where a vertex fetch can give
  0 or 1 in any component itself (`FetchDestinationSwizzle`, xenia:src/xenia/gpu/ucode.h:677-692).
- **Fetches nothing uses:** 5 pixel shaders keep a texture fetch whose result never reaches an export.

A title that wants more from the GPU should get it from xost's code before it gives up pixels. Every step here keeps
the arithmetic as it is, so a program computes the same bits before and after; only where values live and how
instructions pair changes.

## What it takes

Each step is built, then verified as below, then the next begins.

1. **Measurement.** `tools/check_optimize.py`, which already compiles a folder of shaders with and without the
   optimiser and compares their exports on random inputs (`xdis/equivalence.py`), also counts copies, co-issued
   slots, fetches, control flow and GPRs, per shader and in total. The baseline for Dusklight's GX shaders, xost's
   testdata and vulkan4xenon's shaders goes in `results/`. A console probe, `probe_shader_cost`, draws a set of the
   pack's pixel shaders over 1280x720 and a vertex-heavy mesh with the heaviest vertex shaders, timed by the GPU's
   clock, so each step's gain is measured on the hardware as well as counted.
   Built on 2026-10-17: the counts and the baseline are in `results/2026-10-17-milestone-45/`, and the probe times
   every shader in the pack, hashing each pixel shader's picture, with its two baseline runs on the console beside
   them.
2. **Values computed into place.** Where a copy moves a computed component into another register's component, and
   nothing between the computation and the copy reads or writes either place, the computation writes there and the
   copy goes. This covers vectors built a component at a time and exports assembled from parts.
   Built and run on the console on 2026-10-17: the optimiser's `compute_in_place` takes the corpus from 8895 ALU
   instructions to 6887 and from 3253 copies to 1271, and Dusklight's pixel shaders take 15.0% less of the GPU, its
   vertex shaders 14.7%, recorded in `results/2026-10-17-milestone-45/`. It made compiling Dusklight's 150 programs on
   the console 2.6 times slower, 1.32 s to 3.39 s; agreed on 2026-10-17 to bring that back within this milestone.
3. **Registers packed.** The allocator gives values whose components don't overlap in time components of one
   register, rather than a register each.
   Built and run on the console on 2026-10-17: in the programs the optimiser rewrites, the allocator's
   `pack_registers` takes the corpus from 1973 registers to 1529, Dusklight's pixel shaders from 6.2 a shader to 4.5
   and its vertex shaders from 7.5 to 6.3, and its pixel shaders take 4.9% less of the GPU than at step 2, recorded in
   `results/2026-10-17-milestone-45/`.
4. **More co-issue.** With the copies gone, more scalar operations stand next to an independent vector one; the
   pairing takes the vector instruction before or after where it sits now when the order allows.
   Built and run on the console on 2026-10-17: `co_issue` already paired nearly every scalar op
   the optimiser saw, so it now pairs both ways and in the programs it left alone for reading constants at a run-time
   index. The corpus goes from 660 co-issued slots to 726 and from 6887 ALU instructions to 6821, short of the
   milestone's 6671; Dusklight's two relative vertex shaders lose 22 and 25 instructions and take a register more each,
   the rest of its pack is unchanged. Recorded in `results/2026-10-17-milestone-45/`.
5. **Constants from the fetch.** A vertex attribute's missing components that the shader fills with 0 or 1 come from
   the fetch's destination swizzle, not ALU.
   Built and run on the console on 2026-10-17: with the optimiser on, a vector built from a
   vertex input's own components and 0 or 1 past them is the input itself, its fetch giving the constants, which the
   runtime's patch keeps. The corpus goes from 6835 ALU instructions to 6525, 6502 without the two shaders testdata
   gained, 26.9% under the baseline and past the milestone's 6671; Dusklight's and vkbench's from 4911 to 4639, no
   fill of a fetched attribute left. Registers and co-issued slots stay. Recorded in
   `results/2026-10-17-milestone-45/`.
6. **Fetches nothing uses dropped,** with the exports' liveness the optimiser already tracks.
   Built and run on the console on 2026-10-17: `remove_dead` drops a vertex or texture fetch
   nothing live reads, and a vertex fetch's element goes with it; samplers, setters and getters stay. Over the corpus 4
   fetches go, in Dusklight's `6a05b1f88a016fb2.frag`, testdata's `cubedir.vert` and vulkan4xenon's
   `aurora_attr_defaults.vert`. The scope's other four pixel shaders use every sample, but take one sample two or three
   times, which liveness can't drop. 1 of Dusklight's 150 programs changes. Recorded in
   `results/2026-10-17-milestone-45/`.

## Verification, without the user

- **On the PC, every step:** xost's tests; `check_optimize.py` on every shader of the corpus, the step on against off,
  exports equal on random inputs; containers the console has passed come out byte for byte the same unless the step
  targets them; the counts against the baseline.
- **On the console, every step,** driven from the PC through the user's local console agent, which uploads, launches,
  fetches logs and reboots: xsdk_test, whose compiler check compiles shaders on the console and compares them with
  the PC's; vulkan4xenon_test, twice, its shaders compiled with the optimiser by the step's xost; `probe_shader_cost`
  against the baseline, its pictures of every program in Dusklight's pack compared pixel for pixel with the
  baseline's, as the arithmetic doesn't change; and Dusklight's title scene as a smoke run, its 600th frame captured
  and looked at. Break sets as before, each failing the checks it names.
- **Dusklight's frames,** agreed on 2026-10-17: two runs of one build capture the same 600th frame but for the sky,
  but a step that makes shaders cheaper reaches it at another point of the ride, so frames aren't compared across
  steps; the probe's pictures are the pixel check.
- **Recovery:** a GPU hang ends the run, and the agent's reboot recovers it, as for milestone 28's 13 hung tries. The
  user is needed only if the console stops answering the agent, or if xbWatson breaks into an assert, so it stays
  closed during the runs.

## Done when

- Every copy left is one no step can take: a constant, a fetched value into an export, or one in a shader the
  optimiser leaves alone, counted by kind (agreed on 2026-10-17, in place of under 5% of the ALU instructions, as
  copies of constants alone are 5.7% of Dusklight's and vkbench's after step 2). The corpus's ALU total is down by at
  least 25% from the baseline.
  Recorded on 2026-10-17, at steps 5 and 6 together (`results/2026-10-17-milestone-45/`):
  - **The ALU: met.** The 367 shaders the baseline compiled take 6502 ALU instructions with the optimiser on, 26.9%
    under the baseline's 8895 and 169 under 6671; Dusklight's and vkbench's 157 take 4639, 30.1% under 6641.
  - **The copies: not met as worded.** `tools/copy_kinds.py` sorts the 954 copies left in the 370 shaders that compile
    (`copy_kinds.txt`; README, "Copies left, by kind"), 450 of them Dusklight's and vkbench's, given below as corpus
    (Dusklight and vkbench). 771 (349) are the three kinds this line names. 146 (82) more stay for the hardware's
    reasons, though the line doesn't name them: inputs into exports, clamps, fetched values and inputs copied into a
    vector with other values, and computed values the hardware won't let a step move. 5 (0) step 2 leaves by a rule of
    its own, and 32 (19) are computed values step 2 should have taken.
    - *In a program the optimiser leaves alone,* 310 (137). 72 (0) are in hardware loops and 59 (0) in predicated code,
      control flow the milestone leaves as it is. 179 (137) are in the 10 programs that read a constant at a run-time
      index, which only `co_issue` works on, as the other passes don't model a0 (`can_optimize`); Dusklight's are in
      `04a469b1240c5698.vert`, `93d36ce55e3f9bad.vert` and `c0ddfd4547c71e5a.vert`. 97 of those 137 copy a computed
      value against which the tool finds none of step 2's reasons, were the passes to model a0.
    - *A constant into a temp,* 146 (67). 89 (43) are read in one operand beside a computed value, 42 of Dusklight's 43
      a computed vector's w of 1.0, as `vec4(mv_pos, 1.0)` fills it, and 33 (24) beside another constant: an operand
      reads one register (xenia:src/xenia/gpu/ucode.h:1893-1921). 21 (0) are read by a fetch, whose source is a
      register (ucode.h:709-714, 792-795); 2 (0) where the reader would read a third constant (`constants_fit`); and 1
      (0), testdata's `vk_frag_depth.frag`, by a kill, whose operands `propagate_copies` leaves as they are (`fixed`),
      as before the milestone.
    - *A constant into an export,* 158 (67): 65 (22) uniforms, 7 (1) other literals, 39 (35) literal 0s and 1s, and
      47 (9) an export's own 0 and 1 (`xsdk_xsc_emit_export_constants`). Only an ALU instruction writes an export, and
      nothing computes a constant. 84 (43) of them stand alone beside another instruction exporting the same register,
      which could write them as its own constant lanes (ucode.h:2051-2059), and 30 of Dusklight's write only lanes past
      their output's GLSL type, which no pixel shader reads; no step was scoped to merge or drop them.
    - *A fetched value into an export,* 157 (78): 111 (58) a vertex fetch's, 45 (20) a texture fetch's, and 1 (0) the 1
      a vertex fetch gives. A fetch writes a register, never an export (ucode.h:695-714, 782-796), so an ALU
      instruction exports what it fetched.
    - *An input into an export,* 28 (4), 16 (4) of them clamped and 2 (0) the point coordinate's absolute value.
      Interpolators and pixel parameters arrive in registers (xenia:src/xenia/gpu/registers.h:159-161), and only an
      ALU instruction exports.
    - *A computed value step 2 can't move,* 135 (89):
      - 43 (25) an elementwise result whose other lanes are read elsewhere. A vector op writes one register
        (ucode.h:1874), so the result can't both move to the copy's lanes and stay where its other readers read it.
      - 27 (25) a vertex shader's clip-space zw, computed for the position before the interpolators' alloc and exported
        again as an interpolator, and 25 (20) other exports whose writer's result is read elsewhere, Dusklight's all a
        pixel shader's alpha, which its alpha test reads too. An operand reads a temp or a constant, never an export
        (ucode.h:1893-1921), so a value both read and exported is computed into a temp and copied out.
      - 3 (0) read by another instruction in one operand with another writer's lanes, which the copy's place doesn't
        hold.
      - 5 (0) left by step 2's rule that only a vector op exports, and only after the export's own alloc, as xost's
        lowering always has: 4 of testdata's, a scalar op's result, which Xenia exports (ucode.h:2041-2060) but xost
        never has on the console; and vulkan4xenon's `shadow.frag`, computed before the colours' alloc.
      - **32 (19) step 2 should have taken.** 20 (19) gather several writers' results that another instruction reads
        together in one operand: in 19 of Dusklight's vertex shaders, the three `dp4`s of `vec4(mv_pos, 1.0)`, which
        the lighting reads as `mv_pos.xyz` too. 12 (0) are three copies of one `add`'s lanes into one vector, in
        vulkan4xenon's `loop_if.frag`. Looked at by hand in `0963325b37f04df0.vert`, `20a291c66ca7e0fd.vert` and
        `loop_if.frag`, nothing between the writers and the copies reads or writes their places, so the step's
        "vectors built a component at a time" covers them; but `can_place` weighs each writer alone (`can_redirect`:
        an operand reads one writer's result or none of it), and `compute_in_place` each copy. Not fixed here: the
        user's to decide.
    - *A clamp into a temp,* 10 (7): 3 (2) of a texture fetch's value, 3 (1) of an input and 2 (2) of a uniform, as only
      an ALU op clamps (ucode.h:1876), and 2 (2) of a computed value whose writer's result is read unclamped too. With
      the 17 in the programs read at a run-time index, Dusklight's and vkbench's `max_sat` copies into temps are 24,
      against the baseline's 342; 48 (33) copies are clamped in all.
    - *Anything else,* 10 (1): a fetched value, 8 (1), or an input, 2 (0), copied into a temp that's read in one
      operand beside a computed value or a constant. An operand reads one register, xost's fetches write theirs whole
      (README, step 3), and the inputs arrive where the hardware puts them.
- Co-issued slots and GPRs improve on the baseline, with the numbers recorded.
  **Met.** Over the 367 shaders the baseline compiled, optimiser on: co-issued slots 675 to 726, 7.6% of the ALU to
  11.2%, and registers 2115 to 1530 (-27.7%); Dusklight's and vkbench's 583 to 630 and 1117 to 784. On the console
  Dusklight's pixel shaders take 4.52 registers a shader for 6.19 and its vertex shaders 6.38 for 9.77, at most 16 for
  17 and 14 for 15. Its two vertex shaders read at a run-time index take a register more each than at the baseline,
  from step 4.
- Every shader's exports match with the optimiser on and off, and every console check above passes.
  **Met.** `check_optimize.py` found no exports differing at any step, the last (`steps5_6.txt`) with 15 shaders not
  simulated, for hardware loops, the cube instruction and predicates, and 16 testdata shaders xost refuses on purpose.
  On the console at every step, through the console agent: xsdk_test passed all but its two controller checks, which
  failed with 0x48F as no controller was connected, 380 checks and 383 from the a0 reload on; vulkan4xenon_test passed
  426, and 426 in each of two runs with its shaders compiled by the step's optimiser; `probe_shader_cost` drew the
  baseline's 100 pictures, the 3 that never repeat aside; `probe_xsc` compiled the pack's 150 programs as the PC does;
  and Dusklight's title scene ran, its frames not compared across steps (above). Each break set failed the checks it
  aims at (README). Dusklight's pack is built with the final xost in dusklight-xenon `8d1f8d67b4`.
- Compiling Dusklight's 150 programs on the console, as `probe_xsc` times it, takes about what it did before the
  milestone, 1.32 s.
  **Met.** 1.32 s at steps 5 and 6, as before the milestone; step 2 had made it 3.39 s, and taking every copy it can
  before the optimiser's round starts over brought it back (README, "Compile time").
- `probe_shader_cost` shows the gain on the console, recorded in docs/HARDWARE.md.
  **Met.** docs/HARDWARE.md, "xost's code quality, milestone 45": against each shader's better baseline run,
  Dusklight's pixel shaders take 19.0% less of the GPU and its vertex shaders 18.3%, 18.8% together.

## Not in it

- Changing the arithmetic: reassociating, refolding constants, or anything else that changes a result's bits.
- Control flow: loops, branches and predication stay as they are.
- The GLSL itself, such as Aurora's generator, and xsdk's or vulkan4xenon's CPU costs.
