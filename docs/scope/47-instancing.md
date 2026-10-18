# Milestone 47: instancing

Agreed on 2026-10-17, in this order: 44's first part, 46, 47, then 44's second part.

One of the three agreed to be scoped with the user, loops, instancing and multisampling, by what ports from the
GameCube and Wii and PC games of the same years meet first. It's parity work (CODE_GUIDELINES.md 1.2, the exception
agreed on 2026-10-15): nothing running on xsdk draws instanced today, so a test exercises all of it on the console.

## Why

A Shader Model 3 card draws many copies of a mesh in one call, each copy with data of its own: foliage, crowds,
particles, debris. Vulkan 1.0 does too. xsdk has no instanced draw, so a title draws an instance at a time, and
vulkan4xenon does the same for `instanceCount` (vulkan4xenon:docs/SUPPORT.md, Commands; docs/ROADMAP.md, milestone 24).
docs/PARITY.md's "Instancing" row is Partial for xsdk and xost, and "a draw an instance" for the layer.

**What an instance costs now.** For each instance the layer writes (vulkan4xenon:src/queue.c, `draw_instance` and
`draw_indexed_instance`):
- a group of four float constants holding `gl_InstanceIndex`, if the shader reads it;
- for each stream at the instance's rate, its fetch constant group and a fetch constant flush, 11 dwords
  (src/gpu_draw.c, `xsdk_gpu_set_vertices`);
- a draw packet.

Constant writes between draws hold each draw until the one before it has finished: a cube's draw alone costs the GPU
0.118 us, its two constant groups alone 0.112, and the two together 0.285 (docs/HARDWARE.md, "The GPU's cost of a
draw, milestone 39"; TODO.md, "Constants written between draws hold the GPU"). The layer wrote that cube, a group and a
draw, in 0.29 us of CPU time (docs/HARDWARE.md, "The cost of a draw, milestone 38"). An instance costs at least as
much; exactly how much is unmeasured (vulkan4xenon:docs/ROADMAP.md, milestone 24). At that rate, 10,000 small
instances take about 3 ms of the GPU and 3 ms of the CPU, a fifth of a 60 Hz frame, where their vertices alone would
cost a fraction of it: past a draw's fixed cost, each index costs the GPU about one clock (one triangle's draw 0.048
us against the cube's 0.118, 33 indices apart; the same section).

**What Xenos has.** No instance rate:
- A vertex fetch constant is an address, an endian swap and a size (xenia:src/xenia/gpu/xenos.h:1113-1127), and the
  draw initiator counts vertices (xenia:src/xenia/gpu/registers.h:311-331). Neither libxenon nor Xenia instances
  anything; libxenon's shader "instances" are copies for other vertex formats (libxenon:drivers/xenos/xe.h:97-107).
- But a vertex fetch takes its index from any register's component, floors it, or rounds it if asked, times its
  stride in dwords, plus its offset (xenia:src/xenia/gpu/ucode.h:695-780; shader_interpreter.cc:1007-1015).
- The vertex shader starts with the vertex's index as a float in r0.x: the index, plus `VGT_INDX_OFFSET`, masked to
  24 bits and clamped (xenia:src/xenia/gpu/dxbc_shader_translator.cc:346-374, 376-458).

So the instance is shader work, as docs/PARITY.md's row already says.

**What Direct3D 9 offered.** `SetStreamSourceFreq` (Direct3D 9's documentation, "Efficiently Drawing Multiple Instances
of Geometry"):
- the geometry's stream drew `D3DSTREAMSOURCE_INDEXEDDATA` with the instance count, and each stream of per-instance
  data `D3DSTREAMSOURCE_INSTANCEDATA` with a divisor, nearly always 1;
- only through `DrawIndexedPrimitive`, on `vs_3_0` hardware;
- the shader got no instance index: that's Direct3D 10's `SV_InstanceID`.

Cards without `vs_3_0` batched instances through constants, each vertex carrying its instance's slot. xsdk already does
that: a block array read at a run-time index takes a0 (xost:docs/xsc.md, "Interface conventions").

## How instancing can work on Xenos

1. **A draw an instance,** today's way. Simple and correct, and every instance pays a draw packet and a constant write
   that waits out the draw before it (above). It stays for what the ways below can't take: strips and fans, and
   instances of more than 65,535 vertices.
2. **One draw of N instances of V vertices, auto-indexed.** The vertex shader works out its instance from r0.x, the
   index i: instance k = floor((i + 0.5) / V), vertex j = i - kV. It fetches attributes at the vertex's rate at j, and
   at the instance's rate at k plus the first instance. One packet, one constant group, any number of instances.
3. **Indexed meshes.** Three ways to give the shader both the instance and the mesh's index:
   - **The index buffer repeated, each copy's indices moved on by S,** S a power of two above the mesh's largest
     index. The shader divides by S as way 2 divides by V, so it's the same shader. The copies cost memory, and the
     layer, which reads the app's indices from write-combined memory, CPU time (vulkan4xenon:docs/SUPPORT.md, Memory
     and buffers: reading it back is slow).
   - **A packet an instance over the same indices, `VGT_INDX_OFFSET` moved on by S each time.** Xenia adds the offset
     to auto-indexed draws' indices as well (xenia:src/xenia/gpu/xenos.h:77-81, spirv_shader_translator.cc:1500-1508).
     But it's still a packet an instance, and a register write between draws: with `COHER_DEST_BASE_1`, it was one of
     the two writes that cost each draw 0.2 us before milestone 39 (docs/HARDWARE.md, "The GPU's cost of a draw,
     milestone 39"). Which of the two held the draw wasn't separated.
   - **The shader fetching the mesh's index itself,** the index buffer bound as a vertex stream. No copies, but every
     corner is shaded, with no reuse between triangles; a second fetch waits on the first; a 16-bit index has to be
     picked out of its word; and a pipeline drawn both indexed and not needs two shaders.

   **Chosen: the repeated index buffer,** in copies of a few instances, K, each piece of the draw K instances. The
   shader is way 2's, the triangles keep their shared vertices, and a small K keeps the copying small. The probe
   (step 1) times K against a piece's cost to choose it.
4. **Past 65,535 vertices.** A draw packet counts in 16 bits, auto-indexed or from memory
   (`VGT_DRAW_INITIATOR`'s `num_indices`, xenia:src/xenia/gpu/registers.h:327). 32-bit indices don't lift that: they
   lift the index values, to 24 bits (xenos.h:38-42); `VGT_DMA_SIZE`'s 24-bit count is the index fetch's
   (registers.h:304), not the draw's. Several packets lift it. A draw goes in pieces of whole instances under 65,535
   vertices or indices, the constant group written before each with the piece's first instance, so each piece's index
   starts again at 0. Carrying the index on with `VGT_INDX_OFFSET` would do as well, but would put an unprobed
   register write between draws where a constant write, which takes effect on the next draw on the console, is needed
   anyway (docs/HARDWARE.md, "Instances and long strips, vulkan4xenon milestone 24").
5. **The division's precision.** Xenos's ALU has float instructions alone (xenia:src/xenia/gpu/ucode.h:1006, 1359), so
   it divides by multiplying by 1/V, which the CPU works out.
   - Auto-indexed, (i + 0.5) / V lies at least 0.5/V from a whole number. Within a piece i < 65,536, so the instance
     is below 65,536/V, and three roundings of 2^-23 each, an ALU that truncates rather than rounds, are an error
     under 0.03/V: a sixteenth of the room.
   - Indexed, S is a power of two, so the multiply is exact for every 24-bit index and needs no + 0.5.
   - The vertex within the instance, i - kV, is whole numbers under 2^24, exact in a float.

   Both are checked: on the PC for every V, and on the console at the counts that leave the least room (steps 2 and
   3).
6. **The ALU it costs.** Four instructions a vertex, a multiply-add, a floor, a multiply-add and an add (`kMad`,
   `kFloor`, xenia:src/xenia/gpu/ucode.h:1446, 1457; `kFloors` in the scalar pipe, 1077), some of which co-issue, and
   one register's x and y. Against the 105 ALU instructions of Dusklight's average vertex shader before milestone 45,
   that's about 4%, somewhat more after it, and only shaders compiled for instancing pay it.

## The design

**The interface the runtime fills** is one float constant register, x to w: 1/S, the + 0.5/S (0 for a power of
two), S, and the piece's first instance. S is V for an auto-indexed draw and the copies' step for a repeated
index buffer. Per-instance strides need no constant: a stream at the instance's rate is fetched with its own stride,
as one at the vertex's rate is, through the vertex-fetch patch.

### xost

- **An option, `instanced`** (`XsdkXscOptions`, `xsc/native.py`), for vertex shaders. A vertex shader that reads
  `gl_InstanceIndex` is lowered instanced whatever the option says.
- **The group** takes the register `gl_InstanceIndex` takes now, past the blocks and before the literals, so no block
  the GPU loads shares its group of four (xost:docs/xsc.md, "Interface conventions"). The resource the compiler lists
  becomes `XSDK_XSC_INSTANCE`, in place of `XSDK_XSC_INSTANCE_INDEX`, and the container records the register, which
  also marks the shader instanced.
- **The prologue,** in an exec of ALU instructions alone before the fetches, so each fetch reads a register an
  earlier exec wrote, and the fetches' own exec starts with its fetches, the order whose vertex-cache bits xost
  already sets right (docs/HARDWARE.md, "A fifth vertex fetch"; "Serialized fetches"):
  `k = floor(r0.x * c.x + c.y)`, `t.x = r0.x - c.z * k`, `t.y = k + c.w`.
- **Every vertex fetch reads t.x.** The runtime's patch points a fetch at the instance's rate at t.y (below).
  `gl_InstanceIndex` is t.y, `firstInstance` included, read as xost's integers are, as now.
- **A shader compiled without it is unchanged,** byte for byte.

### xsdk

- **`XsdkVertexElement` gains `per_instance`.** `xsdk_shader_set_vertex_layout` points such an element's fetch at the
  instance, the fetch's source swizzle (xenia:src/xenia/gpu/ucode.h:758), with its stream's stride, and refuses one
  for a shader that isn't instanced. Callers that leave it 0 are unchanged.
- **`xsdk_gpu_set_shaders` keeps the loaded vertex shader's group register.**
- **`xsdk_gpu_draw_instanced(primitive_type, vertex_count, first_instance, instance_count)`:** lists in pieces of whole
  instances up to 65,535 vertices, each piece's group written first; strips and fans an instance a piece, each with
  its own group. `vertex_count` is a whole number of primitives, up to 65,535.
- **`xsdk_gpu_repeat_indices` and `XsdkRepeatedIndices`:** writes copies of a mesh's indices into memory the caller
  gives (`xsdk_gpu_repeated_index_bytes` sizes it), each moved on by S, the power of two above the largest index,
  16-bit while the values fit and 32-bit past them, as many copies as one packet and 24-bit values hold, or fewer if
  asked. A title makes them once, as it makes an index buffer.
- **`xsdk_gpu_draw_indexed_instanced(primitive_type, repeated, first_instance, instance_count)`:** pieces of as many
  instances as there are copies.

### vulkan4xenon

- **A pipeline is instanced** if a binding is at the instance's rate or its vertex shader reads `gl_InstanceIndex`. It
  takes its module's instanced variant: from the bundle, where `tools/build_shaders.py` builds both variants of every
  vertex shader, or compiled on the console when the bundle lacks it, and kept as other shaders compiled there are,
  keyed with the option (vulkan4xenon:src/shader.c). Its attributes at the instance's rate are `per_instance`, and
  their streams are no longer moved for each instance. Pipelines that aren't instanced are drawn as now.
- **`vkCmdDraw`** of a list through `xsdk_gpu_draw_instanced`, the vertex count taken down to whole primitives, as
  Vulkan draws no partial one.
- **`vkCmdDrawIndexed`** of a list: the app's indices from `firstIndex` repeated into K copies, in GPU memory the queue
  reuses once a submit's fence has passed, then `xsdk_gpu_draw_indexed_instanced`.
- **Strips, fans and instances past 65,535 vertices** draw an instance at a time, the group written for each, in the
  pieces they take now. `firstVertex` and `vertexOffset` move the streams at the vertex's rate, as now. Indirect draws
  take the same paths with what they read at replay.
- **The bundle's `V4X_RESOURCE_INSTANCE_INDEX`** becomes the group, and the bundle's layout version goes up.

## What it takes

Each step is built, verified as below, then the next begins.

1. **The probe,** before anything else: `tests/probe_instancing.c`, its shaders packed by
   `tools/gen_instancing_probe.py` with xost's encoders, which stops unless xost's simulator gives each case the
   exports the probe expects, as `tools/gen_relative_constant_probe.py` does. Least risky case first, each logging
   its name before it runs, so a hang names it:
   1. **A fetch at a register an ALU exec wrote:** r0.x copied into another register, and a quad's position and colour
      fetched there.
   2. **The prologue,** as xost will lower it: 64 instances of a 6-vertex quad, each instance's cell and colour from a
      stream at the instance's rate; then the same from 64 repeated copies of a 4-vertex quad's 6 indices.
   3. **The division on the console's ALU:** points, one a vertex, at the vertex counts that leave it the least room
      (3, 7, 255, 257, 4,369, 21,845 and 65,535, and powers of two for the indexed form), each point placed and
      coloured from both streams, every one compared with where the CPU says it lands.
   4. **The cost,** in GPU time from kick to fence over batches of 500 and 2,500, as `probe_gpu_draw_cost` times it,
      and in CPU clocks an instance recorded, as `probe_draw_cost` does: vkbench's 4-pixel cube and a 6-vertex quad,
      drawn as the layer draws an instance now, as a draw an instance with only the group between, and in one draw,
      indexed in copies of 1, 8, 64 and 512.
   5. **Reading write-combined memory:** 64 KB from `xsdk_gpu_alloc` memory with 32-bit and 128-bit loads, in MB/s,
      what the layer pays to read an app's indices.

   The answers go into docs/HARDWARE.md. If cases 1 or 2 fail or hang, the milestone stops here and goes back to the
   user, as the design rests on them.
2. **xost.** The option, the group, the prologue and the resource. `tests/test_instancing.py` takes over
   `tests/test_instance_index.py`'s cases:
   - shaders with `gl_InstanceIndex`, with attributes at the instance's rate alone, and with both, run on the
     simulator for every vertex of N instances of V, the fetch callback giving each stream's element at the index the
     fetch read, each vertex's exports checked;
   - the division as float32 works it out, for every V from 1 to 65,535 at each instance's first and last vertex in a
     piece, the two closest to a wrong floor;
   - XenosRecomp's oracle accepting each instanced shader.
3. **xsdk.** The element flag and its patch, the group register, the draws and the repeated indices. New checks in
   xsdk_test, each within a second:
   - **Instances:** 12,000 instances of a 6-vertex quad, 72,000 vertices in two pieces, from instance 100 of their
     stream, each an 8 by 8 cell of 1280 by 720, placed by a float2 and coloured by a ubyte4n at the instance's rate,
     blue from `gl_InstanceIndex`; every cell's centre checked.
   - **Indexed instances:** the same grid from a 4-vertex quad's 6 indices in copies of 64, 16-bit; and again from a
     quad at vertices 69,996 to 69,999 of a stream, so its copies are 32-bit.
   - **Strips:** 50 instances of a triangle strip, an instance a piece.
   - **The division:** probe case 3's points at three of its counts.
4. **vulkan4xenon.** The instanced pipelines, their variants in the bundle and on the console, and the draws.
   vulkan4xenon_test's `test_instancing.c` keeps its five rows, `instanced_places.vert` among them, which has no
   `gl_InstanceIndex`, and adds:
   - a triangle list of 7 vertices an instance, whose 7th Vulkan never draws;
   - a triangle strip instanced;
   - a draw of 1,000 instances that adds fewer than 200 dwords to the stream (`xsdk_gpu_stats`), which a draw an
     instance can't.
5. **Measured.** The probe's cost cases again with the milestone's code; and vkbench's `instances_4000`, through both
   backends: `draws_4000`'s cubes, each cube's matrix and tint at the instance's rate, one indexed draw, its picture
   `draws_4000`'s.

## Verification, without the user

- **On the PC, every step:** xost's tests; the corpus (Dusklight's 150 programs, xost's testdata, vulkan4xenon's
  shaders) compiled byte for byte as before, but for shaders that read `gl_InstanceIndex`; `check_optimize.py` finding
  instanced shaders' exports equal with the optimiser on and off.
- **On the console, every step,** through the user's local console agent, as milestone 45 ran: the step's probe or
  checks; xsdk_test, vulkan4xenon_test twice and vkbench in full; Dusklight's title scene as a smoke run, as the
  patch and the shader loader change under it. Xenia only as a crash and smoke check, with the GPU flags CLAUDE.md
  gives.
- **Break sets,** each failing the checks it names and only those:
  - xsdk: elements at the instance's rate fetched at the vertex, failing both instance grids; the group's 1/V taken
    for V + 1, failing the grids and the division; pieces after the first without their first instance, failing the
    two-piece grid and not the one-piece ones; copies not moved on by S, failing the indexed grids alone; strips
    drawing their first instance only, failing the strips' check alone.
  - vulkan4xenon: `firstInstance` left out of the group, failing the rows that start past instance 0; the plain
    variant where the shader lacks `gl_InstanceIndex`, failing `instanced_places.vert`'s row; copies made from the
    index buffer's start, ignoring `firstIndex`, failing the indexed rows; the 7-vertex list concatenated whole,
    failing its check; and a draw an instance kept for lists, failing the 1,000-instance check alone.
  - xost: the prologue without its floor, failing `test_instancing.py`.
- **Recovery:** a GPU hang ends the run, and the agent's reboot recovers it. The user is needed only if the console
  stops answering the agent.

## Done when

- **The probe** has run on the console and docs/HARDWARE.md records its answers: fetches at a computed index, the
  division at every count it tried, the costs, and the read speed.
- **Cheaper on the console:** one instanced draw of the 4-pixel cube costs the GPU at most half, and of the 6-vertex
  quad at most a fifth, of an instance drawn as the layer draws one before this milestone; and the CPU, an instance,
  at most a tenth.
- **xost's tests pass,** the division sweep and the instanced shaders' simulations among them, and every corpus shader
  that doesn't read `gl_InstanceIndex` compiles byte for byte as before.
- **xsdk_test passes in full** on the console with the checks above, and vulkan4xenon_test twice, each break set
  failing what it names.
- **vkbench passes** through both backends, `instances_4000` drawing `draws_4000`'s picture, with less draw-and-submit
  time and an earlier fence than `draws_4000` through each, recorded in docs/HARDWARE.md.
- **Dusklight's title scene** runs as before.
- **The docs:** docs/PARITY.md's row reads one draw for lists, indexed or not; vulkan4xenon:docs/SUPPORT.md's
  Commands and Shaders, and xost:docs/xsc.md's interface conventions, say what's above.

## Not in it

- **Divisors past 1,** Direct3D 9's `D3DSTREAMSOURCE_INSTANCEDATA | n` and Vulkan's later
  `VK_EXT_vertex_attribute_divisor`: a multiply a stream in the prologue, and a variant for it. They wait for a use.
- **`gl_VertexIndex`,** Direct3D 10's vertex ID, stays refused, though t.x is most of it.
- **Strips and fans in one draw:** joining instances needs a restart or degenerate triangles made per instance. They
  stay an instance a piece.
- **`VGT_INDX_OFFSET`** as Direct3D 9's `BaseVertexIndex` or for long draws; vulkan4xenon's `vertexOffset` keeps moving
  streams.
- **Aurora drawing GX lines and points as instances** (below): Aurora's own work, after this.
- **Anything the XDK's own instancing does:** no Microsoft material.

## What it changes for ports

Mostly parity. Nothing that runs on xsdk today draws instanced:
- **Dusklight and Melee** don't: the GameCube's GX has no instancing. Aurora's WebGPU backend draws GX lines and points
  as instances, one a segment or point (aurora-xenon:lib/gx/command_processor.cpp:726-733). Its vulkan4xenon backend
  instead writes each as six vertices on the CPU and draws one instance (aurora-xenon:lib/vulkan4xenon/glsl.cpp:1412,
  1437, `stream_vertices` and `write_corners`; encoding.cpp:786-788). Melee's particles draw GX points and lines
  (melee-pc:src/sysdolphin/baselib/psdisp.c:563-591, 691-731), and Dusklight's map paths lines and points
  (dusklight:src/d/d_map_path.cpp:568-596). With this milestone Aurora could draw them as the WebGPU backend does,
  six corners at the vertex's rate and the GX vertices at the instance's, writing a sixth of the vertex bytes it writes
  now for points and a third for lines. What that saves a frame is unmeasured, and it's Aurora's step, not this one.
- **vulkan4xenon_test's** instanced rows become a draw each; they draw at most 8 instances, so nothing measurable
  changes there.
- **vkbench** gains a scene that measures it.
- **PC ports of Direct3D 9's years** draw foliage, crowds and particles through `SetStreamSourceFreq`; for them this
  turns thousands of draws into one. No such port exists yet.

## Risks

- **Vertex fetches after ALU instructions.** xost's vertex shaders fetch first. Which vertex-cache bit stands for which
  instruction in other orders isn't known (docs/HARDWARE.md, "A fifth vertex fetch"), and a fetch reading what the ALU
  wrote in its exec needs serializing (docs/HARDWARE.md, "Serialized fetches"). The prologue's exec of its own keeps
  the known order, and the probe tries it before anything is built on it.
- **The console's ALU** may round the division otherwise than Xenia and the simulator; the margin is sixteenfold, and
  the probe checks it.
- **Reading an app's indices** from write-combined memory may cost the layer more than the pieces it saves; the probe
  measures both, and K follows.
- **Constant writes between pieces** hold the GPU as between any draws; with K instances a piece that's amortised, and
  the probe times it.
