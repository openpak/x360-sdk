# Milestone 44: predicated tiling and multisampling

Agreed on 2026-10-17, in two parts, in this order: 44's first part, 46, 47, then 44's second part.
- **The first part:** tiling at one sample, steps 1 and 2, with the layer's tiled passes at one sample from step 5
  brought forward after step 2.
- **The second part:** multisampling, steps 3 and 4 and the rest of step 5.

**The number.** SCOPE.md already names predicated tiling 44, the last of three milestones toward HDR and deferred
rendering, after render target formats (42) and multiple render targets (43). Milestone 42's scope points to it as well:
"Predicated tiling is milestone 44". Since then, vulkan4xenon milestone 25 has drawn the float, 10-bit and 64-bit
formats and four targets at once, all at one sample (docs/HARDWARE.md, "Float, 10-bit and 64-bit targets and four at
once"). That covers 43 and most of 42, so tiling no longer waits on them. This scope keeps 44, so those pointers stay
true and no number is left unused. It also takes what 42 and 43 left at 2 and 4 samples. What is still open in 42
(16_16, 32_32_FLOAT, gamma and the `_AS_` formats) stays there. SCOPE.md's lines for 42 to 44 change when this is
agreed.

## Why

Two rows of vulkan4xenon:docs/PARITY.md have nothing in the layer:
- "2x and 4x multisampling, alpha to coverage": xsdk has multisampling only, and the layer has none (vulkan4xenon
  milestone 17).
- "Targets past eDRAM: 720p multisampled, a big shadow map": Xenos does it by predicated tiling, and xsdk and the
  layer don't. The layer goes up to 1280x720 at most.

What xsdk has:
- **Multisampling** since milestone 32: one 8_8_8_8 target at 2 or 4 samples (`xsdk_gpu_set_target_samples`), with
  resolves that average the samples (docs/HARDWARE.md, "Multisampled targets, milestone 32").
- **Several targets and the other formats only at one sample.** `xsdk_gpu_set_targets` sets `target_samples = 1`
  (src/gpu_draw.c), and TODO.md lists both at 2 or 4 samples as left over.
- **No alpha to coverage.** `RB_COLORCONTROL` is written only by the target set-up, as libxenon's 0x87000007
  (src/gpu_draw.c, `COLOR_CONTROL`).
- **Hierarchical depth stays off at 2 and 4 samples,** as it was never tried there (src/gpu_draw.c,
  `xsdk_gpu_hiz_cleared`).

The layer refuses at `vkCreateFramebuffer` any subpass whose attachments pass eDRAM's 2048 tiles
(vulkan4xenon:docs/SUPPORT.md, Render passes). It refuses every multisampled image and pipeline too.

**eDRAM's size, confirmed.**
- **The size.** A tile is 80x16 samples of 32 bits, 5120 bytes, and there are 2048 of them: 10,485,760 bytes
  (xenia:src/xenia/gpu/xenos.h:414-419).
- **How samples are laid out.** A pixel's 2 samples take two rows of eDRAM, and its 4 a square of two by two
  (xenia:src/xenia/gpu/shaders/edram.xesli:27-32; xenos.h:426-437). Milestone 32 placed and resolved targets on the
  console with these counts.
- **1280x720 needs these tiles:**

  | Samples | Colour and depth | Bytes | Fits? |
  |---|---|---|---|
  | 1 | 16 tiles across by 45 rows, 720 each, 1440 together | 7,372,800 | Yes |
  | 2 | 1440 each, 2880 together | 14,745,600 | No |
  | 4 | 2880 each, 5760 together | 29,491,200 | No |

  The brief's 7.4 MB and 14.7 MB are right.
- **Targets that don't fit even at one sample:**
  - four 32-bit targets with depth at 720p, as a deferred renderer draws them, take 3600 tiles;
  - a 2048x2048 shadow map's depth alone takes 3328.

**Retail titles tiled.** Xenia records some cases:
- 4D5307F1 draws 1120x720 at 2 samples as an upper bin of 576 rows and a lower one of 144
  (xenia:src/xenia/gpu/xenos.h:271-286);
- 565507D9 resolves a right-hand tile (xenos.h:969-981).

## What the references say

- **Predication** (xenia:src/xenia/gpu/command_processor.cc:704-713; the patent Xenia cites, US20060055701, "Predicated
  packet"):
  - A type-3 packet with bit 0 of its header set runs only if `BIN_SELECT & BIN_MASK` is not 0. Other packets always
    run. Register writes are type 0, so they are never skipped.
  - `PM4_SET_BIN_MASK` (0x50) and `PM4_SET_BIN_SELECT` (0x51) each take 64 bits, high word first, by Xenia's reading
    (command_processor.cc:815-828).
  - `PM4_SET_BIN_MASK_LO`/`_HI` and `PM4_SET_BIN_SELECT_LO`/`_HI` (0x60 to 0x63) each take one word
    (command_processor.cc:793-814; xenos.h:1618-1619, 1628-1633).
  - Xenia starts both at 0xFFFFFFFF (command_processor.h:284-285).
  - In the patent, the host builds each draw's mask from screen extents that the GPU gives back from a first pass. The
    patent doesn't say how.
  - Xenia fakes the screen-extent event, `PM4_EVENT_WRITE_EXT`, with extents covering everything
    (command_processor.cc:1219-1247).
  - Xenia has no handler for `PM4_COND_EXEC`, `PM4_DRAW_INDX_BIN` or `PM4_DRAW_INDX_2_BIN`, which reach its
    unimplemented default (command_processor.cc:715-850). Xenia does handle predication and the bin packets. So titles
    seem to send the predicated packets and the bin packets, and not the other three.
- **A tile's offset** (xenia:src/xenia/gpu/registers.h:475, 586-633; draw_util.cc:339-342, 559-581):
  - With `PA_SU_SC_MODE_CNTL`'s `vtx_window_offset_enable` (xsdk always sets it, src/gpu_draw.c), `PA_SC_WINDOW_OFFSET`
    is added to every vertex after the viewport. It is 15 bits signed each way.
  - The window scissor moves with it unless `window_offset_disable` is set.
  - `PA_SC_SCREEN_SCISSOR` doesn't move.
  - Mesa's driver for Adreno 2xx, Xenos's sibling, draws each tile this way
    (mesa:src/gallium/drivers/freedreno/a2xx/fd2_gmem.c, Mesa 24.0.2):
    - the window offset is the negative of the tile's origin (fd2_gmem.c:711-713);
    - the screen scissor is set to the tile's size (fd2_gmem.c:684-688);
    - the frame's draws are recorded once and run as an indirect buffer for each tile;
    - a store buffer is built once and runs after each tile (fd2_gmem.c:144-221).
- **A tile's resolve.**
  - A resolve copies the eDRAM rectangle that is left after the window offset and the scissor. It writes to the same
    coordinates relative to `RB_COPY_DEST_BASE` (xenia:src/xenia/gpu/draw_util.cc:868-876, 1000-1060).
  - By Xenia's account, Direct3D 9 moves `RB_COPY_DEST_BASE` to put a resolve somewhere else in a texture
    (xenos.h:944-955).
  - A tiled texture's base moves in steps of 32x32 texels for 32- and 64-bit texels (xenos.h:1160-1173). In those
    steps, the address of row y, column 0 is y times the pitch times the texel size
    (xenia:src/xenia/gpu/texture_address.h:189-207).
  - Adreno 2xx has `RB_COPY_DEST_OFFSET` for this (fd2_gmem.c:722-725). Xenos's register table has no such register
    (xenia:src/xenia/gpu/register_table.inc:595-604).
- **The pixel position.**
  - Xenia reads the pixel parameters' xy from the host's render-target position, which is the position after the
    offset (xenia:src/xenia/gpu/dxbc_shader_translator.cc:705-725).
  - Adreno's drivers add the tile's offset in the shader (xenia:src/xenia/gpu/registers.h:178-181;
    fd2_gmem.c:727-735).
  - xost's `gl_FragCoord` reads those parameters, which give the pixel's column and row only with
    `SC_OUTPUT_SCREEN_XY` set (docs/HARDWARE.md, "Reads at a0, and the pixel position"). What they hold under a
    window offset isn't known.
- **Alpha to coverage.**
  - `RB_COLORCONTROL`'s `alpha_to_mask_enable` is bit 4. Four 2-bit offsets in bits 24 to 31 lower each pixel's
    thresholds within a quad: 2, 2, 2, 2 for symmetric coverage, or 3, 1, 0, 2 for dithered coverage, by Xenia's
    reading (xenia:src/xenia/gpu/registers.h:682-731).
  - libxenon's 0x87000007, which xsdk writes, already holds 3, 1, 0, 2.
- **Xenia is wrong in places, and unknown packets hang:**
  - `ZPASS_DONE` hung the GPU at every one of 20 tries ("Occlusion queries, vulkan4xenon milestone 28");
  - a 64-bit clear's two halves are the other way round from Xenia's ("Float, 10-bit and 64-bit targets");
  - the visibility query's id comes from `PA_SC_VIZ_QUERY`, not the packet.

  So every packet xsdk has never sent gets a hang-safe console probe before anything relies on it.

## The design

### Multisampled targets in xsdk

- **Several targets, at any format, at 2 or 4 samples:**
  - `xsdk_gpu_set_targets_samples` is `xsdk_gpu_set_targets` with a sample count, and `xsdk_gpu_set_targets` passes 1;
  - `xsdk_gpu_color_target_tiles` takes the count too;
  - a 64-bit format takes twice the tiles at any count (xenos.h:426-437).

  The console decides which formats take samples (milestone 42 asked the same and never ran). A format it refuses is
  refused by an assert and in the header.
- **Resolves:**
  - `xsdk_gpu_resolve_target` already averages through `RB_COPY_CONTROL`'s sample select at the target's count
    (src/gpu_resolve.c). Step 3 checks that it averages each format, the floats among them, on the console.
  - A depth resolve copies one sample, as Xenia limits it (draw_util.cc, `SanitizeCopySampleSelect`). The console says
    which.
- **The sample pattern.**
  - Milestone 32 found where the samples sit, but not which index each one is.
  - The step 3 probe draws an edge with `PA_SC_AA_MASK` letting one sample through at a time, and resolves that sample
    alone (copy sample select 0 to 3). That ties each index to its place.
  - docs/HARDWARE.md records the pattern, and the layer reports it.
- **Hierarchical depth at 2 and 4 samples,** if milestone 30's probe at those counts finds hidden pixels rejected and
  every visible one drawn. Otherwise it stays off at 2 and 4 samples, and the header says so.
- **Alpha to coverage:**
  - `XsdkBlend` takes `alpha_to_coverage`, and `XsdkBlendState` gains `RB_COLORCONTROL`;
  - `xsdk_gpu_set_blend` and `xsdk_gpu_set_blends` write it;
  - the offsets are the ones step 4's probe finds symmetric, so a coverage of n/4 needs an alpha of n/4.

### Tiled passes in xsdk

A pass too large for eDRAM is drawn in **bands**. A band is a run of whole rows across the target's full width, so the
eDRAM pitch stays the target's.

**Recorded once, run once a band.** The title records the pass's draws once, into an `XsdkCommandBuffer`, using
milestone 18's recordings. For each band, xsdk queues the following in the ring:
1. **The band's target state:**
   - the band's surface, set as `xsdk_gpu_set_targets_samples` sets a target, with the viewport and window scissor over
     the whole target;
   - `PA_SC_WINDOW_OFFSET` at (0, -band top);
   - `PA_SC_SCREEN_SCISSOR` at the band's size.
2. **`PM4_SET_BIN_SELECT`** with the band's bit, only if a draw in the recording narrowed its mask.
3. **The recording,** with `PM4_INDIRECT_BUFFER`, as `xsdk_gpu_execute` runs one from the ring today.
4. **Each resolve the pass asked for,** into the rows of its texture that the band covers:
   - `RB_COPY_DEST_BASE` is moved down to the band's top row, which is a multiple of 32;
   - the window offset is 0 and the rectangle is the band's own;
   - the resolve clears the band for the next band, as resolves clear today.

The recording's draws take the state set inside the recording. A band's resolve undoes the target, as every resolve
does, so state set before the pass doesn't carry over.

**Predication, where the title knows a draw's rows.**
- `xsdk_gpu_set_draw_rows(top, bottom)` puts a `PM4_SET_BIN_MASK` of the bands those rows cross into the recording.
  The draw packets after it carry the predicate bit, until the rows are set back to the whole target.
- Only draw packets carry the bit, so constants, fetch constants and shader loads always run, and state stays the same
  in every band.
- xsdk can't find a draw's rows itself. The GPU's extents (`PM4_EVENT_WRITE_EXT`) are another milestone's work (Not in
  it).
- If step 1's probe finds that the bin packets hang or misbehave, tiling works without them: every draw runs in every
  band, and `xsdk_gpu_set_draw_rows` goes.

**The bands.**
- **Height.** As few bands as fit, each the same height, rounded up to a multiple of 32 rows; the last band is
  shorter.
- **Why 32 rows.** The resolve's destination moves in 32-row steps (above). The tiles' 8 or 16 rows and the resolve's
  8-pixel grid (xenos.h:978-984) both divide 32.
- **The same surface in every band.** Giving every band the same height lets every band share one surface and one
  hierarchical buffer. The last band's screen scissor is shorter.
- **The limits:**
  - each band's targets fit 2048 tiles;
  - a target holds at least 32 rows a band;
  - at most 64 bands when draws are predicated, the width of the bin registers;
  - at most 8192 rows, inside the window offset's 15 bits and the 8192-texel textures (xenos.h:1139-1141).

  `xsdk_gpu_tiling_bands` returns the count, or 0 if the pass can't be tiled.
- **Forcing more bands.** A title can ask for more bands than needed, by giving a band height. The tests and step 5's
  cost measurements use this.

**Depth across bands.** Two layouts:
- **Depth in bands,** the default. Each band's depth comes and goes with its colour.
  - The band's resolve clears depth and the hierarchical buffer for the next band. So the recording can keep
    hierarchical depth on: unlike other recordings (src/gpu.c, `xsdk_gpu_record_begin`), xsdk knows the surface it
    runs on and that the surface was just cleared.
  - If a later pass reads this depth, it is resolved per band into a D24S8 texture. At 2 and 4 samples that copies one
    sample, as above.
- **Depth whole,** asked for with `depth_whole`. All the depth stays in eDRAM at the top, and only the colour is
  banded.
  - Each band moves `RB_DEPTH_INFO`'s base down by the band's rows. eDRAM wraps (xenos.h:271-286), and 4D5307F1 is
    recorded drawing its bins against parts of one depth buffer.
  - Depth then survives from one tiled pass to the next, as it survives between passes today. This is the layout for
    a frame of many passes over one depth (step 5).
  - It fits whenever the whole depth and one 32-row band of colour fit: 720p at 1 and 2 samples, but not at 4.
  - Hierarchical depth stays off in this layout, unless step 1's probe finds that `RB_HIZCONTROL`'s bits 18 to 26 move
    the buffer with the depth base. Milestone 30 found those bits move it.

**The pixel position.**
- If step 1 finds that the pixel parameters give the row within the band, xsdk writes the band's top into a pixel
  constant at each band, and xost adds it to `gl_FragCoord.y`. It would be one more constant the runtime writes, as
  it already writes `gl_InstanceIndex` (src/xsc_lower_vertex.c).
- If they give the target's row, nothing changes.

**The API.**
```c
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t samples;
	XsdkColorTarget targets[XSDK_COLOR_TARGETS]; // formats; xsdk places them
	uint32_t count;
	bool depth;
	bool depth_whole;
	uint32_t band_rows; // 0: as few bands as fit
} XsdkTiling;

typedef struct {
	uint32_t target; // a colour target, or XSDK_TILE_DEPTH
	const XsdkTexture *texture;
	float clear_rgba[4];
} XsdkTileResolve;

uint32_t xsdk_gpu_tiling_bands(const XsdkTiling *tiling);
NTSTATUS xsdk_gpu_begin_tiles(const XsdkTiling *tiling, XsdkCommandBuffer *buffer);
void xsdk_gpu_set_draw_rows(uint32_t top, uint32_t bottom);
NTSTATUS xsdk_gpu_end_tiles(const XsdkTileResolve *resolves, uint32_t count);
```
- `xsdk_gpu_begin_tiles` begins recording.
- `xsdk_gpu_end_tiles` ends the recording and queues the bands.
- **Band 0's clear:**
  - If the last tiled pass had the same layout, its last band's resolve already cleared band 0 for this pass.
  - Otherwise `xsdk_gpu_end_tiles` first resolves band 0 into the first resolve's texture with the clear, copying junk
    that the pass then overwrites, as the layer does today (vulkan4xenon:docs/SUPPORT.md, Render passes).
- **What can't go inside:**
  - nothing a recording can't hold: fences, resolves, executes;
  - visibility queries, as a query's end would write its bit once a band, and the last band would decide.

**What a band costs.**
- **The CPU** records the pass once, whatever the band count. Each band adds:
  - about 40 dwords of target state, window offset, scissors and bin select;
  - 3 dwords for the indirect buffer;
  - about 160 dwords for each resolve, as today.

  That is a few microseconds a band.
- **The GPU** runs every draw's vertices, set-up and state once more for each band. Predication skips that for draws
  whose rows the title gave, and the scissor rejects pixels outside the band.
  - Pixel shading and resolving total the same as for one band, since each pixel is in one band.
  - Each band adds:
    - a resolve's set-up and its wait for idle. A whole 720p resolve takes 270 to 279 us, and most of that is copying,
      which the bands share (docs/HARDWARE.md, "The pixel rate, milestone 31");
    - clearing depth and the hierarchical buffer's second pass, about 30 us: a resolve clearing colour and depth took
      319 us against 290 clearing colour alone ("Hierarchical depth, milestone 30").

### vulkan4xenon (its milestone 17, widened as its milestone 25 proposed)

**Multisampled images:**
- **Images:**
  - `VK_SAMPLE_COUNT_2_BIT` and `_4_BIT` images as colour and depth attachments, for the formats step 3 passes;
  - the formats' `sampleCounts` and the device's framebuffer sample counts say so;
  - sampled sample counts stay 1, a deviation, as Xenos samples no multisampled textures.
- **Pipelines:**
  - `rasterizationSamples` 2 and 4;
  - `alphaToCoverageEnable`;
  - `standardSampleLocations` only if step 3's pattern is Vulkan's, index for index;
  - `pSampleMask` other than all ones, `alphaToOneEnable` and sample shading are refused.
- **Resolve attachments.** A subpass's resolve attachment is the averaging resolve at the pass's end.
- **Storing a multisampled attachment.** It stores the average of its samples into its image's memory, and loading it
  draws that average back into every sample. That is a deviation: the samples aren't kept apart. Edges drawn before
  keep their blend, and the next pass draws new edges.
- **`vkCmdResolveImage`** copies that stored average.

**Render passes past eDRAM:**
- A render pass whose subpass doesn't fit eDRAM becomes a tiled pass, where `vkCreateFramebuffer` now refuses it.
- **How it maps:**
  - the subpass's commands are replayed once, into a recording;
  - each load op's draw-back goes at the start of the recording, so the scissor clips it to each band;
  - each store op is a band's resolve.
- **Depth:**
  - The layer asks for depth whole when it fits, so depth stays in eDRAM between passes as today.
  - Otherwise it stores depth per band, and a later pass that loads it draws it back from the pixel shader. That gives
    one depth a pixel at 2 and 4 samples, and loses stencil, as Xenos exports no stencil. Both deviations are logged
    once.
- **What eDRAM holds.** The layer's tracking of eDRAM's contents learns that after a tiled pass, eDRAM holds the last
  band and the clear its resolve made.
- **Refused in a tiled pass:**
  - queries and time stamps;
  - a render area smaller than the framebuffer.

## What it takes

Each step is built, then verified as below, before the next one begins.

1. **The tiling probe** (`tests/probe_tiles.c`). It sends nothing xsdk relies on yet. The cases go from what xsdk
   already writes to what it has never sent. Each case that sends a new packet runs from a fresh boot. Every case
   ends with a fence and a resolve read back by the CPU.
   - **The window offset.**
     - **The draw:**
       - a 256x128 target drawn with an offset of (0, -64) and a screen scissor of 256x64;
       - a quad across all of its rows;
       - sentinel colours past the scissor.
     - **The checks:**
       - rows 64 to 127 land in eDRAM rows 0 to 63, and nothing else is written;
       - the window scissor moves with the offset, and stops moving with `window_offset_disable`.
     - **The pixel parameters.** A shader exports them as colour, with `SC_OUTPUT_SCREEN_XY` as xost sets it, to show
       whether `gl_FragCoord.y` is the band's row or the target's.
   - **A resolve whose destination moves** to rows 32, 64 and 96 of a tiled texture full of sentinels. The band lands
     at its rows, with the rows above and below untouched.
   - **Depth moved by a band** (depth whole):
     - draws of near and far quads across bands, with `RB_DEPTH_INFO`'s base moved per band and wrapping past tile
       2048, each pixel checked;
     - hidden quads timed with hierarchical depth on, with `RB_HIZCONTROL`'s bits 18 to 26 moved with the base and not
       moved.
   - **Predication.** Four draws carry the predicate bit, with masks 1, 2, 1 << 32 and all ones. They run under selects
     1, 2 and 1 << 32. The probe records which draws land:
     - first with `PM4_SET_BIN_MASK` and `_SELECT` (0x50, 0x51), with both word orders tried;
     - then with the one-word packets (0x60 to 0x63);
     - then with the packets and the draws inside a recording run from the ring;
     - and a predicated draw before any bin packet, to show what XAM leaves set.

     The last case sets both registers to all ones and ends the title. The Guide then opens over the dashboard, and
     the next title draws, to show XAM's device isn't left skipping its own predicated packets.
   - **On the PC:** the probe builds, and Xenia runs it as a smoke check only. Xenia models the offset, the resolve and
     the bins, so a case that fails there is wrong in the probe.
   - **On the console:** each case's log line and resolved words. docs/HARDWARE.md records what the console did, with
     the date.
   - **Break set:**
     - the predicate bit left off: every draw lands under every select;
     - the window offset's sign flipped: nothing lands in the target;
     - the destination base not moved: every band lands at row 0.

     The probe's checks for each must fail.
2. **Tiling at one sample.**
   - **Build:** the tiled-pass API above, without samples. Also xost's band offset for `gl_FragCoord`, if step 1 asks
     for it.
   - `tests/test_tiles.c` in xsdk_test, each check under a second. Every check compares a tiled frame with a reference:
     the same draws made untiled into a target small enough to fit, moved by the viewport so its rows line up, with
     every pixel of every target equal.
     - **1280x720, one 8_8_8_8 target, forced into 2 and 3 bands.** The draws:
       - triangles whose shallow and steep edges cross the band boundaries;
       - a full-screen `gl_FragCoord` gradient;
       - near and far quads over a depth clear, with hierarchical depth on;
       - a draw given band 0's rows by `xsdk_gpu_set_draw_rows` whose triangles reach band 1, which must not show
         there.
     - **Four targets at 720p** (8_8_8_8, 2_10_10_10, 16_16_16_16_FLOAT and 32_FLOAT) with depth: 3600 tiles, so 2
       bands. Each target is resolved into its texture and read by value.
     - **A 2048x2048 depth-only pass** (2 bands). Its depth is resolved per band, and a quad samples it, each texel
       checked.
     - **Depth whole at 720p, forced into 3 bands,** checked against the reference.
     - The frames show on screen, each variant in its own colours, so a capture tells them apart.
   - **Cost:** `probe_tiles` gets a timing case. It times by the GPU's clock (`xsdk_gpu_write_clock`):
     - a 720p frame of a fill-bound scene and of a vertex-bound one, at 1, 2 and 3 bands;
     - one band's fixed cost;
     - a predicated draw outside its band against the same draw drawn.
   - **On the PC:** the new test's code is built clean in full, and xsdk_test runs in Xenia as a smoke check.
   - **On the console:**
     - xsdk_test in full;
     - vulkan4xenon_test and vkbench, both unchanged;
     - docs/HARDWARE.md records the numbers.
   - **Break set, each failing its checks:**
     - the window offset left at 0: below band 0 nothing matches;
     - the destination base not moved: every band lands at the top;
     - a band's resolve not clearing: band 1 shows band 0's draws where it has none;
     - the bin select left all ones: the band-0 draw shows in band 1;
     - the hierarchical buffer's second pass left out per band: hidden quads show at band tops;
     - `gl_FragCoord`'s band offset left out, if built: the gradient breaks at the boundaries.
3. **Multisampled formats and targets.**
   - **Build:**
     - `xsdk_gpu_set_targets_samples` and the tile counts;
     - tiled passes at 2 and 4 samples;
     - hierarchical depth at 2 and 4 samples, if the probe passes.
   - **First, `probe_msaa` extended:**
     - each target format at 2 and 4 samples: whether it draws, blends and averages on resolve;
     - four targets at once at 2 and 4 samples;
     - each sample index's place, through `PA_SC_AA_MASK` and single-sample resolves;
     - which sample a depth resolve copies;
     - milestone 30's hidden-pixel probe at 2 and 4 samples;
     - milestone 31's fill and blend rates, and a 720p resolve's time, at 1, 2 and 4 samples.
   - **xsdk_test's `test_multisample.c` and `test_tiles.c`:**
     - **Edge counts.** Milestone 32's edge counts for each format the probe passes, at 2 and 4 samples, in steps of a
       half and a quarter, within each format's precision.
     - **Four targets at once** at 2 and 4 samples, read by value.
     - **720p tiled:**
       - **At 2 samples:** 2 bands, and 3 with depth whole. The reference is untiled at 1280x512, which fits exactly.
       - **At 4 samples:** 3 bands. The reference is untiled at 1280x256.
       - In both, every pixel is equal, the boundary rows among them.
   - **On the console:** as step 2. docs/HARDWARE.md records the probe's answers and rates.
   - **Break set:**
     - resolves copying sample 0: the edge counts fail;
     - `xsdk_gpu_set_targets_samples` writing 1 sample: no edge blends;
     - a 64-bit target's tiles not doubled at 4 samples: another target is overwritten;
     - the bands of a 4-sample pass sized as for 2 samples: band 1's rows wrap into band 0's.
4. **Alpha to coverage.**
   - **First, a probe:** quads at alphas from 0 to 1 in steps of 1/16, at 1, 2 and 4 samples, with offsets 2, 2, 2, 2
     and 3, 1, 0, 2. It counts the samples covered over each quad of pixels.
   - **Build:** `XsdkBlend.alpha_to_coverage`, with the offsets the probe finds symmetric.
   - **xsdk_test:** at each alpha step and sample count, each resolved pixel equals the coverage the probe recorded.
     The quads show side by side.
   - **On the console:** as step 2.
   - **Break set:**
     - the enable bit left off: every alpha below 1 resolves as covered;
     - the offsets left at 0: the thresholds shift, and the steps fail.
5. **The layer** (vulkan4xenon milestone 17), then Aurora and Dusklight.
   - **Build:** the layer's part above.
   - **Aurora's vulkan4xenon backend** (aurora-xenon) draws the EFB at the sample count it's asked for, instead of
     warning and drawing at one (aurora-xenon:lib/vulkan4xenon/backend.cpp:422-424, 497-501):
     - its copies read the resolve attachment;
     - its depth copies and GXPeekZ read one sample's depth.

     Upstream's WebGPU path refuses depth copies from a multisampled EFB (aurora-xenon:lib/gfx/encoding.cpp:319-321),
     and Twilight Princess makes one every frame (vulkan4xenon:docs/ROADMAP.md, milestone 14).
   - **Dusklight takes a setting for the sample count.** Today it sets none, so Aurora's default of 1 applies
     (aurora-xenon:lib/aurora.cpp:72-74; dusklight-xenon:src/m_Do/m_Do_main.cpp:694-740).
   - **vulkan4xenon_test's `test_multisample.c` and `test_tiles.c`.** Each is checked by pixel or by value, and each
     check fails with its break.
     - **Multisampling:**
       - a pass at 2 and 4 samples with a resolve attachment, its edges counted as in xsdk_test;
       - alpha to coverage's steps;
       - the formats' sample counts and the refusals.
     - **A tiled pass of four attachments at 720p,** loaded and stored across two passes.
     - **Tiled passes at 720p** at 2 and 4 samples, depth loaded across passes in both layouts.
     - **A 2048x2048 shadow-map pass** sampled after.
   - **On the console:**
     - vulkan4xenon_test, twice;
     - xsdk_test and vkbench.
   - **Dusklight, through the diagnostics build** (its latest sources in
     vulkan4xenon:results/2026-10-15-milestone-14-steps-5-6/diagnostic):
     - **First, at one sample with every pass forced into 2 and then 3 bands.** This is the cost of tiling alone: GPU ms
       a frame against unbanded.
     - **Then at 2 samples** with depth whole, and **at 4 samples.**
     - **What's recorded for each:**
       - fps and GPU ms a frame, in the logo shot and in Ordon;
       - the logo shot's frame captured at the same frame as at 1 sample, and compared by eye and by difference
         image.
   - **Break set:**
     - resolve attachments resolving sample 0;
     - the tiled pass's window offset left at 0;
     - `alphaToCoverageEnable` ignored;
     - the load's draw-back skipped in bands after the first.

     Each fails the checks that `breaks.py` names for it.

## Verification, without the user

Runs are driven from the PC through the user's local console agent, as for milestone 45. It uploads, launches,
fetches logs and reboots.
- **Probes are hang-safe.** A probe case that sends a packet xsdk has never sent runs from a fresh boot. Its log line
  is written and flushed before the packet. A hang ends that run, and the agent's reboot recovers the console, as it
  did for milestone 28's 13 hung tries.
- **When the user is needed:** only if the console stops answering the agent, or if xbWatson breaks into an assert. So
  xbWatson stays closed during the runs.
- **Xenia is a smoke check only.** It skips nothing these steps add, but its MSAA resolves and pixel position aren't
  evidence, and its screen extents are fake.

## Done when

- **The probes:** docs/HARDWARE.md records what steps 1, 3 and 4 found:
  - the window offset, the pixel position and the moved resolve;
  - depth moved by a band;
  - predication, or that it hung and tiling does without it;
  - each format's sample counts and resolves;
  - the sample pattern by index;
  - hierarchical depth at 2 and 4 samples;
  - alpha to coverage's thresholds.
- **xsdk_test passes on the console** with the new checks:
  - every tiled frame equals its reference pixel for pixel, at 720p at 1 sample in 2 and 3 bands, at 2 samples in
    2 and 3 bands, and at 4 samples in 3 bands;
  - the four-target and 2048x2048 passes read back by value;
  - the edge counts and alpha-to-coverage steps hold.

  Each break set fails the checks it aims at.
- **The cost of tiling, recorded in docs/HARDWARE.md from `probe_tiles`, all by the GPU's clock:**
  - **One band's fixed cost.** It should be under 50 us, past the resolve's own copying time.
  - **A predicated draw outside its band.** It should cost the GPU under a fifth of the same draw drawn.
  - **720p frames of the fill-bound and vertex-bound scenes:** at 1 sample untiled, at 2 samples in 2 and 3 bands,
    and at 4 samples in 3 bands.

  If the fixed cost or the skipped draw misses its bar, the record says where the time goes, as milestone 31's did.
- **vulkan4xenon_test** passes twice on the console with the step 5 checks, and fails each break. vkbench's times stay
  within 3% of before. docs/PARITY.md's two rows become "Yes", with the deviations above in docs/SUPPORT.md.
- **Dusklight's frame times and captures** at forced bands, 2 samples and 4 samples are recorded against one sample.
  Its setting stays at 1 sample unless 2 samples holds the title scene and Ordon at 29.5 fps or better.

## Not in it

- **The GPU finding draws' rows:**
  - `PM4_EVENT_WRITE_EXT`'s screen extents from a first band;
  - `PM4_COND_EXEC`, `PM4_DRAW_INDX_BIN` and `PM4_COND_WRITE` for masks the GPU makes.

  Each is a packet xsdk hasn't sent, for a later milestone if step 2's numbers show re-run vertices matter.
- **Bands of columns,** and bands that start off a 32-row step.
- **Multisampled attachments stored sample by sample.** That would mean each sample resolved apart and drawn back under
  `PA_SC_AA_MASK`, at two or four times the cost. Also left out:
  - sampled multisampled images;
  - sample masks other than all ones;
  - sample shading, `gl_SampleID` and `gl_SampleMask`;
  - centroid interpolation;
  - alpha to one.
- **Stencil kept across passes when depth is in bands.**
- **Milestone 42's leftovers at any sample count:** 8_8_8_8_GAMMA, 16_16, 32_32_FLOAT and the `_AS_` formats.
- **The alpha test** (docs/PARITY.md's xsdk "No"), though it shares `RB_COLORCONTROL`.
- **In tiled passes:** queries and time stamps, and render areas smaller than the framebuffer.
- **A lower resolution in place of tiling.**

## Real-world payoff

**Tiling at one sample is where most of the value is.** PC games of Direct3D 9's years that ports will come from draw
G-buffers of three or four targets at 720p (3600 tiles) and shadow maps of 2048x2048 (3328 tiles). Today neither can
draw on xsdk at all. Steps 1 and 2, with the layer's tiled passes, give them that. These steps are mostly known
registers; predication is the one unknown, and the design works without it.

**Multisampling is parity with modest payoff.** Retail titles used 2 samples at 720p with two bins. A port that offers
MSAA on the PC can offer it here. But GameCube- and Wii-era ports draw little geometry, and the display's 2-tap scaler
to 1080p softens edges already (docs/HARDWARE.md, "The built-in 2-tap filter").

**For Dusklight, it is large work for a modest gain, and maybe not affordable at 4 samples.** The estimate below comes
from figures measured for other reasons. Step 5 measures the real cost.
- **Today:** 720p at one sample. The GPU takes 17.9 ms a frame in the logo shot and 18.1 to 19.0 ms in Ordon, under
  the game's 33.3 ms (vulkan4xenon:docs/ROADMAP.md, milestone 14). The logo shot's frame has 17 passes and 11 copies
  (vulkan4xenon:results/2026-10-13-milestone-12/dusklight_diag.log:803).
- **Why tiling hurts this frame.** At one sample, a pass after a copy finds the EFB's colour still in eDRAM and draws
  nothing back (vulkan4xenon:docs/ROADMAP.md, milestone 4). Banded, every pass that loads colour draws it back. A
  720p draw-back is about 0.3 ms at the 3.4 Gpixel/s fill milestone 31 reached. With some ten loading passes, that is
  about 3 ms a frame.
- **Vertices run once more for each extra band.** On 2026-10-13, every draw scissored to one pixel still took 7.9 ms
  of 25.9, and that included the copies and resolves (milestone 11). Milestone 45 has cut vertex shading by 19% since.
  So an extra band likely costs a few ms.
- **The 2-sample resolves** of the copies add an unknown amount on top.
- **Altogether:**
  - **2 samples with depth whole** (3 bands): likely 5 to 10 ms more, so 23 to 29 ms. That's near the edge of 30 fps.
  - **4 samples:** depth can't stay whole, so every pass draws depth back too. That likely passes 33 ms.
- **The look.** MSAA smooths the stair-steps on geometry edges. The game's alpha-tested foliage stays aliased, as
  Aurora never uses alpha to coverage (aurora-xenon:lib has no `alphaToCoverage`).
- **The work it takes.**
  - Aurora's backend changes, and Twilight Princess's per-frame depth copy has to read a multisampled depth.
  - Upstream Aurora itself refuses that depth copy on the PC.

**What it suggests.**
- Agree to steps 1 and 2, with the layer's tiled passes at one sample, as a first part. They could come straight
  after step 2 instead of in step 5.
- Treat steps 3 and 4 and the layer's multisampling as parity work with full tests.
- Treat Dusklight at 2 samples as a measurement, its setting off unless the numbers allow it.
