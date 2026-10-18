# Milestone 46: loops in shaders, to Shader Model 3's

Agreed on 2026-10-17, in this order: 44's first part, 46, 47, then 44's second part. The three were scoped by what
ports from the GameCube, the Wii and PC games of the same years hit first.

## Why

vulkan4xenon measures the layer against a typical Shader Model 3 card (vulkan4xenon:docs/PARITY.md). Its Loops row is
Partial, for xost alone: "fixed counts unrolled, an `if` on data inside them; counts in a block as hardware loops,
broken out of on a value every vertex or pixel shares; no break on data, `continue` or counted loop inside another".
The layer adds that a count past 255 runs 255 (vulkan4xenon:docs/SUPPORT.md, Shaders). SUPPORT.md also refuses, in a
loop a block counts, a store into part of an output, and a sample or derivative at coordinates a pass works out.

A Shader Model 3 shader may break out of a loop per pixel or vertex, nest loops 4 deep, and sample in a loop's pass.
Each gap is xost's: Xenos has the control flow, and xsdk sets loop constants already.

## The gap

Checked on 2026-10-17 by compiling small GLSL shaders on the PC with xost at xsdk's register split, each named below.

1. **Breaking out on data.** SM3's `break_comp` and `breakp` break per pixel or vertex, so pixels of one 64-wide
   vector leave at different passes.
   - **xost today:** refused. In an unrolled loop, any break inside an if/else on data
     (`src/xsc_unroll.c:885-890`, `vk_loop_if_break.frag`). In a hardware loop, any break but `if (c) break;` ending a
     pass (`hardware_break`, `src/xsc_unroll.c:913-926`), and that one only on a value every pixel shares
     (`emit_loop`, `src/xsc_lower_flow.c:584-587`, `vk_loop_break_data.frag`). `if (c) break;` at the top of a
     block-counted pass is refused too.
   - **A miscompile found while checking:** a break on a condition the unroller folds, `if (i == 2) break;` in a loop of
     4, compiles and runs every pass. The simulator gives 4 steps where the GLSL gives 3. `branch` walks the taken arm,
     which leaves the loop, and drops its `ended` (`src/xsc_unroll.c:942-948`). The same holds for `if (i == 1)
     continue;`. No shader in the corpus has either shape.
   - **Xenos:**
     - LOOP_END can break on the predicate, "if the predicate in all 64 invocations matches"
       (xenia:src/xenia/gpu/ucode.h:372-373), after the pass count is incremented (ucode.h:133-135).
     - An exec on the predicate runs for the whole vector if any invocation needs it, so the compiler predicates each
       instruction in it as well (ucode.h:114-124). Xenia's interpreter skips a predicated instruction where p0
       disagrees (xenia:src/xenia/gpu/shader_interpreter.cc:101-121).
     - `cnd*` selects per pixel. The predicate counter ops `setp_inv`, `setp_pop`, `setp_clr` and `setp_rstr`
       (ucode.h:1206-1247) are Xenos's means for nested per-pixel masks.
     - Xenia's translator breaks each invocation on its own p0 (xenia:src/xenia/gpu/spirv_shader_translator.cc:986-1000),
       so Xenia can't show the 64-wide rule.
     - On the console, a divergent predicate is untried: cases 24 and 25 of `probe_a0_predicated` mixed it with a0's
       failures (docs/HARDWARE.md, "a0 and the predicate's execs", "Still unknown").
2. **`continue`.** SM3's assembly has no `continue`: its compilers lower one into the rest of the pass. GLSL and HLSL
   have it, so this is parity at the source.
   - **xost today:** refused in a hardware loop (`src/xsc_unroll.c:1323-1327`, `block_continue.frag`). On data in an
     unrolled loop it's refused as a break is (`unrolled_data_continue.frag`). On a folded condition it's miscompiled,
     as above.
3. **Nested loops.** SM3 nests `loop` and `rep` 4 deep.
   - **Xenos:** LOOP_START nests up to 4 (ucode.h:131). Each level keeps its own count and aL
     (shader_interpreter.cc:131-184; aL is the iterator times the step plus the start, xenia:src/xenia/gpu/shader_interpreter.h:90-104).
   - **xost today:** constant counts nest unrolled (`loop_nested.frag`), and so does a constant loop inside a
     block-counted pass (`unrolled_in_block.frag`). These are refused (`src/xsc_unroll.c:1195-1196`;
     `src/xsc_lower_flow.c:581-583`):
     - a block-counted loop inside another loop, either kind (`block_nested.frag`, `block_in_unrolled.frag`);
     - one inside an if/else on data (`block_in_data_if.frag`).
   - **The program builder** holds one open loop (`loop_open`, `src/xsc_program.c:275-302`). The layout and the
     allocator already walk a list of loops (`control_flow`, `stretch_over_loops`, `src/xsc_program.c:454-509, 874-895`).
4. **Loop counts.** SM3's `loop` and `rep` take their count from an integer constant, 0 to 255. Xenos's loop constant
   counts in 8 bits, with aL's start and step in 8 bits each (xenia:src/xenia/gpu/xenos.h:1086-1097).
   - **On the console:** a count of 300 ran as 255 and -5 as none (docs/HARDWARE.md, "Branches, vulkan4xenon milestone
     21").
   - **So "a count past 255 runs 255" is parity:** one SM3 loop can't run more either. Against Vulkan it stays a
     deviation, recorded as now.
   - Constant counts unroll up to 4096 passes (`MAX_TRIP`, `src/xsc_unroll.c:12, 1280-1284`), past SM3.
5. **Loops whose count is data.** SM3 has no such loop either: a loop on data is a counted loop broken out of on
   data.
   - **xost today:** refuses each of these (`src/xsc_unroll.c:1034-1063`):
     - a count read from a float (`float_count.frag`) or an interpolant (`data_count.frag`);
     - `while (x < y)` (`while_data.frag`);
     - a block-counted loop that starts past 0 or steps other than `i++` (`src/xsc_unroll.c:1223-1224`).

One more refusal lies next to these. A pixel shader gathers its samples before its ALU work
(`src/xsc_lower_fragment.c:220-229`), so a sample or derivative at coordinates a pass works out is refused
(`src/xsc_lower.c:1406-1412`). SM3 samples in a loop's pass at a LOD or gradients it gives (`texldl`, `texldd`), and a
loop that leaves on data is where such samples sit: parallax occlusion mapping steps along a ray until it passes the
height map.

## The design

**One mechanism for every break and continue on data.** The masks are values carried through the pass and applied by
`cnd` selects, as an if/else on data already is (xost:docs/xsc.md, "Control flow"). Nothing in the pass is predicated
per pixel. LOOP_END's predicated break is used only to leave early, once no pixel is left.

- **In the unroller,** as it walks a pass (`src/xsc_unroll.c`):
  - **Two flags per loop:** `running`, 1 while the pixel runs this pass, and `alive`, 1 while it's still in the loop.
    Each is a 0.0 or 1.0 value, as xost holds bools (xost:docs/xsc.md, `&&` as `min`).
  - **At a break or continue on condition c:**
    - each value the loop carries keeps a stopped copy, `stopped = cnd(running && c, value now, stopped)`;
    - then `running = running && !c`;
    - a break also clears `alive` there.
  - **At the pass's end,** each carried value is `cnd(running, value at the end, stopped)`.
  - **A pixel that has left** starts each later pass with `running` 0, so its values never change again.
  - **The arms of an if/else on data** carry the flags as they carry locals, and the merge selects them.
  - **Unrolled loops** get the same values threaded from pass to pass, straight-line code that the optimiser and
    `xdis/equivalence.py` take as they are.
  - **A folded condition** ends the pass, or the loop, as the unroller walks, which fixes the miscompile.
- **In a hardware loop:**
  - `alive` is a register the loop carries, from 1.
  - At the pass's end a `setp_eq` on it sets p0 where the pixel has left, and LOOP_END breaks on p0
    (ucode.h:1162-1171, 372-373).
  - Whichever way the console takes that break, the values are right. A pixel that left keeps them in every later
    pass, and the loop constant bounds the passes. So the break only saves passes, and Xenia's per-invocation break
    gives the same values.
  - Today's `if (c) break;` ending a pass, on a value every pixel shares, keeps its lowering byte for byte.
  - `i`, read after a loop it can leave on data, is carried and gated like any other value.
- **Loops whose count is data** become hardware loops with a fixed count of 255, the most a loop constant counts and
  SM3's own:
  - The loop's condition is a break at the top of the pass, or at the end for `do`/`while`.
  - The counter is a float the loop carries from its start by its step, as xost holds integers.
  - **A new need,** `XSDK_XSC_FIXED_LOOP` (the loop constant in `reg`, the count in `regs`). A runtime writes it with
    `xsdk_gpu_loop_constant(255, 0, 1)`. vulkan4xenon's bundle carries it as a resource beside its two block-loop
    kinds, and the layer writes it with the others.
  - **Block-counted loops** that start past 0 or step other than `i++` take this path too.
  - **Loop constants** go one per distinct count rather than one per loop, from the stage's 16
    (`src/xsc_lower_flow.c:9-16`).
- **Nested loops:**
  - **To 4 hardware loops deep** (ucode.h:131); a fifth is refused.
  - **Each level keeps its own state:** the unroller keeps a stack of hardware loops, the lowering keeps its loop state
    and saved caches per level, and the builder keeps a stack of open loops.
  - **Two loops that open or close at one stage:** the outer LOOP_START comes first and the inner LOOP_END first.
  - **An inner loop's `alive` starts from the outer pass's `running`.**
  - **A hardware loop inside an if/else on data runs for every pixel,** as both arms do, merged by selects.
  - **An if/else on a uniform whose arm holds a loop becomes selects,** since a loop isn't an exec.
  - **A hardware loop inside an unrolled one is copied** once a pass, the copies sharing a loop constant.
- **Samples and derivatives in a pass** are lowered inside it, after their coordinates.
  - Their fetches are serialized and grouped within the pass, as elsewhere (xost:docs/xsc.md, "Fetch layout").
  - Selects run every pixel of a quad through every pass, so an implicit LOD reads what the pixel's neighbours worked out
    in the same pass. At pixels that left at different passes it mixes passes, which GLSL leaves undefined in
    non-uniform control flow.
- **a0 and aL:**
  - A read at a run-time index in a pass loads a0 in the pass, as it does now (`a0_set`,
    `src/xsc_lower_flow.c:609, 641-642`), at every level. Nothing relies on a0 across LOOP_START, LOOP_END or the back
    edge.
  - A gated pass has no exec on the predicate, so the rule that a0 doesn't reach into one doesn't arise
    (docs/HARDWARE.md, "a0 and the predicate's execs"). A uniform if/else in a pass keeps its rule: an arm using a0 goes
    in plain execs.
  - aL stays unread (xost:docs/xsc.md, "Loops a block counts").

**Not chosen:**
- **Predicating each instruction of the pass on `alive`.** It would save a select per carried value, but:
  - a divergent predicate is untried on the console;
  - p0 is the one predicate, which a uniform if/else in the pass also uses;
  - a vertex shader's execs on the predicate hung the GPU (docs/HARDWARE.md, "Branches, vulkan4xenon milestone 21").

  It can follow if the probe shows divergent predication works and the selects cost.
- **The predicate counter ops,** which need the predicated pass above.
- **Predicated jumps over the rest of a pass** (ucode.h:144): Xenia doesn't say how one takes a vector that disagrees
  (xost:docs/xsc.md, "No jumps").
- **Hardware loops for constant counts with a break,** for their early exit: constant counts unroll as now. The probe's
  timings say whether a later step is worth it.

**The risks:**
- **The predicated LOOP_END with a divergent predicate is unproven.** Xenia's comment comes from a sequencer
  specification, and nothing says whether a vector's unused slots or killed pixels hold the break back. None of these
  can make a value wrong under the design, only slow; the probe settles them.
- **A vertex shader's predicated LOOP_END has never run on the console.** Today's `if (c) break;` already emits one,
  and vulkan4xenon_test's vertex loop has no break. If the probe hangs it, vertex shaders' loops run their whole count,
  gated, with no predicated break.
- **Nested LOOP_START has never run on the console.** Nested loops of 255 passes each can run long: 255 to the fourth
  passes is a hang to the user. SM3 allows the same.
- **Cost:** a vector runs as many passes as its slowest pixel needs. Each pass adds a select per carried value at each
  break or continue and at its end, plus the flags and a `setp`. Registers grow by the flags and the stopped copies.
- **a0 across the loop's control flow** isn't known, and the design doesn't rely on it.

## What it takes

Each step is built, verified as below, and run on the console before the next.

1. **The probe, first and hang-safe.** `tests/probe_loop_break.c` draws pixel and vertex shaders that
   `tools/gen_loop_break_probe.py` builds by hand with xost's encoders, as `probe_a0_predicated` does.
   - Each case gets its own draws and fence, vertex cases last. A file on `game:` names the case to start from after a
     hang.
   - The grid is 128 by 72 quads of 10 pixels. Each pixel's break pass comes from its quad and its place in it, so
     pixels of one vector disagree.
   - The cases:
     - **A pass counter left ungated,** loop constant 255, LOOP_END breaking on each pixel's p0. Each pixel's count shows
       how many passes its vector ran: the last pixel's (all 64 agree), the first's, or its own. It also shows which
       pixels share a vector.
     - **The design's shape,** gated values, `alive` and the predicated LOOP_END: each pixel holds its own break pass.
     - **Early exit,** timed by the GPU's clock: every pixel leaving at pass 1, at 128 and never; half and half within
       every vector; and thin triangles that fill vectors in part.
     - **Nested loops** 2, 3 and 4 deep, a loop constant each and an ungated counter at each level, and an inner break
       that leaves the inner loop alone.
     - **a0 loaded before LOOP_START** and read in the pass, and loaded in a pass and read after LOOP_END.
     - **The same in a vertex shader:** first today's uniform `if (c) break;`, then a divergent break, then nesting.
     - **A sample in a pass,** at a LOD the pass gives and at the computed one.

   The answers go into docs/HARDWARE.md before anything is built on them. If LOOP_END doesn't break when all 64 agree,
   or hangs, loops leave by the gating alone and run their whole count. If nested loops misbehave, step 5 stops at what
   ran.
2. **Breaks and continues on data in unrolled loops, and the miscompile.** No new hardware behaviour.
   - **xost:** the gating in the unroller, and folded conditions taken as they fold.
   - **New testdata:** `vk_loop_if_break.frag` now compiled; a `continue` on data; and `if (i == 2) break;` and
     `if (i == 1) continue;`. Each runs on the simulator for pixels leaving at every pass, vertex and pixel shaders.
   - **xsdk_test:** a raster grid where pixels leave at different passes.
3. **Breaks and continues on data in hardware loops,** with the early exit. The simulator gains the group mode below.
   - **xsdk_test:** a grid whose pixels leave at different passes within each vector, each holding its own value.
   - **xsdk_test, timed:** the same loop timed with every pixel leaving at pass 1 against a count of 1.
4. **Loops whose count is data:** the fixed count, its need, xsdk_test setting it, and vulkan4xenon's bundle and layer.
   Testdata: a count from a float uniform, from an interpolant, and `while` and `do`/`while` on data.
5. **Nested loops,** to the depth step 1 proved, and a loop inside an if/else on data.
6. **Samples and derivatives in a pass,** at a LOD or gradients the pass gives and at the computed LOD. Testdata: a
   parallax occlusion loop, its step count from the view angle.
7. **vulkan4xenon:**
   - `test_loops.c` gets a scene for each of steps 2 to 6;
   - vulkan4xenon:docs/SUPPORT.md and PARITY.md, and xost:docs/xsc.md, say what xost now takes;
   - SUPPORT.md keeps the 255 deviation and adds that a loop on data stops at 255 passes.

## Verification

- **On the PC, every step:**
  - xost's tests;
  - the new shaders on `xdis/simulate.py` in both of its models;
  - `tools/check_optimize.py` over the corpus: Dusklight's GX shaders, xost's testdata and vulkan4xenon's shaders.
    Every shader that compiled before compiles byte for byte the same unless it uses what the step adds. Unrolled
    loops with breaks compare optimiser on against off; programs with hardware loops stay out of the optimiser
    (`can_optimize`, `src/xsc_optimize.c:1436-1441`).
- **The simulator** runs one pixel and breaks a hardware loop where that pixel's p0 says (`xdis/simulate.py:331-355`),
  as Xenia's translator does. It gains a group mode:
  - several pixels in lockstep;
  - an exec on the predicate runs if any pixel needs it, each instruction predicated per pixel (ucode.h:114-124);
  - a predicated LOOP_END breaks only when every pixel agrees (ucode.h:372-373);
  - a fifth nested loop is an error.

  Each loop test wants every pixel's GLSL result in both models. The design is built to hold in both.
- **On the console, every step,** through the console agent:
  - xsdk_test, whose compiler check compiles the new testdata on the console and compares it with the PC's;
  - vulkan4xenon_test twice;
  - vkbench;
  - Dusklight's title scene as a smoke run.

  A GPU hang ends a run, and the agent's reboot recovers it.
- **Break sets, each failing the checks it names:**
  - **gating off in hardware loops,** so a pixel that left takes later passes' values: step 3's grid fails on the
    console and in the group model, while the one-pixel model passes, which is why the group model exists;
  - **the predicated break dropped,** or its `setp` inverted: the timed check fails, the values still right;
  - **a `continue` lowered as a break:** the continue checks fail;
  - **a folded break ignored,** as today: step 2's `if (i == 2) break;` check fails;
  - **the fixed count's loop constant written as 0:** step 4's checks draw no passes;
  - **an inner and an outer loop's constants swapped:** step 5's counts fail;
  - **a sample in a pass gathered before the loop:** step 6's check fails.

## Done when

- docs/HARDWARE.md records step 1's answers from the console.
- xost compiles each of these, in vertex and pixel shaders:
  - a break or continue on data anywhere in a pass, in unrolled and hardware loops;
  - folded breaks and continues, now right;
  - hardware loops nested as deep as step 1 proved, to 4;
  - loops whose count is data;
  - samples and derivatives in a pass.

  Every testdata shader for these gives its GLSL result on the simulator in both models.
- On the console:
  - xsdk_test's loop grids have 0 pixels wrong over 59 frames;
  - the timed check shows the early exit, every pixel leaving at pass 1 under a count of 255 within 10% of a count
    of 1, or HARDWARE.md records why it can't;
  - xsdk_test, vulkan4xenon_test and vkbench pass in full;
  - Dusklight's title scene runs as before.
- The corpus compiles byte for byte as before but for shaders using what the milestone adds. Dusklight's pack has no
  loops, so it is unchanged.
- PARITY.md's Loops row reads Yes for xost and for the layer, its one deviation the 255 passes a loop runs, which is
  SM3's own. Its "Where the gaps lie" line on xost changes to match.

## Not in it

- **`discard` or `return` in a loop.** Today's refusal stays.
- **A store into part of an output in a hardware loop.** A local does it.
- **Hardware loops for constant counts with a break,** for the early exit; the probe's timings decide later.
- **Predicating the pass per pixel, the predicate counter ops, and predicated jumps** (not chosen, above).
- **Counts past 255 in one hardware loop,** such as two nested loops for one count, and SPIR-V's iteration hints that
  could bound a loop on data below 255.
- **The bare-uniform hardware loop,** `src/xsc_lower_loop.c`. It stays as it is for OpenGL's GLSL.
- **The optimiser on programs with hardware loops.**
- **Instancing and multisampling,** the next two milestones.

## What it pays off

- **Dusklight: nothing.** None of its 150 programs has a `for`, `while`, `break` or `continue`
  (dusklight-xenon:platforms/xenon/shaders, 2026-10-17).
  - Aurora's upstream WGSL loops over a channel's 8 lights with a `continue` on a mask bit
    (aurora-xenon:lib/gx/shader.cpp:859-860). The xenon generator unrolls them per configuration instead
    (aurora-xenon:lib/vulkan4xenon/glsl.cpp:268-281). The Melee port goes through the same generator.
- **vulkan4xenon's tests: nothing new.** 5 of its 72 test shaders loop: `loop_count.frag`, `loop_count.vert`,
  `loop_count_if.frag`, `loop_if.frag` and `loop_uniform_if.frag`. All are within today's support.
- **So the payoff is parity, not a current game,** and PARITY.md's "almost every ported shader meets one" isn't borne
  out by the ports there are. What a port of a PC game of the Direct3D 9 years would hit:
  - **parallax occlusion and relief mapping:** a step count worked out per pixel, a break once the ray passes the
    height, and samples at gradients in the pass. That needs steps 3, 4 and 6.
  - **light loops** that `continue` past a light's range or mask, and blur and shadow kernels whose tap count is a
    float uniform: steps 2 to 4.
  - **loops over a block's count,** such as skinning by a bone count, which work already unless nested.
- **The one fix a title could meet today** is the miscompile: a folded break or continue in an unrolled loop compiles
  silently wrong.
