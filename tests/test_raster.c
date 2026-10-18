#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsc_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"
#include "xsdk/xsc.h"

// One cell per piece of raster and blend state, each checked by pixel against what the CPU works out; then, for
// vulkan4xenon milestone 26, a row of quads drawn as their edges and their corners, a row of cells clipped by planes,
// and point sprites from xost's shaders compiled on the console; and a grid of quads coloured from a block at their
// vertices' index, read at a0 past the interpolators' alloc (docs/HARDWARE.md, "a0 doesn't survive an alloc"), two
// coloured by pixel shaders reading at a0 past fetch grouping and in and past if/else arms (docs/HARDWARE.md, "a0
// through fetch grouping" and "a0 and the predicate's execs"), and one by fetches far across a large texture, read in
// a later exec (docs/HARDWARE.md, "Slow fetches across execs"); and, for milestone 46 steps 2 and 3, grids whose pixels
// each leave an unrolled loop and a hardware loop at their own pass, on breaks and continues on data xost gates, and
// the hardware loop timed with every pixel leaving at its first pass of 255, against a count of 1.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CELL_WIDTH = 200,
	CELL_HEIGHT = 180,
	// Blends round once to 8 bits; the CPU's sums may land a step either side.
	BLEND_TOLERANCE = 2,
	POINTS = 48,
	// The clip plane row: cells 200 by 80.
	CLIP_Y = 320,
	CLIP_HEIGHT = 80,
	// The planes' lines: the column the first cell's plane keeps from, and the row and column the second's two keep
	// above and left of.
	CLIP_COLUMN = 140,
	CLIP_QUARTER_ROW = 360,
	CLIP_QUARTER_COLUMN = 390,
	// The point sprites' row, and how far a sprite's colour may land from what its coordinates give: two pixels and a
	// bit of the smaller one's, as where in a pixel the hardware takes them isn't known.
	SPRITE_Y = 360,
	SPRITE_TOLERANCE = 24,
	// The fill mode row: quads 200 by 80, their corners on pixels' centres, and how far a line or point may land.
	FILL_Y = 20,
	FILL_WIDTH = 200,
	FILL_HEIGHT = 80,
	FILL_SLACK = 1,
	// The indexed grid, one draw: 70 by 8 quads 10 pixels square right of the constant blends, each centre checked.
	INDEXED_X = 540,
	INDEXED_Y = 620,
	INDEXED_SIZE = 10,
	INDEXED_COLUMNS = 70,
	INDEXED_ROWS = 8,
	INDEXED_QUADS = INDEXED_COLUMNS * INDEXED_ROWS,
	INDEXED_VERTICES = INDEXED_QUADS * QUAD_VERTICES,
	// The length of each of vk_dynamic_interpolator.vert's arrays, its scales and its colours.
	INDEXED_INDICES = 16,
	MAX_INDEXED_LOGGED = 8,
	// The pixel shaders' grids right of the fill row, each 35 by 8 quads the indexed grid's size.
	PIXEL_GRID_Y = 20,
	PIXEL_GRID_COLUMNS = 35,
	PIXEL_GRID_ROWS = 8,
	PIXEL_GRID_QUADS = PIXEL_GRID_COLUMNS * PIXEL_GRID_ROWS,
	PIXEL_GRID_VERTICES = PIXEL_GRID_QUADS * QUAD_VERTICES,
	TEXCOORD_X = 540,
	UNIFORM_IF_X = 890,
	// A position, then the quad's indices i and j for the pixel shader, as triangle_vertex_shader's colour.
	PIXEL_VERTEX_FLOATS = 8,
	PIXEL_STRIDE_BYTES = PIXEL_VERTEX_FLOATS * 4,
	// vk_dynamic_texcoord.frag's 32 by 32 texture in 4 by 4 blocks, a block an index, and its coordinates and scales.
	TEXTURE_SIDE = 32,
	BLOCKS_SIDE = 4,
	BLOCK_SIDE = TEXTURE_SIDE / BLOCKS_SIDE,
	TEXCOORD_ROWS = 2 * INDEXED_INDICES,
	// vk_dynamic_if.frag's rows: colours, tints and the mode, which picks the arm; its first draw takes the arm reading
	// at j, over the grid's top half.
	UNIFORM_IF_ROWS = 2 * INDEXED_INDICES + 1,
	MODE_ROW = 2 * INDEXED_INDICES,
	ARM_TAKEN_QUADS = PIXEL_GRID_QUADS / 2,
	// vk_slow_fetch.frag's grid right of the point sprites, 16 by 8 quads, every pixel checked. Its texture's rows each
	// hold their class's colour, row mod 16; a quad's class is its index, and its pixels sample 37 columns and 3 steps
	// of 16 rows apart, each in its own tile, and quads 331 columns and 7 steps apart.
	SLOW_X = 1060,
	SLOW_Y = 320,
	SLOW_COLUMNS = 16,
	SLOW_ROWS = 8,
	SLOW_QUADS = SLOW_COLUMNS * SLOW_ROWS,
	SLOW_VERTICES = SLOW_QUADS * QUAD_VERTICES,
	SLOW_SIDE = 1024,
	SLOW_PIXEL_COLUMNS = 37,
	SLOW_PIXEL_STEPS = 3,
	SLOW_QUAD_COLUMNS = 331,
	SLOW_QUAD_STEPS = 7,
	// The indexed grid, then the pixel shaders'.
	GRIDS = 4,
	// vk_loop_grid.frag's grid between the point sprites and the slow fetch grid, one quad 100 by 80, every pixel
	// checked: its column and row in a 10-pixel cell, from gl_FragCoord, pick the passes it leaves its loop at.
	LOOP_X = 950,
	LOOP_Y = 320,
	LOOP_WIDTH = 100,
	LOOP_HEIGHT = 80,
	LOOP_CELL = 10,
	LOOP_PASSES = 8,
	// vk_loop_count_grid.frag's grid right of everything else, one quad 40 by 280, every pixel checked, its loop a push
	// constant counts; then the same shader over the whole target, every pixel leaving at the loop's first pass, timed
	// at the most a loop constant counts against a count of 1, the best of a few rounds each.
	COUNT_X = 1240,
	COUNT_Y = 120,
	COUNT_WIDTH = 40,
	COUNT_HEIGHT = 280,
	COUNT_PASSES = 8,
	TIMED_PASSES = 255,
	TIMED_ROUNDS = 4,
	// Leaving early costs nothing past the passes run, within a tenth (docs/scope/46-loops.md, "Done when").
	TIMED_SLACK_TENTHS = 11,
};

// Each grid's check, which its count of wrong quads follows.
static const char *const grid_checks[GRIDS] = {
	"raster: one draw of xost's vk_dynamic_interpolator.vert compiled on the console colours each of 560 quads as its "
	"vertices' index, 0 to 15, picks, read at a0 past the interpolators' alloc",
	"raster: one draw of xost's vk_dynamic_texcoord.frag compiled on the console samples each of 280 quads' texel at "
	"coords[i], i 0 to 15, read at a0 past j's load",
	"raster: two draws of xost's vk_dynamic_if.frag compiled on the console colour each of 280 quads from rows at i "
	"read before and past an if/else on a uniform whose arm reads at j, the arm taken in the first",
	"raster: one draw of xost's vk_slow_fetch.frag compiled on the console colours every pixel of 128 quads from two "
	"fetches far across a 1024 by 1024 texture, read in the next exec, and a third at the row they give, pixels",
};

// Point sprites, each centred on a pixel's corner, as wide as its size, from vk_point_sprite.vert.
typedef struct {
	uint32_t x;
	float size;
} Sprite;

static const Sprite sprites[] = { { 820, 24.0f }, { 900, 64.0f } };

enum {
	BLEND_NONE,
	BLEND_ALPHA,
	BLEND_ADD,
	BLEND_SUBTRACT,
	BLEND_MAX,
	BLEND_MASK_RED_BLUE,
	// The source and destination mixed by a constant colour's channels, then by a constant's alpha.
	BLEND_CONSTANT_COLOR,
	BLEND_CONSTANT_ALPHA,
	BLENDS,
};

// Each constant blend's own colour, set before its draw, red to alpha.
static const float constant_color[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
static const float constant_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.6f };

enum {
	RASTER_PLAIN,
	RASTER_CULL_BACK,
	RASTER_CULL_FRONT,
	RASTER_BIAS,
	RASTER_WIREFRAME,
	RASTER_POINTS,
	RASTERS,
};

// Pulls the biased rectangle toward the viewer, well past one 24-bit step.
static const float depth_bias = -0.01f;

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	// Alpha in the top byte.
	uint32_t argb;
	uint32_t blend;
	uint32_t raster;
	// Placed right to left, which reverses the winding.
	bool mirrored;
	bool depth_test;
	bool scissor;
	// Drawn over all of clip space, into a viewport set to the rectangle.
	bool viewport;
} Draw;

// Cells 200x180: x 40, 290, 540, 790 and 1040; y 120 and 420.
static const Draw draws[] = {
	// Alpha blend: half-transparent orange over the right of a blue base.
	{ 40, 120, 200, 180, 0xFF2080E0, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 140, 120, 100, 180, 0x80E08020, BLEND_ALPHA, RASTER_PLAIN, false, false, false, false },
	// Additive.
	{ 290, 120, 200, 180, 0xFF6040C0, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 390, 120, 100, 180, 0xFF60E010, BLEND_ADD, RASTER_PLAIN, false, false, false, false },
	// Subtract, source less destination.
	{ 540, 120, 200, 180, 0xFF40C020, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 640, 120, 100, 180, 0xFFE04080, BLEND_SUBTRACT, RASTER_PLAIN, false, false, false, false },
	// Max.
	{ 790, 120, 200, 180, 0xFF30D050, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 890, 120, 100, 180, 0xFFC04090, BLEND_MAX, RASTER_PLAIN, false, false, false, false },
	// Write mask: only red and blue of the overlay land.
	{ 1040, 120, 200, 180, 0xFF20E020, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 1140, 120, 100, 180, 0xFFE020E0, BLEND_MASK_RED_BLUE, RASTER_PLAIN, false, false, false, false },
	// Cull back with clockwise front faces: a clockwise rectangle, then a mirrored one.
	{ 40, 420, 90, 180, 0xFFE04040, BLEND_NONE, RASTER_CULL_BACK, false, false, false, false },
	{ 150, 420, 90, 180, 0xFF40E040, BLEND_NONE, RASTER_CULL_BACK, true, false, false, false },
	// Cull front.
	{ 290, 420, 90, 180, 0xFF4040E0, BLEND_NONE, RASTER_CULL_FRONT, false, false, false, false },
	{ 400, 420, 90, 180, 0xFFE0E040, BLEND_NONE, RASTER_CULL_FRONT, true, false, false, false },
	// Scissor: a rectangle over the whole cell, cut to x 580-699 and y 450-569.
	{ 540, 420, 200, 180, 0xFFE040E0, BLEND_NONE, RASTER_PLAIN, false, false, true, false },
	// Viewport: all of clip space into x 820-959, y 440-579.
	{ 820, 440, 140, 140, 0xFF40E0E0, BLEND_NONE, RASTER_PLAIN, false, false, false, true },
	// Depth bias: a base at 0.5, then the same depth on top, unbiased on the left and biased on the right.
	{ 1040, 420, 200, 180, 0xFF808080, BLEND_NONE, RASTER_PLAIN, false, true, false, false },
	{ 1040, 420, 100, 180, 0xFFFF8000, BLEND_NONE, RASTER_PLAIN, false, true, false, false },
	{ 1140, 420, 100, 180, 0xFFFF8000, BLEND_NONE, RASTER_BIAS, false, true, false, false },
	// Constant blends, cells 80 high at y 620: by a colour's channels, then by an alpha.
	{ 40, 620, 200, 80, 0xFF2040E0, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 140, 620, 100, 80, 0xFFE0C020, BLEND_CONSTANT_COLOR, RASTER_PLAIN, false, false, false, false },
	{ 290, 620, 200, 80, 0xFF40E080, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 390, 620, 100, 80, 0xFFC02060, BLEND_CONSTANT_ALPHA, RASTER_PLAIN, false, false, false, false },
};

// Each quad over its cell, clipped to where the planes its mask names keep; the last is drawn with them all off again.
typedef struct {
	uint32_t x;
	uint32_t argb;
	uint32_t mask;
} ClipDraw;

static const ClipDraw clip_draws[] = {
	{ 40, 0xFFE08040, 1 << 0 },
	{ 290, 0xFF40C0E0, 1 << 3 | 1 << 5 },
	{ 540, 0xFFC0E040, 0 },
};

// The fill mode row: a quad as its triangles' edges, then one as their corners.
typedef struct {
	uint32_t x;
	uint32_t argb;
	uint32_t raster;
} FillDraw;

static const FillDraw fill_draws[] = {
	{ 40, 0xFFE0E040, RASTER_WIREFRAME },
	{ 290, 0xFF40E0E0, RASTER_POINTS },
};

// A pixel near which a line or point should or shouldn't be: any pixel within the slack holds the colour, or none.
typedef struct {
	const char *what;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	bool absent;
} FillPoint;

static const uint32_t scissor_x = 580;
static const uint32_t scissor_y = 450;
static const uint32_t scissor_size = 120;

typedef struct {
	const char *what;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	uint32_t tolerance;
} Point;

static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

typedef struct {
	XsdkBlendState blends[BLENDS];
	XsdkRasterState rasters[RASTERS];
	XsdkDepthStencilState depth_off;
	XsdkDepthStencilState depth_less;
} States;

static void make_states(States *states)
{
	const XsdkBlend blends[BLENDS] = {
		[BLEND_NONE] = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_ALPHA] = { XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
			XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_ADD] = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_SUBTRACT] = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_SUBTRACT, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_MAX] = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_MAX, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_MASK_RED_BLUE] = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_RED | XSDK_WRITE_BLUE },
		[BLEND_CONSTANT_COLOR] = { XSDK_BLEND_CONSTANT_COLOR, XSDK_BLEND_ONE_MINUS_CONSTANT_COLOR, XSDK_BLEND_OP_ADD,
			XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_CONSTANT_ALPHA] = { XSDK_BLEND_CONSTANT_ALPHA, XSDK_BLEND_ONE_MINUS_CONSTANT_ALPHA, XSDK_BLEND_OP_ADD,
			XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
	};
	const XsdkRaster rasters[RASTERS] = {
		[RASTER_PLAIN] = { XSDK_CULL_NONE, true, 0.0f, 0.0f, XSDK_FILL_SOLID },
		[RASTER_CULL_BACK] = { XSDK_CULL_BACK, true, 0.0f, 0.0f, XSDK_FILL_SOLID },
		[RASTER_CULL_FRONT] = { XSDK_CULL_FRONT, true, 0.0f, 0.0f, XSDK_FILL_SOLID },
		[RASTER_BIAS] = { XSDK_CULL_NONE, true, depth_bias, 0.0f, XSDK_FILL_SOLID },
		[RASTER_WIREFRAME] = { XSDK_CULL_NONE, true, 0.0f, 0.0f, XSDK_FILL_WIREFRAME },
		[RASTER_POINTS] = { XSDK_CULL_NONE, true, 0.0f, 0.0f, XSDK_FILL_POINTS },
	};
	for (uint32_t i = 0; i < BLENDS; i++)
		xsdk_gpu_blend_state(&states->blends[i], &blends[i]);
	for (uint32_t i = 0; i < RASTERS; i++)
		xsdk_gpu_raster_state(&states->rasters[i], &rasters[i]);
	const XsdkDepthStencil off = { .depth_test = false };
	const XsdkDepthStencil less = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	xsdk_gpu_depth_stencil_state(&states->depth_off, &off);
	xsdk_gpu_depth_stencil_state(&states->depth_less, &less);
}

static float channel(uint32_t argb, uint32_t shift)
{
	return (argb >> shift & 0xFF) / 255.0f;
}

static uint32_t to_byte(float value)
{
	const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)(clamped * 255.0f + 0.5f);
}

// What the colour channels of src drawn over dst come to, as the CPU works out each blend.
static uint32_t blended(uint32_t blend, uint32_t src, uint32_t dst)
{
	uint32_t argb = 0xFF000000;
	const float alpha = channel(src, 24);
	for (uint32_t shift = 0; shift <= 16; shift += 8) {
		const float s = channel(src, shift);
		const float d = channel(dst, shift);
		float out = s;
		if (blend == BLEND_ALPHA)
			out = s * alpha + d * (1.0f - alpha);
		else if (blend == BLEND_ADD)
			out = s + d;
		else if (blend == BLEND_SUBTRACT)
			out = s - d;
		else if (blend == BLEND_MAX)
			out = s > d ? s : d;
		else if (blend == BLEND_MASK_RED_BLUE && shift == 8)
			out = d;
		else if (blend == BLEND_CONSTANT_COLOR)
			out = s * constant_color[2 - shift / 8] + d * (1.0f - constant_color[2 - shift / 8]);
		else if (blend == BLEND_CONSTANT_ALPHA)
			out = s * constant_alpha[3] + d * (1.0f - constant_alpha[3]);
		argb |= to_byte(out) << shift;
	}
	return argb;
}

// Screen-space signed area of the rectangle's first triangle, y down: positive is clockwise as it looks.
static bool clockwise(const Draw *draw)
{
	const float left = draw->mirrored ? (float)(draw->x + draw->width) : (float)draw->x;
	const float right = draw->mirrored ? (float)draw->x : (float)(draw->x + draw->width);
	const float top = (float)draw->y;
	const float bottom = (float)(draw->y + draw->height);
	// Its vertices 0, 1 and 2 are the top left, top right and bottom left, so the cross product reduces to this.
	return (right - left) * (bottom - top) > 0.0f;
}

static bool drawn_when_culled(const Draw *draw)
{
	const bool front = clockwise(draw);
	if (draw->raster == RASTER_CULL_BACK)
		return front;
	if (draw->raster == RASTER_CULL_FRONT)
		return !front;
	return true;
}

static uint32_t points_expected(Point *points)
{
	uint32_t n = 0;
	// The blend cells: the base alone, then the overlay over it.
	for (uint32_t i = 0; i < 10; i += 2) {
		const Draw *const base = &draws[i];
		const Draw *const top = &draws[i + 1];
		points[n++] = (Point){ "base alone", base->x + 50, base->y + 90, base->argb, 0 };
		points[n++] =
			(Point){ "blended", top->x + 50, top->y + 90, blended(top->blend, top->argb, base->argb), BLEND_TOLERANCE };
	}
	for (uint32_t i = 10; i < 14; i++) {
		const Draw *const d = &draws[i];
		points[n++] = (Point){ drawn_when_culled(d) ? "front face drawn" : "back face culled", d->x + 45, d->y + 90,
			drawn_when_culled(d) ? d->argb : CLEAR_ARGB, 0 };
	}
	const uint32_t scissor_colour = draws[14].argb;
	const uint32_t last = scissor_size - 1;
	points[n++] = (Point){ "scissor: top left inside", scissor_x, scissor_y, scissor_colour, 0 };
	points[n++] = (Point){ "scissor: bottom right inside", scissor_x + last, scissor_y + last, scissor_colour, 0 };
	points[n++] = (Point){ "scissor: left of it", scissor_x - 1, scissor_y, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "scissor: below it", scissor_x + last, scissor_y + scissor_size, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "scissor: right of it", scissor_x + scissor_size, scissor_y + last, CLEAR_ARGB, 0 };
	const Draw *const v = &draws[15];
	points[n++] = (Point){ "viewport: inside its top left", v->x + 2, v->y + 2, v->argb, 0 };
	points[n++] = (Point){ "viewport: inside its bottom right", v->x + v->width - 3, v->y + v->height - 3, v->argb, 0 };
	points[n++] = (Point){ "viewport: left of it", v->x - 3, v->y + 70, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "viewport: below it", v->x + 70, v->y + v->height + 2, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "depth bias: unbiased, hidden", 1040 + 50, 420 + 90, draws[16].argb, 0 };
	points[n++] = (Point){ "depth bias: biased, shown", 1140 + 50, 420 + 90, draws[18].argb, 0 };
	for (uint32_t i = 19; i < 23; i += 2) {
		const Draw *const base = &draws[i];
		const Draw *const top = &draws[i + 1];
		points[n++] = (Point){ "constant blend's base alone", base->x + 50, base->y + 40, base->argb, 0 };
		points[n++] = (Point){ "constant blend", top->x + 50, top->y + 40, blended(top->blend, top->argb, base->argb),
			BLEND_TOLERANCE };
	}
	return n;
}

// The clip-space x of a pixel column's left edge and y of a row's top, as set_draw_constants places them.
static float clip_x(uint32_t column)
{
	return (column - 0.5f) / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(uint32_t row)
{
	return 1.0f - (row - 0.5f) / (HEIGHT / 2.0f);
}

// Plane 0 keeps the columns from CLIP_COLUMN on; planes 3 and 5 the rows above CLIP_QUARTER_ROW and the columns left
// of CLIP_QUARTER_COLUMN. dot(position, plane) >= 0 is kept, and the position's w is 1.
static void clip_planes(float planes[XSDK_CLIP_PLANES][4])
{
	const float keep[XSDK_CLIP_PLANES][4] = {
		[0] = { 1.0f, 0.0f, 0.0f, -clip_x(CLIP_COLUMN) },
		[3] = { 0.0f, 1.0f, 0.0f, -clip_y(CLIP_QUARTER_ROW) },
		[5] = { -1.0f, 0.0f, 0.0f, clip_x(CLIP_QUARTER_COLUMN) },
	};
	memcpy(planes, keep, sizeof keep);
}

static uint32_t clip_points_expected(Point *points)
{
	const uint32_t first = clip_draws[0].argb;
	const uint32_t quarter = clip_draws[1].argb;
	const uint32_t after = clip_draws[2].argb;
	const uint32_t middle = CLIP_Y + CLIP_HEIGHT / 2;
	const Point clipped[] = {
		{ "clip plane 0: the cell's left cut", 100, middle, CLEAR_ARGB, 0 },
		{ "clip plane 0: just left of its line", CLIP_COLUMN - 4, middle, CLEAR_ARGB, 0 },
		{ "clip plane 0: just right of its line", CLIP_COLUMN + 4, middle, first, 0 },
		{ "clip plane 0: the cell's right kept", 200, middle, first, 0 },
		{ "clip planes 3 and 5: the top left kept", 340, CLIP_QUARTER_ROW - 20, quarter, 0 },
		{ "clip planes 3 and 5: the top right cut", 440, CLIP_QUARTER_ROW - 20, CLEAR_ARGB, 0 },
		{ "clip planes 3 and 5: the bottom left cut", 340, CLIP_QUARTER_ROW + 20, CLEAR_ARGB, 0 },
		{ "clip planes 3 and 5: the bottom right cut", 440, CLIP_QUARTER_ROW + 20, CLEAR_ARGB, 0 },
		{ "clip planes off: the next cell's top left", 560, CLIP_Y + 10, after, 0 },
		{ "clip planes off: the next cell's bottom right", 720, CLIP_Y + CLIP_HEIGHT - 10, after, 0 },
	};
	memcpy(points, clipped, sizeof clipped);
	return sizeof clipped / sizeof clipped[0];
}

// gl_PointCoord's colour at a pixel of a sprite centred on the corner at (x - 0.5, SPRITE_Y - 0.5): from 0 at its left
// and top edges to 1 at its right and bottom, at the pixel's centre.
static uint32_t sprite_argb(const Sprite *s, int32_t dx, int32_t dy)
{
	const float u = (dx + 0.5f + s->size / 2.0f) / s->size;
	const float v = (dy + 0.5f + s->size / 2.0f) / s->size;
	return 0xFF000000 | to_byte(u) << 16 | to_byte(v) << 8;
}

static uint32_t sprite_points_expected(Point *points)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < sizeof sprites / sizeof sprites[0]; i++) {
		const Sprite *const s = &sprites[i];
		const int32_t in = (int32_t)(s->size / 2.0f) - 4;
		const int32_t out = (int32_t)(s->size / 2.0f) + 3;
		const Point sprite[] = {
			{ "point sprite: its top left", s->x - in, SPRITE_Y - in, sprite_argb(s, -in, -in), SPRITE_TOLERANCE },
			{ "point sprite: its top right", s->x + in, SPRITE_Y - in, sprite_argb(s, in, -in), SPRITE_TOLERANCE },
			{ "point sprite: its bottom left", s->x - in, SPRITE_Y + in, sprite_argb(s, -in, in), SPRITE_TOLERANCE },
			{ "point sprite: its bottom right", s->x + in, SPRITE_Y + in, sprite_argb(s, in, in), SPRITE_TOLERANCE },
			{ "point sprite: left of it", s->x - out, SPRITE_Y, CLEAR_ARGB, 0 },
			{ "point sprite: right of it", s->x + out, SPRITE_Y, CLEAR_ARGB, 0 },
			{ "point sprite: above it", s->x, SPRITE_Y - out, CLEAR_ARGB, 0 },
			{ "point sprite: below it", s->x, SPRITE_Y + out, CLEAR_ARGB, 0 },
		};
		memcpy(points + n, sprite, sizeof sprite);
		n += sizeof sprite / sizeof sprite[0];
	}
	return n;
}

// The square's first triangle is its top left, top right and bottom left, so the triangles share the diagonal from the
// bottom left to the top right, which crosses the quad's centre.
static uint32_t fill_points_expected(FillPoint *points)
{
	const uint32_t wire = fill_draws[0].argb;
	const uint32_t dots = fill_draws[1].argb;
	const uint32_t left = fill_draws[0].x;
	const uint32_t corner = fill_draws[1].x;
	const uint32_t bottom = FILL_Y + FILL_HEIGHT;
	const FillPoint fills[] = {
		{ "wireframe: the top edge", left + FILL_WIDTH / 2, FILL_Y, wire, false },
		{ "wireframe: the bottom edge", left + FILL_WIDTH / 2, bottom, wire, false },
		{ "wireframe: the left edge", left, FILL_Y + FILL_HEIGHT / 2, wire, false },
		{ "wireframe: the right edge", left + FILL_WIDTH, FILL_Y + FILL_HEIGHT / 2, wire, false },
		{ "wireframe: the diagonal", left + FILL_WIDTH / 2, FILL_Y + FILL_HEIGHT / 2, wire, false },
		{ "wireframe: inside, above the diagonal", left + 30, FILL_Y + 15, wire, true },
		{ "wireframe: inside, below the diagonal", left + FILL_WIDTH - 30, FILL_Y + FILL_HEIGHT - 15, wire, true },
		{ "point fill: the top left corner", corner, FILL_Y, dots, false },
		{ "point fill: the top right corner", corner + FILL_WIDTH, FILL_Y, dots, false },
		{ "point fill: the bottom left corner", corner, bottom, dots, false },
		{ "point fill: the bottom right corner", corner + FILL_WIDTH, bottom, dots, false },
		{ "point fill: nothing along the top edge", corner + FILL_WIDTH / 2, FILL_Y, dots, true },
		{ "point fill: nothing on the diagonal", corner + FILL_WIDTH / 2, FILL_Y + FILL_HEIGHT / 2, dots, true },
	};
	memcpy(points, fills, sizeof fills);
	return sizeof fills / sizeof fills[0];
}

// Places the unit square, edges half a pixel early, or over all of clip space for the viewport draw.
static void set_draw_constants(const Draw *d)
{
	float left = (d->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	float right = (d->x + d->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	float top = 1.0f - (d->y - 0.5f) / (HEIGHT / 2.0f);
	float bottom = 1.0f - (d->y + d->height - 0.5f) / (HEIGHT / 2.0f);
	if (d->viewport) {
		left = -1.0f;
		right = 1.0f;
		top = 1.0f;
		bottom = -1.0f;
	}
	if (d->mirrored) {
		const float swap = left;
		left = right;
		right = swap;
	}
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, 0.5f, 0.0f },
		{ channel(d->argb, 16), channel(d->argb, 8), channel(d->argb, 0), channel(d->argb, 24) },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
}

static void draw_all(const States *states)
{
	for (uint32_t i = 0; i < sizeof draws / sizeof draws[0]; i++) {
		const Draw *const d = &draws[i];
		xsdk_gpu_set_blend(&states->blends[d->blend]);
		if (d->blend == BLEND_CONSTANT_COLOR || d->blend == BLEND_CONSTANT_ALPHA)
			xsdk_gpu_set_blend_constants(d->blend == BLEND_CONSTANT_COLOR ? constant_color : constant_alpha);
		xsdk_gpu_set_raster(&states->rasters[d->raster]);
		xsdk_gpu_set_depth_stencil(d->depth_test ? &states->depth_less : &states->depth_off);
		if (d->scissor)
			xsdk_gpu_set_scissor(scissor_x, scissor_y, scissor_size, scissor_size);
		if (d->viewport)
			xsdk_gpu_set_viewport(d->x, d->y, d->width, d->height, 0.0f, 1.0f);
		set_draw_constants(d);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		if (d->scissor)
			xsdk_gpu_set_scissor(0, 0, WIDTH, HEIGHT);
		if (d->viewport)
			xsdk_gpu_set_viewport(0.0f, 0.0f, WIDTH, HEIGHT, 0.0f, 1.0f);
	}
}

// The fill mode row, each quad's corners on pixels' centres, so its lines and points land on whole pixels.
static void draw_fills(const States *states)
{
	xsdk_gpu_set_blend(&states->blends[BLEND_NONE]);
	xsdk_gpu_set_depth_stencil(&states->depth_off);
	for (uint32_t i = 0; i < sizeof fill_draws / sizeof fill_draws[0]; i++) {
		const FillDraw *const f = &fill_draws[i];
		const float left = clip_x(f->x) + 0.5f / (WIDTH / 2.0f);
		const float top = clip_y(FILL_Y) - 0.5f / (HEIGHT / 2.0f);
		// Negated as a float: CLEAR_ARGB is past INT_MAX, so the enum's constants are unsigned.
		const float vertex[3][4] = {
			{ FILL_WIDTH / (WIDTH / 2.0f), -(FILL_HEIGHT / (HEIGHT / 2.0f)), 1.0f, 1.0f },
			{ left, top, 0.5f, 0.0f },
			{ channel(f->argb, 16), channel(f->argb, 8), channel(f->argb, 0), 1.0f },
		};
		xsdk_gpu_set_raster(&states->rasters[f->raster]);
		xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

// Whether a resolved pixel, its bytes B, G, R and A, is within a step or two of the colour.
static bool colour_near(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	bool near = true;
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		near &= (got > wanted ? got - wanted : wanted - got) <= BLEND_TOLERANCE;
	}
	return near;
}

static bool fill_holds(volatile const uint32_t *pixels, const FillPoint *p, uint32_t frame, uint32_t *misses)
{
	bool found = false;
	for (uint32_t y = p->y - FILL_SLACK; y <= p->y + FILL_SLACK; y++) {
		for (uint32_t x = p->x - FILL_SLACK; x <= p->x + FILL_SLACK; x++)
			found |= colour_near(pixels[tiled_index(x, y, WIDTH)], p->argb);
	}
	const bool holds = found != p->absent;
	if (!holds && *misses < MAX_MISSES_LOGGED)
		xsdk_log_write("raster: frame %u: %s at (%u, %u)%s", (unsigned)frame, p->what, (unsigned)p->x, (unsigned)p->y,
			p->absent ? ": drawn" : ": not drawn");
	*misses += !holds;
	return holds;
}

// The clip row, each quad with its planes, then every plane off for what's drawn after.
static void draw_clipped(const States *states)
{
	float planes[XSDK_CLIP_PLANES][4];
	clip_planes(planes);
	xsdk_gpu_set_blend(&states->blends[BLEND_NONE]);
	xsdk_gpu_set_raster(&states->rasters[RASTER_PLAIN]);
	xsdk_gpu_set_depth_stencil(&states->depth_off);
	for (uint32_t i = 0; i < sizeof clip_draws / sizeof clip_draws[0]; i++) {
		const ClipDraw *const c = &clip_draws[i];
		const Draw d = { c->x, CLIP_Y, CELL_WIDTH, CLIP_HEIGHT, c->argb, BLEND_NONE, RASTER_PLAIN, false, false, false,
			false };
		xsdk_gpu_set_clip_planes(c->mask, planes);
		set_draw_constants(&d);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

// What vk_point_sprite.vert and vk_point_coord.frag draw with: their shaders, and the point's vertex.
typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	XsdkXscShader vertex_compiled;
	XsdkXscShader pixel_compiled;
	const XsdkXscNeed *vertex_needs;
	size_t vertex_need_count;
	const XsdkXscNeed *pixel_needs;
	size_t pixel_need_count;
	volatile uint32_t *memory;
	uint32_t point;
} Sprites;

static void set_literals(const XsdkXscNeed *needs, size_t count, bool vertex)
{
	for (size_t i = 0; i < count; i++) {
		const float values[4] = { (float)needs[i].value[0], (float)needs[i].value[1], (float)needs[i].value[2],
			(float)needs[i].value[3] };
		if (needs[i].kind != XSDK_XSC_LITERAL)
			continue;
		if (vertex)
			xsdk_gpu_set_vertex_constants(needs[i].reg, values, 1);
		else
			xsdk_gpu_set_pixel_constants(needs[i].reg, values, 1);
	}
}

// Each sprite's place, its centre on a pixel's corner, in c0, and its size in c1.x, the push block's registers.
static void draw_sprites(const Sprites *s)
{
	xsdk_gpu_set_shaders(&s->vertex, &s->pixel);
	xsdk_gpu_set_vertices(0, s->point, 4 * sizeof(float));
	set_literals(s->vertex_needs, s->vertex_need_count, true);
	set_literals(s->pixel_needs, s->pixel_need_count, false);
	for (uint32_t i = 0; i < sizeof sprites / sizeof sprites[0]; i++) {
		const float constants[2][4] = {
			{ clip_x(sprites[i].x), clip_y(SPRITE_Y), 0.0f, 0.0f },
			{ sprites[i].size, 0.0f, 0.0f, 0.0f },
		};
		xsdk_gpu_set_vertex_constants(0, &constants[0][0], 2);
		xsdk_gpu_draw(XSDK_PRIMITIVE_POINT_LIST, 1);
	}
}

// A grid of quads INDEXED_SIZE pixels square, columns wide from x and y, and the colour each one's centre should hold,
// or each of its pixels if whole.
typedef struct {
	const char *what;
	uint32_t x;
	uint32_t y;
	uint32_t columns;
	uint32_t quads;
	const uint32_t *argb;
	bool whole;
} Grid;

// What the indexed grid draws with: vk_dynamic_interpolator.vert, its block's register and rows, and the quads.
typedef struct {
	XsdkShader vertex;
	XsdkXscShader compiled;
	const XsdkXscNeed *needs;
	size_t need_count;
	uint32_t block;
	float rows[2 * INDEXED_INDICES][4];
	volatile uint32_t *memory;
	uint32_t vertices;
	uint32_t argb[INDEXED_QUADS];
	Grid grid;
} Indexed;

// What a pixel shader's grid draws with: the shader, its block's register and rows, and its quads' colours.
typedef struct {
	XsdkShader pixel;
	XsdkXscShader compiled;
	const XsdkXscNeed *needs;
	size_t need_count;
	uint32_t block;
	float rows[UNIFORM_IF_ROWS][4];
	uint32_t vertices;
	uint32_t argb[PIXEL_GRID_QUADS];
	Grid grid;
} PixelGrid;

// What vk_slow_fetch.frag's grid draws with: the shader, its texture and fetch constant, its quads and the grid.
typedef struct {
	XsdkShader pixel;
	XsdkXscShader compiled;
	const XsdkXscNeed *needs;
	size_t need_count;
	XsdkTexture texture;
	XsdkSamplerState sampler;
	uint32_t slot;
	volatile uint32_t *memory;
	uint32_t vertices;
	uint32_t argb[SLOW_QUADS];
	Grid grid;
} Slow;

// What a loop grid draws with: the shader, its place, its quad and the colour of each place in a cell; for
// vk_loop_count_grid.frag, its push block's register and its loop constant, and a quad over the whole target after the
// grid's.
typedef struct {
	XsdkShader pixel;
	XsdkXscShader compiled;
	const XsdkXscNeed *needs;
	size_t need_count;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	volatile uint32_t *memory;
	uint32_t vertices;
	uint32_t argb[LOOP_CELL][LOOP_CELL];
	uint32_t block;
	uint32_t loop_constant;
} LoopGrid;

// The pixel shaders' grids: triangle_vertex_shader passing each quad's indices on, both grids' quads, the texels
// vk_dynamic_texcoord.frag samples and their fetch constant, and each grid.
typedef struct {
	XsdkShader vertex;
	volatile uint32_t *memory;
	XsdkTexture texture;
	XsdkSamplerState sampler;
	uint32_t slot;
	PixelGrid texcoord;
	PixelGrid uniform_if;
	Slow slow;
	LoopGrid loop;
	LoopGrid count;
} PixelGrids;

// vk_loop_count_grid.frag's place: each pixel's column and row in its cell, or every pixel leaving at the first pass.
static const float count_grid_place[4] = { 1.0f, 1.0f, 0.0f, 0.0f };
static const float first_pass_place[4] = { 0.0f, 0.0f, 9.0f, 0.0f };

// Index i's colour: red 16i + 8, green its complement and blue it xor 0x5A, so a pixel names the row read.
static uint32_t indexed_argb(uint32_t i)
{
	const uint32_t n = 16 * i + 8;
	return 0xFF000000 | n << 16 | (255 - n) << 8 | (n ^ 0x5A);
}

static void draw_indexed(const Indexed *r, const XsdkShader *pixel)
{
	xsdk_gpu_set_shaders(&r->vertex, pixel);
	xsdk_gpu_set_vertices(0, r->vertices, INDEXED_VERTICES * STRIDE_BYTES);
	set_literals(r->needs, r->need_count, true);
	xsdk_gpu_set_vertex_constants(r->block, &r->rows[0][0], 2 * INDEXED_INDICES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, INDEXED_VERTICES);
}

// The grid's quads count from first.
static void draw_quads(const PixelGrids *p, const PixelGrid *g, uint32_t first, uint32_t count)
{
	const uint32_t quad_bytes = QUAD_VERTICES * PIXEL_STRIDE_BYTES;
	xsdk_gpu_set_shaders(&p->vertex, &g->pixel);
	xsdk_gpu_set_vertices(0, g->vertices + first * quad_bytes, count * quad_bytes);
	set_literals(g->needs, g->need_count, false);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, count * QUAD_VERTICES);
}

// A loop grid's quad, or the quad over the whole target after it; vk_loop_count_grid.frag's place and its count first.
static void draw_loop(const PixelGrids *p, const LoopGrid *l, uint32_t quad, const float place[4], uint32_t passes)
{
	const uint32_t quad_bytes = QUAD_VERTICES * PIXEL_STRIDE_BYTES;
	xsdk_gpu_set_shaders(&p->vertex, &l->pixel);
	xsdk_gpu_set_vertices(0, l->vertices + quad * quad_bytes, quad_bytes);
	set_literals(l->needs, l->need_count, false);
	if (place) {
		const uint32_t loop = xsdk_gpu_loop_constant(passes, 0, 1);
		xsdk_gpu_set_pixel_constants(l->block, place, 1);
		xsdk_gpu_set_loop_constants(l->loop_constant, &loop, 1);
	}
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// vk_dynamic_texcoord.frag in one draw; vk_dynamic_if.frag in two, its arm reading at j taken in the first, over the
// grid's top half, and not in the second; vk_slow_fetch.frag in one; vk_loop_grid.frag and vk_loop_count_grid.frag in
// one each.
static void draw_pixel_grids(const PixelGrids *p)
{
	const PixelGrid *const t = &p->texcoord;
	const PixelGrid *const u = &p->uniform_if;
	const Slow *const s = &p->slow;
	const float not_taken[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
	xsdk_gpu_set_texture(p->slot, &p->texture, &p->sampler);
	xsdk_gpu_set_pixel_constants(t->block, &t->rows[0][0], TEXCOORD_ROWS);
	draw_quads(p, t, 0, PIXEL_GRID_QUADS);
	xsdk_gpu_set_pixel_constants(u->block, &u->rows[0][0], UNIFORM_IF_ROWS);
	draw_quads(p, u, 0, ARM_TAKEN_QUADS);
	xsdk_gpu_set_pixel_constants(u->block + MODE_ROW, not_taken, 1);
	draw_quads(p, u, ARM_TAKEN_QUADS, PIXEL_GRID_QUADS - ARM_TAKEN_QUADS);
	xsdk_gpu_set_texture(s->slot, &s->texture, &s->sampler);
	xsdk_gpu_set_shaders(&p->vertex, &s->pixel);
	xsdk_gpu_set_vertices(0, s->vertices, SLOW_VERTICES * PIXEL_STRIDE_BYTES);
	set_literals(s->needs, s->need_count, false);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, SLOW_VERTICES);
	draw_loop(p, &p->loop, 0, NULL, 0);
	draw_loop(p, &p->count, 0, count_grid_place, COUNT_PASSES);
}

// The pixels a grid checks a quad, its centre or all of them.
static uint32_t grid_side(const Grid *g)
{
	return g->whole ? INDEXED_SIZE : 1;
}

// The grid's quads whose centre, or pixels whose colour, isn't their quad's, the first few logged.
static uint32_t grid_wrong(const Grid *g, volatile const uint32_t *pixels, uint32_t frame, uint32_t *logged)
{
	const uint32_t side = grid_side(g);
	const uint32_t first = g->whole ? 0 : INDEXED_SIZE / 2;
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < g->quads * side * side; i++) {
		const uint32_t quad = i / (side * side);
		const uint32_t column = quad % g->columns;
		const uint32_t row = quad / g->columns;
		const uint32_t x = g->x + column * INDEXED_SIZE + first + i % side;
		const uint32_t y = g->y + row * INDEXED_SIZE + first + i / side % side;
		const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
		if (colour_near(stored, g->argb[quad]))
			continue;
		if (*logged < MAX_INDEXED_LOGGED) {
			xsdk_log_write(
				"raster: frame %u: the %s quad at column %u, row %u, index %u, held %08X at (%u, %u), not %08X",
				(unsigned)frame, g->what, (unsigned)column, (unsigned)row, (unsigned)(quad % INDEXED_INDICES),
				(unsigned)stored, (unsigned)x, (unsigned)y, (unsigned)__builtin_bswap32(g->argb[quad]));
			(*logged)++;
		}
		wrong++;
	}
	return wrong;
}

// A loop grid's pixels whose colour isn't their place's, the first few logged.
static uint32_t loop_wrong(const LoopGrid *g, volatile const uint32_t *pixels, uint32_t frame, uint32_t *logged)
{
	uint32_t wrong = 0;
	for (uint32_t y = g->y; y < g->y + g->height; y++) {
		for (uint32_t x = g->x; x < g->x + g->width; x++) {
			const uint32_t want = g->argb[y % LOOP_CELL][x % LOOP_CELL];
			const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
			if (colour_near(stored, want))
				continue;
			if (*logged < MAX_INDEXED_LOGGED) {
				xsdk_log_write(
					"raster: frame %u: a loop grid's pixel at (%u, %u), column %u and row %u of its cell, "
					"held %08X, not %08X",
					(unsigned)frame, (unsigned)x, (unsigned)y, (unsigned)(x % LOOP_CELL), (unsigned)(y % LOOP_CELL),
					(unsigned)stored, (unsigned)__builtin_bswap32(want));
				(*logged)++;
			}
			wrong++;
		}
	}
	return wrong;
}

static bool point_holds(volatile const uint32_t *pixels, const Point *p, uint32_t frame, uint32_t *misses)
{
	const uint32_t stored = pixels[tiled_index(p->x, p->y, WIDTH)];
	const uint32_t want = __builtin_bswap32(p->argb);
	bool holds = true;
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		holds &= (got > wanted ? got - wanted : wanted - got) <= p->tolerance;
	}
	if (!holds && *misses < MAX_MISSES_LOGGED)
		xsdk_log_write("raster: frame %u: %s at (%u, %u) held %08X, not %08X", (unsigned)frame, p->what, (unsigned)p->x,
			(unsigned)p->y, (unsigned)stored, (unsigned)want);
	*misses += !holds;
	return holds;
}

// Each frame draws every cell into eDRAM, resolves into the buffer not on screen, and shows it.
static void draw_frames(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *const *buffers, bool has_display, const Sprites *s, const Indexed *r, const PixelGrids *p)
{
	const Grid *const grids[GRIDS] = { r ? &r->grid : NULL, p ? &p->texcoord.grid : NULL,
		p ? &p->uniform_if.grid : NULL, p ? &p->slow.grid : NULL };
	uint32_t quads_checked[GRIDS] = { 0 };
	uint32_t quads_wrong[GRIDS] = { 0 };
	uint32_t quads_logged[GRIDS] = { 0 };
	uint32_t loop_checked = 0;
	uint32_t loop_misses = 0;
	uint32_t loop_logged = 0;
	uint32_t count_checked = 0;
	uint32_t count_misses = 0;
	uint32_t count_logged = 0;
	States states;
	make_states(&states);
	// The depth bias cell needs depth cleared to 1.0, which a float rounding once turned into 0.
	xsdk_gpu_set_depth_clear(1.0f, 0);
	Point points[POINTS];
	const uint32_t point_count = points_expected(points);
	Point clip_points[POINTS];
	const uint32_t clip_point_count = clip_points_expected(clip_points);
	Point sprite_points[POINTS];
	const uint32_t sprite_point_count = sprite_points_expected(sprite_points);
	FillPoint fill_points[POINTS];
	const uint32_t fill_point_count = fill_points_expected(fill_points);
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	uint32_t clip_frames_wrong = 0;
	uint32_t sprite_frames_wrong = 0;
	uint32_t fill_frames_wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(0, vertices, sizeof square);
		draw_all(&states);
		draw_fills(&states);
		draw_clipped(&states);
		if (s)
			draw_sprites(s);
		if (r)
			draw_indexed(r, pixel);
		if (p)
			draw_pixel_grids(p);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("raster: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's colour and depth are whatever eDRAM held; later ones are the clear values.
		if (frame > 0 && fences_landed) {
			bool holds = true;
			for (uint32_t i = 0; i < point_count; i++)
				holds &= point_holds(buffer, &points[i], frame, &misses);
			frames_wrong += !holds;
			bool clipped = true;
			for (uint32_t i = 0; i < clip_point_count; i++)
				clipped &= point_holds(buffer, &clip_points[i], frame, &misses);
			clip_frames_wrong += !clipped;
			bool sprited = s != NULL;
			for (uint32_t i = 0; s && i < sprite_point_count; i++)
				sprited &= point_holds(buffer, &sprite_points[i], frame, &misses);
			sprite_frames_wrong += !sprited;
			bool filled = true;
			for (uint32_t i = 0; i < fill_point_count; i++)
				filled &= fill_holds(buffer, &fill_points[i], frame, &misses);
			fill_frames_wrong += !filled;
			for (uint32_t g = 0; g < GRIDS; g++) {
				if (!grids[g])
					continue;
				quads_wrong[g] += grid_wrong(grids[g], buffer, frame, &quads_logged[g]);
				quads_checked[g] += grids[g]->quads * grid_side(grids[g]) * grid_side(grids[g]);
			}
			if (p) {
				loop_misses += loop_wrong(&p->loop, buffer, frame, &loop_logged);
				loop_checked += LOOP_WIDTH * LOOP_HEIGHT;
				count_misses += loop_wrong(&p->count, buffer, frame, &count_logged);
				count_checked += COUNT_WIDTH * COUNT_HEIGHT;
			}
		}
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write(
		"raster: %u points a frame, %u of the clip planes; %u and %u of %u frames checked held a wrong one, "
		"%u points wrong",
		(unsigned)point_count, (unsigned)clip_point_count, (unsigned)frames_wrong, (unsigned)clip_frames_wrong,
		(unsigned)(FRAMES - 1), (unsigned)misses);
	check("raster: every frame's fence landed", fences_landed);
	check("raster: blends, constant blends, write mask, culling, scissor, viewport and depth bias as worked out",
		fences_landed && frames_wrong == 0);
	check(
		"raster: a wireframe quad draws its triangles' edges and nothing inside, a point-filled one its corners alone",
		fences_landed && fill_frames_wrong == 0);
	check("raster: clip planes 0, 3 and 5 cut their cells at their lines, and nothing once they're off",
		fences_landed && clip_frames_wrong == 0);
	check(
		"raster: point sprites from xost's shaders compiled on the console as wide as their gl_PointSize, 24 and 64 "
		"pixels, gl_PointCoord across them from the top left",
		fences_landed && sprite_frames_wrong == 0);
	for (uint32_t g = 0; g < GRIDS; g++) {
		char name[256];
		(void)snprintf(name, sizeof name, "%s: %u of %u wrong", grid_checks[g], (unsigned)quads_wrong[g],
			(unsigned)quads_checked[g]);
		check(name, fences_landed && quads_checked[g] && quads_wrong[g] == 0);
	}
	char name[320];
	(void)snprintf(name, sizeof name,
		"raster: one draw of xost's vk_loop_grid.frag compiled on the console colours every pixel of 80 cells of 10 by "
		"10 as it leaves an unrolled loop at its own pass, on breaks on data in arms of an if/else, a continue on data "
		"and a break the unroller folds: %u of %u wrong",
		(unsigned)loop_misses, (unsigned)loop_checked);
	check(name, fences_landed && loop_checked && loop_misses == 0);
	(void)snprintf(name, sizeof name,
		"raster: one draw of xost's vk_loop_count_grid.frag compiled on the console colours every pixel of 112 cells "
		"of 10 by 10 as it leaves a hardware loop at its own pass, on a continue and two breaks on data, and its "
		"counter after the loop: %u of %u wrong",
		(unsigned)count_misses, (unsigned)count_checked);
	check(name, fences_landed && count_checked && count_misses == 0);
	if (has_display) {
		check("raster: every frame shown", shown);
		xsdk_log_write("raster: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

static const XscTestShader *xsc_shader(const char *name)
{
	for (size_t i = 0; i < sizeof xsc_test_shaders / sizeof xsc_test_shaders[0]; i++) {
		if (!strcmp(xsc_test_shaders[i].name, name))
			return &xsc_test_shaders[i];
	}
	return NULL;
}

// xost's compiler on the console, as the layer compiles what its bundle lacks (tests/test_xsc.c).
static bool compile_shader(
	const char *name, XsdkShader *shader, XsdkXscShader *compiled, const XsdkXscNeed **needs, size_t *need_count)
{
	const XscTestShader *const source = xsc_shader(name);
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	if (!source || xsdk_xsc_compile(source->spirv, source->spirv_words, &options, compiled) != XSDK_XSC_OK)
		return false;
	*needs = source->needs;
	*need_count = source->need_count;
	return NT_SUCCESS(xsdk_shader_load(shader, (const uint32_t *)compiled->container, compiled->container_bytes));
}

// The sprites' shaders and the point they draw, moved by their constants; false, the sprites undrawn, on a failure.
static bool make_sprites(Sprites *s)
{
	static const float point[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
	const XsdkVertexElement position = { .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 };
	if (!compile_shader(
			"vk_point_sprite.vert", &s->vertex, &s->vertex_compiled, &s->vertex_needs, &s->vertex_need_count) ||
		!compile_shader("vk_point_coord.frag", &s->pixel, &s->pixel_compiled, &s->pixel_needs, &s->pixel_need_count) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&s->vertex, &position, 1, (const uint32_t[]){ sizeof point })))
		return false;
	s->memory = xsdk_gpu_alloc(sizeof point);
	if (!s->memory)
		return false;
	uint32_t words[4];
	memcpy(words, point, sizeof point);
	for (uint32_t i = 0; i < 4; i++)
		s->memory[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	s->point = xsdk_gpu_address(s->memory);
	xsdk_gpu_invalidate(s->point, sizeof point);
	return true;
}

// vk_dynamic_interpolator.vert, its block's rows, scales 1 and each index's colour, and the grid's quads, each placed
// as set_draw_constants places the square, its index in z; false, the grid undrawn, on a failure.
static bool make_indexed(Indexed *r)
{
	if (!compile_shader("vk_dynamic_interpolator.vert", &r->vertex, &r->compiled, &r->needs, &r->need_count) ||
		r->compiled.resource_count != 1 ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&r->vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	r->block = r->compiled.resources[0].reg;
	for (uint32_t i = 0; i < INDEXED_INDICES; i++) {
		const uint32_t argb = indexed_argb(i);
		const float colour[4] = { channel(argb, 16), channel(argb, 8), channel(argb, 0), channel(argb, 24) };
		for (uint32_t c = 0; c < 4; c++) {
			r->rows[i][c] = 1.0f;
			r->rows[INDEXED_INDICES + i][c] = colour[c];
		}
	}
	r->memory = xsdk_gpu_alloc(INDEXED_VERTICES * STRIDE_BYTES);
	if (!r->memory)
		return false;
	for (uint32_t v = 0; v < INDEXED_VERTICES; v++) {
		const uint32_t quad = v / QUAD_VERTICES;
		const float *const corner = square[v % QUAD_VERTICES];
		const uint32_t x = INDEXED_X + quad % INDEXED_COLUMNS * INDEXED_SIZE + (corner[0] ? INDEXED_SIZE : 0);
		const uint32_t y = INDEXED_Y + quad / INDEXED_COLUMNS * INDEXED_SIZE + (corner[1] ? INDEXED_SIZE : 0);
		const float vertex[VERTEX_FLOATS] = { clip_x(x), clip_y(y), (float)(quad % INDEXED_INDICES), 0.0f };
		uint32_t words[VERTEX_FLOATS];
		memcpy(words, vertex, sizeof words);
		for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
			r->memory[v * VERTEX_FLOATS + i] = words[i];
	}
	__asm__ volatile("sync" ::: "memory");
	r->vertices = xsdk_gpu_address(r->memory);
	xsdk_gpu_invalidate(r->vertices, INDEXED_VERTICES * STRIDE_BYTES);
	for (uint32_t quad = 0; quad < INDEXED_QUADS; quad++)
		r->argb[quad] = indexed_argb(quad % INDEXED_INDICES);
	r->grid = (Grid){ "indexed", INDEXED_X, INDEXED_Y, INDEXED_COLUMNS, INDEXED_QUADS, r->argb, false };
	return true;
}

// A pixel grid's quad's indices: i 0 to 15 along the grid, and j another index, never i, a step on every 16 quads.
static uint32_t index_i(uint32_t quad)
{
	return quad % INDEXED_INDICES;
}

static uint32_t index_j(uint32_t quad)
{
	return (index_i(quad) + 1 + quad / INDEXED_INDICES % (INDEXED_INDICES - 1)) % INDEXED_INDICES;
}

// The pixel grids' quads from x, placed as the indexed grid's, i and j a half past whole so int() takes them however
// they interpolate.
static void write_pixel_grid(volatile uint32_t *memory, uint32_t x0)
{
	for (uint32_t v = 0; v < PIXEL_GRID_VERTICES; v++) {
		const uint32_t quad = v / QUAD_VERTICES;
		const float *const corner = square[v % QUAD_VERTICES];
		const uint32_t x = x0 + quad % PIXEL_GRID_COLUMNS * INDEXED_SIZE + (corner[0] ? INDEXED_SIZE : 0);
		const uint32_t y = PIXEL_GRID_Y + quad / PIXEL_GRID_COLUMNS * INDEXED_SIZE + (corner[1] ? INDEXED_SIZE : 0);
		const float vertex[PIXEL_VERTEX_FLOATS] = { clip_x(x), clip_y(y), 0.0f, 1.0f, (float)index_i(quad) + 0.5f,
			(float)index_j(quad) + 0.5f, 0.0f, 0.0f };
		uint32_t words[PIXEL_VERTEX_FLOATS];
		memcpy(words, vertex, sizeof words);
		for (uint32_t i = 0; i < PIXEL_VERTEX_FLOATS; i++)
			memory[v * PIXEL_VERTEX_FLOATS + i] = words[i];
	}
}

// A pixel shader compiled on the console with one block, its grid's quads from x, and the grid.
static bool make_pixel_grid(PixelGrid *g, const char *name, const char *what, uint32_t x, uint32_t vertices)
{
	if (!compile_shader(name, &g->pixel, &g->compiled, &g->needs, &g->need_count) || !g->compiled.resource_count ||
		g->compiled.resources[0].kind != XSDK_XSC_UNIFORM_BUFFER)
		return false;
	g->block = g->compiled.resources[0].reg;
	g->vertices = vertices;
	g->grid = (Grid){ what, x, PIXEL_GRID_Y, PIXEL_GRID_COLUMNS, PIXEL_GRID_QUADS, g->argb, false };
	return true;
}

// Its texels, 4 by 4 blocks, block n index n's colour, and its rows: block i's centre in coords[i], and scales 1.
static bool make_texcoord(PixelGrids *p)
{
	PixelGrid *const g = &p->texcoord;
	const XsdkXscResource *const sampler = &g->compiled.resources[1];
	if (g->compiled.resource_count != 2 || sampler->kind != XSDK_XSC_SAMPLER ||
		!NT_SUCCESS(xsdk_texture_create(&p->texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIDE, TEXTURE_SIDE, 1, true)))
		return false;
	uint32_t texels[TEXTURE_SIDE][TEXTURE_SIDE];
	for (uint32_t y = 0; y < TEXTURE_SIDE; y++) {
		for (uint32_t x = 0; x < TEXTURE_SIDE; x++)
			texels[y][x] = indexed_argb(y / BLOCK_SIDE * BLOCKS_SIDE + x / BLOCK_SIDE);
	}
	for (uint32_t n = 0; n < INDEXED_INDICES; n++) {
		const float centre[4] = { (n % BLOCKS_SIDE + 0.5f) / BLOCKS_SIDE, (n / BLOCKS_SIDE + 0.5f) / BLOCKS_SIDE, 0.0f,
			0.0f };
		memcpy(g->rows[n], centre, sizeof centre);
		for (uint32_t c = 0; c < 4; c++)
			g->rows[INDEXED_INDICES + n][c] = 1.0f;
	}
	xsdk_texture_upload(&p->texture, 0, &texels[0][0], sizeof texels[0]);
	xsdk_gpu_invalidate(p->texture.address, p->texture.size_bytes);
	const XsdkSampler point_clamped = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&p->sampler, &point_clamped);
	p->slot = sampler->reg;
	for (uint32_t quad = 0; quad < PIXEL_GRID_QUADS; quad++)
		g->argb[quad] = indexed_argb(index_i(quad));
	return true;
}

// vk_dynamic_if.frag's colour: colors[i] times the tint, plus colors[i] twice.
static uint32_t uniform_if_argb(const float rows[][4], uint32_t i, uint32_t tint)
{
	uint32_t argb = 0xFF000000;
	for (uint32_t c = 0; c < 3; c++) {
		const float colour = rows[i][c];
		argb |= to_byte(colour * rows[INDEXED_INDICES + tint][c] + 2.0f * colour) << (16 - 8 * c);
	}
	return argb;
}

// Its rows: colors[n] index n's colour over 4, tints[n] index 15 - n's, and the mode taking the arm, multipliers 1;
// each quad's colour, its tint at j where the arm is taken, over the grid's top half, and at i below.
static void make_uniform_if(PixelGrid *g)
{
	for (uint32_t n = 0; n < INDEXED_INDICES; n++) {
		const uint32_t colour = indexed_argb(n);
		const uint32_t tint = indexed_argb(INDEXED_INDICES - 1 - n);
		for (uint32_t c = 0; c < 3; c++) {
			g->rows[n][c] = channel(colour, 16 - 8 * c) / 4.0f;
			g->rows[INDEXED_INDICES + n][c] = channel(tint, 16 - 8 * c);
		}
		g->rows[n][3] = 1.0f;
		g->rows[INDEXED_INDICES + n][3] = 1.0f;
	}
	const float taken[4] = { 1.0f, 1.0f, 1.0f, 0.0f };
	memcpy(g->rows[MODE_ROW], taken, sizeof taken);
	for (uint32_t quad = 0; quad < PIXEL_GRID_QUADS; quad++) {
		const uint32_t i = index_i(quad);
		g->argb[quad] = uniform_if_argb(g->rows, i, quad < ARM_TAKEN_QUADS ? index_j(quad) : i);
	}
}

// vk_slow_fetch.frag's quads: each one's class, its index and a half, then how far across the texture, and how many
// steps of 16 rows down, its corners sample; its colour, its class's.
static void write_slow_quads(Slow *s)
{
	for (uint32_t v = 0; v < SLOW_VERTICES; v++) {
		const uint32_t quad = v / QUAD_VERTICES;
		const float *const corner = square[v % QUAD_VERTICES];
		const uint32_t x = SLOW_X + quad % SLOW_COLUMNS * INDEXED_SIZE + (corner[0] ? INDEXED_SIZE : 0);
		const uint32_t y = SLOW_Y + quad / SLOW_COLUMNS * INDEXED_SIZE + (corner[1] ? INDEXED_SIZE : 0);
		const uint32_t across = quad * SLOW_QUAD_COLUMNS + (corner[0] ? INDEXED_SIZE * SLOW_PIXEL_COLUMNS : 0);
		const uint32_t down = quad * SLOW_QUAD_STEPS + (corner[1] ? INDEXED_SIZE * SLOW_PIXEL_STEPS : 0);
		const float vertex[PIXEL_VERTEX_FLOATS] = { clip_x(x), clip_y(y), 0.0f, 1.0f, (float)index_i(quad) + 0.5f,
			(float)across, (float)down, 0.0f };
		uint32_t words[PIXEL_VERTEX_FLOATS];
		memcpy(words, vertex, sizeof words);
		for (uint32_t i = 0; i < PIXEL_VERTEX_FLOATS; i++)
			s->memory[v * PIXEL_VERTEX_FLOATS + i] = words[i];
	}
	for (uint32_t quad = 0; quad < SLOW_QUADS; quad++)
		s->argb[quad] = indexed_argb(index_i(quad));
}

// vk_slow_fetch.frag, its texture, each row its class's colour, row mod 16, wrapped, and its quads; false, the grid
// undrawn, on a failure.
static bool make_slow(Slow *s)
{
	if (!compile_shader("vk_slow_fetch.frag", &s->pixel, &s->compiled, &s->needs, &s->need_count) ||
		s->compiled.resource_count != 1 || s->compiled.resources[0].kind != XSDK_XSC_SAMPLER ||
		!NT_SUCCESS(xsdk_texture_create(&s->texture, XSDK_FORMAT_A8R8G8B8, SLOW_SIDE, SLOW_SIDE, 1, true)))
		return false;
	uint32_t texels[SLOW_SIDE];
	for (uint32_t y = 0; y < SLOW_SIDE; y++) {
		for (uint32_t x = 0; x < SLOW_SIDE; x++)
			texels[x] = indexed_argb(y % INDEXED_INDICES);
		xsdk_texture_upload_rows(&s->texture, 0, y, 1, texels, sizeof texels);
	}
	xsdk_gpu_invalidate(s->texture.address, s->texture.size_bytes);
	const XsdkSampler point_wrapped = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_WRAP,
		.address_v = XSDK_ADDRESS_WRAP };
	xsdk_texture_sampler_state(&s->sampler, &point_wrapped);
	s->slot = s->compiled.resources[0].reg;
	s->memory = xsdk_gpu_alloc(SLOW_VERTICES * PIXEL_STRIDE_BYTES);
	if (!s->memory)
		return false;
	write_slow_quads(s);
	__asm__ volatile("sync" ::: "memory");
	s->vertices = xsdk_gpu_address(s->memory);
	xsdk_gpu_invalidate(s->vertices, SLOW_VERTICES * PIXEL_STRIDE_BYTES);
	s->grid = (Grid){ "slow fetch", SLOW_X, SLOW_Y, SLOW_COLUMNS, SLOW_QUADS, s->argb, true };
	return true;
}

// vk_loop_grid.frag's colour at a column and row of its cell, as tests/test_loop_branches.py works it out: the column
// picks the pass it breaks out at, in arms of an if/else on data, the row the pass it continues past, and none runs
// pass 6.
static uint32_t loop_argb(uint32_t column, uint32_t row)
{
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	for (uint32_t i = 0; i < LOOP_PASSES; i++) {
		if (i == 6)
			break;
		sum[0] += 0.0625f;
		if (i == row)
			continue;
		if (column < 5) {
			sum[1] += 0.0625f;
			if (i > column)
				break;
		} else if (i + 3 > column) {
			break;
		}
		sum[2] += (float)(i + 1) * 0.03125f;
	}
	return 0xFF000000 | to_byte(sum[0]) << 16 | to_byte(sum[1]) << 8 | to_byte(sum[2]);
}

// vk_loop_count_grid.frag's colour at a column and row of its cell, as tests/test_hardware_loop_breaks.py works it out:
// the column picks the pass it continues past and the pass it breaks at in an arm, the row the pass whose end it breaks
// at, and the counter after the loop adds to red.
static uint32_t count_argb(uint32_t column, uint32_t row)
{
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	uint32_t i = 0;
	for (; i < COUNT_PASSES; i++) {
		sum[0] += 0.0625f;
		if (i + 2 == column)
			continue;
		if (column < 5) {
			sum[1] += 0.0625f;
			if (i > column)
				break;
		}
		sum[2] += (float)(i + 1) * 0.015625f;
		if (i >= row)
			break;
	}
	return 0xFF000000 | to_byte(sum[0] + (float)i * 0.03125f) << 16 | to_byte(sum[1]) << 8 | to_byte(sum[2]);
}

// Each vertex of the square from x and y, width by height, into memory.
static void write_square(volatile uint32_t *memory, uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float *const corner = square[v];
		const float vertex[PIXEL_VERTEX_FLOATS] = { clip_x(x + (corner[0] ? width : 0)),
			clip_y(y + (corner[1] ? height : 0)), 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
		uint32_t words[PIXEL_VERTEX_FLOATS];
		memcpy(words, vertex, sizeof words);
		for (uint32_t i = 0; i < PIXEL_VERTEX_FLOATS; i++)
			memory[v * PIXEL_VERTEX_FLOATS + i] = words[i];
	}
}

// A loop grid's shader, its quads and each place's colour from argb; false, the grid undrawn, on a failure.
static bool make_loop(LoopGrid *g, const char *name, uint32_t quads, uint32_t (*argb)(uint32_t column, uint32_t row))
{
	if (!compile_shader(name, &g->pixel, &g->compiled, &g->needs, &g->need_count))
		return false;
	const uint32_t quad_words = QUAD_VERTICES * PIXEL_VERTEX_FLOATS;
	g->memory = xsdk_gpu_alloc(quads * quad_words * 4);
	if (!g->memory)
		return false;
	write_square(g->memory, g->x, g->y, g->width, g->height);
	if (quads > 1)
		write_square(g->memory + quad_words, 0, 0, WIDTH, HEIGHT);
	__asm__ volatile("sync" ::: "memory");
	g->vertices = xsdk_gpu_address(g->memory);
	xsdk_gpu_invalidate(g->vertices, quads * quad_words * 4);
	for (uint32_t row = 0; row < LOOP_CELL; row++) {
		for (uint32_t column = 0; column < LOOP_CELL; column++)
			g->argb[row][column] = argb(column, row);
	}
	return true;
}

// vk_loop_count_grid.frag's grid and its quad over the target, and where its push block and loop constant are; false,
// the grid undrawn, on a failure.
static bool make_count(LoopGrid *g)
{
	*g = (LoopGrid){ .x = COUNT_X, .y = COUNT_Y, .width = COUNT_WIDTH, .height = COUNT_HEIGHT };
	if (!make_loop(g, "vk_loop_count_grid.frag", 2, count_argb))
		return false;
	bool block = false, loop = false;
	for (size_t i = 0; i < g->compiled.resource_count; i++) {
		if (g->compiled.resources[i].kind == XSDK_XSC_PUSH_CONSTANTS) {
			g->block = g->compiled.resources[i].reg;
			block = true;
		}
	}
	for (size_t i = 0; i < g->need_count; i++) {
		if (g->needs[i].kind == XSDK_XSC_BLOCK_LOOP) {
			g->loop_constant = g->needs[i].reg;
			loop = true;
		}
	}
	return block && loop;
}

// triangle_vertex_shader for a position and the indices, both grids' quads, and the grids; false, the grids undrawn,
// on a failure.
static bool make_pixel_grids(PixelGrids *p)
{
	static const XsdkVertexElement indices_layout[] = {
		{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	};
	if (!NT_SUCCESS(xsdk_shader_load(&p->vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&p->vertex, indices_layout,
			sizeof indices_layout / sizeof indices_layout[0], (const uint32_t[]){ PIXEL_STRIDE_BYTES })))
		return false;
	const uint32_t grid_bytes = PIXEL_GRID_VERTICES * PIXEL_STRIDE_BYTES;
	p->memory = xsdk_gpu_alloc(2 * grid_bytes);
	if (!p->memory)
		return false;
	write_pixel_grid(p->memory, TEXCOORD_X);
	write_pixel_grid(p->memory + grid_bytes / 4, UNIFORM_IF_X);
	__asm__ volatile("sync" ::: "memory");
	const uint32_t address = xsdk_gpu_address(p->memory);
	xsdk_gpu_invalidate(address, 2 * grid_bytes);
	if (!make_pixel_grid(&p->texcoord, "vk_dynamic_texcoord.frag", "texcoord", TEXCOORD_X, address) ||
		!make_pixel_grid(&p->uniform_if, "vk_dynamic_if.frag", "uniform if", UNIFORM_IF_X, address + grid_bytes) ||
		!make_texcoord(p) || !make_slow(&p->slow))
		return false;
	p->loop = (LoopGrid){ .x = LOOP_X, .y = LOOP_Y, .width = LOOP_WIDTH, .height = LOOP_HEIGHT };
	if (!make_loop(&p->loop, "vk_loop_grid.frag", 1, loop_argb) || !make_count(&p->count))
		return false;
	make_uniform_if(&p->uniform_if);
	return true;
}

// vk_loop_count_grid.frag over the whole target, every pixel leaving at the first pass, the GPU's clock copied with it
// idle before and after; the fewest clocks of a few rounds, 0 if a fence didn't land.
static uint64_t time_loop(const PixelGrids *p, volatile uint32_t *clocks, uint32_t passes)
{
	const uint32_t address = xsdk_gpu_address(clocks);
	uint64_t best = 0;
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	for (uint32_t round = 0; round < TIMED_ROUNDS; round++) {
		xsdk_gpu_write_clock(address, true);
		draw_loop(p, &p->count, 1, first_pass_place, passes);
		xsdk_gpu_write_clock(address + XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t), true);
		if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
			return 0;
		const uint64_t taken = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clocks);
		if (round == 0 || taken < best)
			best = taken;
	}
	return best;
}

// A group leaves a hardware loop once none of its pixels is left (docs/HARDWARE.md, "Loops on the console, milestone
// 46 step 1"): every pixel leaving at the first pass of 255 takes as long as a count of 1.
static void time_early_exit(const PixelGrids *p)
{
	volatile uint32_t *const clocks = xsdk_gpu_alloc(2 * XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t));
	const uint64_t one = clocks ? time_loop(p, clocks, 1) : 0;
	const uint64_t most = clocks ? time_loop(p, clocks, TIMED_PASSES) : 0;
	if (clocks)
		xsdk_gpu_free(clocks);
	char name[320];
	(void)snprintf(name, sizeof name,
		"raster: vk_loop_count_grid.frag over the whole target, every pixel leaving at its loop's first pass, takes "
		"%llu GPU clocks under a count of %u, within a tenth of %llu under a count of 1",
		(unsigned long long)most, (unsigned)TIMED_PASSES, (unsigned long long)one);
	check(name, one && most && most * 10 <= one * TIMED_SLACK_TENTHS);
}

static volatile uint32_t *upload_square(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	if (!vertices)
		return NULL;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), sizeof square);
	return vertices;
}

void test_raster(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("raster: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("raster: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check("raster: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("raster: vertex fetch patched for a float4 position", patched);
	volatile uint32_t *const vertices = upload_square();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("raster: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	Sprites sprite_state = { .vertex = { .microcode = NULL }, .pixel = { .microcode = NULL } };
	const bool sprites_made = make_sprites(&sprite_state);
	check("raster: xost compiles and xsdk loads the point sprite shaders on the console", sprites_made);
	Indexed indexed = { .vertex = { .microcode = NULL } };
	const bool indexed_made = make_indexed(&indexed);
	check("raster: xost compiles and xsdk loads vk_dynamic_interpolator.vert on the console", indexed_made);
	PixelGrids grids = { .vertex = { .microcode = NULL } };
	const bool grids_made = make_pixel_grids(&grids);
	check(
		"raster: xost compiles and xsdk loads vk_dynamic_texcoord.frag, vk_dynamic_if.frag, vk_slow_fetch.frag, "
		"vk_loop_grid.frag and vk_loop_count_grid.frag on the console",
		grids_made);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && buffers[0] && buffers[1])
		draw_frames(&vertex, &pixel, xsdk_gpu_address(vertices), buffers, has_display,
			sprites_made ? &sprite_state : NULL, indexed_made ? &indexed : NULL, grids_made ? &grids : NULL);
	if (grids_made)
		time_early_exit(&grids);
	xsdk_display_close();
	if (grids.count.memory)
		xsdk_gpu_free(grids.count.memory);
	if (grids.count.pixel.microcode)
		xsdk_shader_free(&grids.count.pixel);
	if (grids.loop.memory)
		xsdk_gpu_free(grids.loop.memory);
	if (grids.loop.pixel.microcode)
		xsdk_shader_free(&grids.loop.pixel);
	xsdk_texture_free(&grids.slow.texture);
	if (grids.slow.memory)
		xsdk_gpu_free(grids.slow.memory);
	if (grids.slow.pixel.microcode)
		xsdk_shader_free(&grids.slow.pixel);
	xsdk_texture_free(&grids.texture);
	xsdk_gpu_free(grids.memory);
	if (grids.uniform_if.pixel.microcode)
		xsdk_shader_free(&grids.uniform_if.pixel);
	if (grids.texcoord.pixel.microcode)
		xsdk_shader_free(&grids.texcoord.pixel);
	if (grids.vertex.microcode)
		xsdk_shader_free(&grids.vertex);
	if (indexed.memory)
		xsdk_gpu_free(indexed.memory);
	if (indexed.vertex.microcode)
		xsdk_shader_free(&indexed.vertex);
	if (sprite_state.memory)
		xsdk_gpu_free(sprite_state.memory);
	if (sprite_state.pixel.microcode)
		xsdk_shader_free(&sprite_state.pixel);
	if (sprite_state.vertex.microcode)
		xsdk_shader_free(&sprite_state.vertex);
	// The shaders' fetch patch records point into the containers (src/shader.c), so they go last.
	xsdk_xsc_free(&sprite_state.pixel_compiled);
	xsdk_xsc_free(&sprite_state.vertex_compiled);
	xsdk_xsc_free(&indexed.compiled);
	xsdk_xsc_free(&grids.count.compiled);
	xsdk_xsc_free(&grids.loop.compiled);
	xsdk_xsc_free(&grids.slow.compiled);
	xsdk_xsc_free(&grids.uniform_if.compiled);
	xsdk_xsc_free(&grids.texcoord.compiled);
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("raster: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
