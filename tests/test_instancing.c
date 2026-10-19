#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsc_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/xsc.h"

// Instanced draws (docs/scope/47-instancing.md, step 3), from xost's shaders compiled on the console, each check a
// frame of its own: grids of 12,000 instances of a quad over 8 by 8 cells of the whole target, auto-indexed in two
// pieces, from 16-bit copies of a quad's indices and from 32-bit ones, each cell placed and coloured by its instance's
// element and blue from gl_InstanceIndex; 50 instances of a triangle strip, an instance a piece; and a point a vertex
// at the counts that leave the division the least room. The strips and the points read no element at the instance's
// rate, each moved by gl_InstanceIndex alone, so a break of those elements fails the grids alone.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 1000,
	MAX_LOGGED = 4,
	// Blue no instance's blue comes within the tolerance of, as theirs are multiples of 4.
	CLEAR_ARGB = 0xFF202022,
	// ubyte4n's colours convert once more on the way to the 8-bit target (tests/test_vertex.c).
	TOLERANCE = 1,
	// The grids: 160 by 90 cells of 8 by 8, the first 12,000 drawn, from instance 100.
	CELL = 8,
	CELLS_ACROSS = WIDTH / CELL,
	CELLS = CELLS_ACROSS * (HEIGHT / CELL),
	GRID_INSTANCES = 12000,
	GRID_FIRST = 100,
	QUAD_VERTICES = 6,
	QUAD_CORNERS = 4,
	SHORT_COPIES = 64,
	// The power of two above the 32-bit copies' largest index, 69,999, and the copies 24-bit indices then hold.
	LONG_STEP = 1 << 17,
	LONG_COPIES = 128,
	// The corners' stream: the 6-vertex quad, the 4-corner quad after it, and the 32-bit copies' quad at its end.
	NEAR_CORNERS = QUAD_VERTICES,
	FAR_CORNERS = 69996,
	// Long enough that a break fetching the instance's stream at the vertex stays in it.
	ELEMENTS = FAR_CORNERS + QUAD_CORNERS,
	CORNER_BYTES = 2 * 4,
	INSTANCE_BYTES = 3 * 4,
	// A draw packet counts 16 bits of vertices or indices (xenia:src/xenia/gpu/registers.h:327).
	MAX_DRAW = 0xFFFF,
	// The step shader's vertices: a corner and its step an instance, then a colour; a packet's worth, so a division
	// that comes out low still fetches inside them.
	STEP_BYTES = 5 * 4,
	STEP_ELEMENTS = MAX_DRAW,
	// The strips: two squares of 10 a strip, from x 40, an instance every 24 pixels, from instance 7.
	STRIP_VERTICES = 6,
	STRIP_SQUARE = 10,
	STRIP_INSTANCES = 50,
	STRIP_FIRST = 7,
	STRIP_X = 40,
	STRIP_Y = 300,
	STRIP_STEP = 24,
	STRIP_RED = 0xA0,
	STRIP_GREEN = 0x60,
	// The points: each the top left pixel of a cell of 2 by 2, a block of them an instance, from instance 5.
	POINT_CELLS_ACROSS = WIDTH / 2,
	DIVISION_FIRST = 5,
};

// The three of the probe's counts (docs/HARDWARE.md, "Instancing on the console, milestone 47 step 1") that leave the
// division the least room, 0.494/V to 0.495/V, under xost's test's three roundings
// (xenos-toolchain/tests/test_instancing.py, check_division).
static const uint32_t division_counts[] = { 255, 257, 21845 };

// A quad's two triangles in units of a cell, and a 4-corner quad's corners and indices drawing the same.
static const float quad_corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
static const float corner_places[QUAD_CORNERS][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
static const uint32_t quad_indices[QUAD_VERTICES] = { 0, 1, 2, 2, 1, 3 };
// A strip of two squares, in units of a square.
static const float strip_corners[STRIP_VERTICES][2] = { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 2, 0 }, { 2, 1 } };

// vk_instance_cells.vert's corner at the vertex's rate, and place and colour at the instance's.
static const XsdkVertexElement cell_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT2, .stream = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .stream = 1, .per_instance = true },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.format = XSDK_VERTEX_UBYTE4N,
		.stream = 1,
		.offset_bytes = 8,
		.per_instance = true },
};
static const uint32_t cell_strides[] = { CORNER_BYTES, INSTANCE_BYTES };
// vk_instance_step.vert's corner with its step, and colour, both at the vertex's rate.
static const XsdkVertexElement step_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_UBYTE4N, .offset_bytes = 16 },
};

typedef struct {
	XsdkXscShader cells_compiled;
	XsdkXscShader steps_compiled;
	XsdkShader cells;
	XsdkShader steps;
	XsdkShader pixel;
	volatile uint32_t *corners;
	volatile uint32_t *instances;
	volatile uint32_t *points;
	volatile uint32_t *short_memory;
	volatile uint32_t *long_memory;
	XsdkRepeatedIndices short_copies;
	XsdkRepeatedIndices long_copies;
	volatile uint32_t *image;
} Scene;

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// Pixel x's centre at x in clip space, as xsdk sets the GPU's pixel centres (src/gpu_setup.c, PIX_CENTER_D3D).
static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// What a resolve stores for a colour, alpha 255: its bytes blue, green, red, alpha from the top.
static uint32_t stored(uint32_t red, uint32_t green, uint32_t blue)
{
	return blue << 24 | green << 16 | red << 8 | 0xFF;
}

// An element's red and green, 2 apart from either neighbour's, so a wrong element shows past the tolerance.
static uint32_t element_red(uint32_t element)
{
	return element % 128 * 2;
}

static uint32_t element_green(uint32_t element)
{
	return element / 128 % 128 * 2;
}

static uint32_t ubyte4n(uint32_t red, uint32_t green)
{
	return red | green << 8 | 0xFFu << 24;
}

// Blue from gl_InstanceIndex, as both shaders work it out.
static uint32_t instance_blue(uint32_t instance)
{
	return instance % 64 * 4;
}

static uint32_t clear_stored(void)
{
	return stored(CLEAR_ARGB >> 16 & 0xFF, CLEAR_ARGB >> 8 & 0xFF, CLEAR_ARGB & 0xFF);
}

static bool close_to(uint32_t held, uint32_t want)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t a = held >> shift & 0xFF;
		const uint32_t b = want >> shift & 0xFF;
		if ((a > b ? a - b : b - a) > TOLERANCE)
			return false;
	}
	return true;
}

// Counts the pixel in wrong unless it holds want, logging the first few.
static void tally(const Scene *s, const char *what, uint32_t x, uint32_t y, uint32_t want, uint32_t *wrong)
{
	const uint32_t held = s->image[tiled_index(x, y, WIDTH)];
	if (close_to(held, want))
		return;
	if ((*wrong)++ < MAX_LOGGED)
		xsdk_log_write("instancing: %s: (%u, %u) held %08X, not %08X", what, (unsigned)x, (unsigned)y, (unsigned)held,
			(unsigned)want);
}

static void hand_to_gpu(volatile uint32_t *memory, uint32_t size_bytes)
{
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
}

// The target, the shaders and the shader's literals.
static void begin_frame(const Scene *s, const XsdkShader *vertex, const XsdkXscShader *compiled)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(vertex, &s->pixel);
	for (size_t i = 0; i < compiled->need_count; i++) {
		const XsdkXscNeed *const n = &compiled->needs[i];
		if (n->kind != XSDK_XSC_LITERAL)
			continue;
		const float value[4] = { (float)n->value[0], (float)n->value[1], (float)n->value[2], (float)n->value[3] };
		xsdk_gpu_set_vertex_constants(n->reg, value, 1);
	}
}

// Resolves the frame, clearing eDRAM, and waits for it; false if the fence didn't land.
static bool end_frame(const Scene *s, const char *what)
{
	xsdk_gpu_resolve(xsdk_gpu_address(s->image), WIDTH, HEIGHT, CLEAR_ARGB);
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	if (!landed)
		xsdk_log_write("instancing: %s: the fence didn't land", what);
	return landed;
}

// The corners' stream: cells' corners in clip space, each quad's from its top left.
static void put_corner(volatile uint32_t *corners, uint32_t n, const float corner[2])
{
	corners[2 * n] = float_bits(corner[0] * CELL / (WIDTH / 2.0f));
	corners[2 * n + 1] = float_bits(-corner[1] * CELL / (HEIGHT / 2.0f));
}

static void put_corners(volatile uint32_t *corners)
{
	for (uint32_t i = 0; i < 2 * ELEMENTS; i++)
		corners[i] = 0;
	for (uint32_t v = 0; v < QUAD_VERTICES; v++)
		put_corner(corners, v, quad_corners[v]);
	for (uint32_t v = 0; v < QUAD_CORNERS; v++) {
		put_corner(corners, NEAR_CORNERS + v, corner_places[v]);
		put_corner(corners, FAR_CORNERS + v, corner_places[v]);
	}
}

// Element e of the instances' stream from GRID_FIRST: its cell's top left corner, half a pixel up and left of its
// first pixel's centre, and its colour; the elements before and past them 0.
static void put_instances(volatile uint32_t *instances)
{
	for (uint32_t e = 0; e < ELEMENTS; e++) {
		const uint32_t cell = e - GRID_FIRST;
		const bool drawn = e >= GRID_FIRST && cell < GRID_INSTANCES;
		instances[3 * e] = drawn ? float_bits(clip_x(cell % CELLS_ACROSS * CELL - 0.5f)) : 0;
		instances[3 * e + 1] = drawn ? float_bits(clip_y(cell / CELLS_ACROSS * CELL - 0.5f)) : 0;
		instances[3 * e + 2] = drawn ? ubyte4n(element_red(e), element_green(e)) : 0;
	}
}

// What a grid's cell's centre should hold: its instance's colour, or the clear past the instances.
static uint32_t cell_want(uint32_t cell)
{
	const uint32_t e = GRID_FIRST + cell;
	return cell < GRID_INSTANCES ? stored(element_red(e), element_green(e), instance_blue(e)) : clear_stored();
}

// The grid's 12,000 instances, auto-indexed or from copies, each corner from first_corner; how many of the 14,400
// cells' centres are wrong, or CELLS + 1 if the fence didn't land.
static uint32_t grid_wrong(const Scene *s, const char *what, const XsdkRepeatedIndices *copies, uint32_t first_corner)
{
	begin_frame(s, &s->cells, &s->cells_compiled);
	const uint32_t skipped = first_corner * CORNER_BYTES;
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(s->corners) + skipped, ELEMENTS * CORNER_BYTES - skipped);
	xsdk_gpu_set_vertices(1, xsdk_gpu_address(s->instances), ELEMENTS * INSTANCE_BYTES);
	if (copies)
		xsdk_gpu_draw_indexed_instanced(XSDK_PRIMITIVE_TRIANGLE_LIST, copies, GRID_FIRST, GRID_INSTANCES);
	else
		xsdk_gpu_draw_instanced(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES, GRID_FIRST, GRID_INSTANCES);
	if (!end_frame(s, what))
		return CELLS + 1;
	uint32_t wrong = 0;
	for (uint32_t cell = 0; cell < CELLS; cell++)
		tally(s, what, cell % CELLS_ACROSS * CELL + CELL / 2, cell / CELLS_ACROSS * CELL + CELL / 2, cell_want(cell),
			&wrong);
	return wrong;
}

static void check_grids(const Scene *s)
{
	char name[320];
	uint32_t wrong = grid_wrong(s, "the auto-indexed grid", NULL, 0);
	(void)snprintf(name, sizeof name,
		"instancing: one draw of 12,000 instances of a 6-vertex quad from instance 100, in two pieces, colours each of "
		"14,400 cells of 8 by 8 as its instance's element and gl_InstanceIndex, the 2,400 past them clear: %u wrong",
		(unsigned)wrong);
	check(name, wrong == 0);
	const XsdkRepeatedIndices *const short_copies = &s->short_copies;
	wrong = grid_wrong(s, "the 16-bit grid", short_copies, NEAR_CORNERS);
	(void)snprintf(name, sizeof name,
		"instancing: one draw of the same 12,000 instances from a 4-vertex quad's 6 indices in 64 copies, 16-bit, "
		"moved on by 4: %u of 14,400 cells wrong",
		(unsigned)wrong);
	check(name,
		wrong == 0 && short_copies->copies == SHORT_COPIES && short_copies->step == QUAD_CORNERS &&
			!short_copies->indices_32_bit);
	const XsdkRepeatedIndices *const long_copies = &s->long_copies;
	wrong = grid_wrong(s, "the 32-bit grid", long_copies, 0);
	(void)snprintf(name, sizeof name,
		"instancing: one draw of the same 12,000 instances from a quad at vertices 69,996 to 69,999 in as many copies "
		"as 24-bit indices hold, %u, 32-bit, moved on by %u: %u of 14,400 cells wrong",
		(unsigned)long_copies->copies, (unsigned)long_copies->step, (unsigned)wrong);
	check(name,
		wrong == 0 && long_copies->copies == LONG_COPIES && long_copies->step == LONG_STEP &&
			long_copies->indices_32_bit);
}

// A vertex of the step shader: a corner, the step gl_InstanceIndex moves it by, in pixels, and its colour.
static void put_step(
	volatile uint32_t *points, uint32_t n, float x, float y, float step_x, float step_y, uint32_t colour)
{
	points[5 * n] = float_bits(clip_x(x));
	points[5 * n + 1] = float_bits(clip_y(y));
	points[5 * n + 2] = float_bits(step_x / (WIDTH / 2.0f));
	points[5 * n + 3] = float_bits(-step_y / (HEIGHT / 2.0f));
	points[5 * n + 4] = colour;
}

// 50 instances of a strip, each its two squares from STRIP_X, STRIP_STEP on for each instance past the first; each
// square's centre in its instance's colour, and the gaps between, before the first and past the last clear.
static void check_strips(const Scene *s)
{
	for (uint32_t v = 0; v < STRIP_VERTICES; v++)
		put_step(s->points, v, STRIP_X - 0.5f + strip_corners[v][0] * STRIP_SQUARE - STRIP_FIRST * STRIP_STEP,
			STRIP_Y - 0.5f + strip_corners[v][1] * STRIP_SQUARE, STRIP_STEP, 0.0f, ubyte4n(STRIP_RED, STRIP_GREEN));
	hand_to_gpu(s->points, STRIP_VERTICES * STEP_BYTES);
	begin_frame(s, &s->steps, &s->steps_compiled);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(s->points), STEP_ELEMENTS * STEP_BYTES);
	xsdk_gpu_draw_instanced(XSDK_PRIMITIVE_TRIANGLE_STRIP, STRIP_VERTICES, STRIP_FIRST, STRIP_INSTANCES);
	const bool landed = end_frame(s, "the strips");
	const uint32_t clear = clear_stored();
	const uint32_t y = STRIP_Y + STRIP_SQUARE / 2;
	uint32_t wrong = 0;
	uint32_t checked = 0;
	// Place n is instance n - 1's: the one before the first, each instance's, and the one past the last.
	for (uint32_t n = 0; landed && n < STRIP_INSTANCES + 2; n++) {
		const uint32_t x = STRIP_X + n * STRIP_STEP - STRIP_STEP;
		const bool drawn = n >= 1 && n <= STRIP_INSTANCES;
		const uint32_t want = drawn ? stored(STRIP_RED, STRIP_GREEN, instance_blue(STRIP_FIRST + n - 1)) : clear;
		tally(s, "the strips", x + STRIP_SQUARE / 2, y, want, &wrong);
		tally(s, "the strips", x + STRIP_SQUARE * 3 / 2, y, want, &wrong);
		tally(s, "the strips", x + STRIP_SQUARE * 2 + 1, y, clear, &wrong);
		checked += 3;
	}
	char name[256];
	(void)snprintf(name, sizeof name,
		"instancing: 50 instances of a 6-vertex triangle strip from instance 7, an instance a piece, each moved and "
		"coloured by gl_InstanceIndex: %u of %u pixels wrong",
		(unsigned)wrong, (unsigned)checked);
	check(name, landed && wrong == 0);
}

// The points at a count: as many instances as a packet holds, instance k a block of rows, from the top, as wide as
// its points up to a row of cells; point j of it at its place in the block, coloured by j and gl_InstanceIndex.
// How many are wrong, or all of them if the fence didn't land.
static uint32_t division_wrong(const Scene *s, uint32_t vertex_count)
{
	const uint32_t instance_count = MAX_DRAW / vertex_count;
	const uint32_t width = vertex_count < POINT_CELLS_ACROSS ? vertex_count : POINT_CELLS_ACROSS;
	const uint32_t rows = (vertex_count + width - 1) / width;
	for (uint32_t j = 0; j < vertex_count; j++)
		put_step(s->points, j, 2.0f * (j % width), 2.0f * (j / width) - 2.0f * rows * DIVISION_FIRST, 0.0f, 2.0f * rows,
			ubyte4n(element_red(j), element_green(j)));
	hand_to_gpu(s->points, vertex_count * STEP_BYTES);
	begin_frame(s, &s->steps, &s->steps_compiled);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(s->points), STEP_ELEMENTS * STEP_BYTES);
	xsdk_gpu_draw_instanced(XSDK_PRIMITIVE_POINT_LIST, vertex_count, DIVISION_FIRST, instance_count);
	char what[64];
	(void)snprintf(what, sizeof what, "the points at V %u", (unsigned)vertex_count);
	if (!end_frame(s, what))
		return instance_count * vertex_count;
	uint32_t wrong = 0;
	for (uint32_t k = 0; k < instance_count; k++) {
		const uint32_t want_blue = instance_blue(DIVISION_FIRST + k);
		for (uint32_t j = 0; j < vertex_count; j++)
			tally(s, what, 2 * (j % width), 2 * (k * rows + j / width),
				stored(element_red(j), element_green(j), want_blue), &wrong);
	}
	return wrong;
}

static void check_division(const Scene *s)
{
	const uint32_t counts = sizeof division_counts / sizeof division_counts[0];
	uint32_t wrong = 0;
	uint32_t points = 0;
	for (uint32_t c = 0; c < counts; c++) {
		wrong += division_wrong(s, division_counts[c]);
		points += MAX_DRAW / division_counts[c] * division_counts[c];
	}
	char name[256];
	(void)snprintf(name, sizeof name,
		"instancing: a point a vertex at V 255, 257 and 21,845, as many instances as a packet holds from instance 5, "
		"each on its own pixel in its colour: %u of %u points wrong",
		(unsigned)wrong, (unsigned)points);
	check(name, wrong == 0);
}

static const XscTestShader *xsc_shader(const char *name)
{
	for (size_t i = 0; i < sizeof xsc_test_shaders / sizeof xsc_test_shaders[0]; i++) {
		if (!strcmp(xsc_test_shaders[i].name, name))
			return &xsc_test_shaders[i];
	}
	return NULL;
}

// xost's compiler on the console, as the raster test compiles its shaders; true if the shader loads instanced at the
// group the compile lists.
static bool compile_instanced(const char *name, XsdkShader *shader, XsdkXscShader *compiled)
{
	const XscTestShader *const source = xsc_shader(name);
	XsdkXscOptions options;
	xsdk_xsc_default_options(&options);
	options.optimize = true;
	if (!source || xsdk_xsc_compile(source->spirv, source->spirv_words, &options, compiled) != XSDK_XSC_OK ||
		!NT_SUCCESS(xsdk_shader_load(shader, (const uint32_t *)compiled->container, compiled->container_bytes)))
		return false;
	for (size_t i = 0; i < compiled->resource_count; i++) {
		if (compiled->resources[i].kind == XSDK_XSC_INSTANCE)
			return shader->instanced && shader->instance_constant == compiled->resources[i].reg;
	}
	return false;
}

// triangle_vertex_shader, a float4 position and colour, isn't instanced, so an element at the instance's rate is
// refused for it.
static bool refuses_plain(void)
{
	static const uint32_t strides[] = { 2 * 4 * 4 };
	static const XsdkVertexElement layout[] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .per_instance = true },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
	};
	XsdkShader plain = { .microcode = NULL };
	if (!NT_SUCCESS(xsdk_shader_load(&plain, triangle_vertex_shader, sizeof triangle_vertex_shader)))
		return false;
	const NTSTATUS status = xsdk_shader_set_vertex_layout(&plain, layout, 2, strides);
	const bool refused = status == STATUS_INVALID_PARAMETER && !plain.instanced;
	xsdk_shader_free(&plain);
	return refused;
}

static bool make_scene(Scene *s)
{
	s->corners = xsdk_gpu_alloc(ELEMENTS * CORNER_BYTES);
	s->instances = xsdk_gpu_alloc(ELEMENTS * INSTANCE_BYTES);
	s->points = xsdk_gpu_alloc(STEP_ELEMENTS * STEP_BYTES);
	s->image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	const uint32_t far_indices[QUAD_VERTICES] = { FAR_CORNERS + 0, FAR_CORNERS + 1, FAR_CORNERS + 2, FAR_CORNERS + 2,
		FAR_CORNERS + 1, FAR_CORNERS + 3 };
	const uint32_t short_bytes = xsdk_gpu_repeated_index_bytes(quad_indices, QUAD_VERTICES, SHORT_COPIES);
	const uint32_t long_bytes = xsdk_gpu_repeated_index_bytes(far_indices, QUAD_VERTICES, 0);
	s->short_memory = xsdk_gpu_alloc(short_bytes);
	s->long_memory = xsdk_gpu_alloc(long_bytes);
	if (!s->corners || !s->instances || !s->points || !s->image || !s->short_memory || !s->long_memory)
		return false;
	put_corners(s->corners);
	hand_to_gpu(s->corners, ELEMENTS * CORNER_BYTES);
	put_instances(s->instances);
	hand_to_gpu(s->instances, ELEMENTS * INSTANCE_BYTES);
	for (uint32_t i = 0; i < STEP_ELEMENTS * STEP_BYTES / 4; i++)
		s->points[i] = 0;
	hand_to_gpu(s->points, STEP_ELEMENTS * STEP_BYTES);
	xsdk_gpu_repeat_indices(&s->short_copies, s->short_memory, quad_indices, QUAD_VERTICES, SHORT_COPIES);
	xsdk_gpu_invalidate(s->short_copies.address, short_bytes);
	xsdk_gpu_repeat_indices(&s->long_copies, s->long_memory, far_indices, QUAD_VERTICES, 0);
	xsdk_gpu_invalidate(s->long_copies.address, long_bytes);
	return NT_SUCCESS(xsdk_shader_set_vertex_layout(&s->cells, cell_layout, 3, cell_strides)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&s->steps, step_layout, 2, (const uint32_t[]){ STEP_BYTES }));
}

static void free_scene(Scene *s)
{
	xsdk_gpu_free(s->long_memory);
	xsdk_gpu_free(s->short_memory);
	xsdk_gpu_free(s->image);
	xsdk_gpu_free(s->points);
	xsdk_gpu_free(s->instances);
	xsdk_gpu_free(s->corners);
	if (s->pixel.microcode)
		xsdk_shader_free(&s->pixel);
	if (s->steps.microcode)
		xsdk_shader_free(&s->steps);
	if (s->cells.microcode)
		xsdk_shader_free(&s->cells);
	// The shaders' fetch patch records point into the containers (src/shader.c), so they go last.
	xsdk_xsc_free(&s->steps_compiled);
	xsdk_xsc_free(&s->cells_compiled);
}

void test_instancing(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("instancing: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	check(
		"instancing: xsdk_shader_set_vertex_layout refuses an element at the instance's rate for a vertex shader that "
		"isn't instanced",
		refuses_plain());
	Scene s = { .cells = { .microcode = NULL }, .steps = { .microcode = NULL }, .pixel = { .microcode = NULL } };
	const bool compiled = compile_instanced("vk_instance_cells.vert", &s.cells, &s.cells_compiled) &&
		compile_instanced("vk_instance_step.vert", &s.steps, &s.steps_compiled) &&
		NT_SUCCESS(xsdk_shader_load(&s.pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check(
		"instancing: xost compiles vk_instance_cells.vert and vk_instance_step.vert on the console, and xsdk loads "
		"each instanced at the group register the compile lists",
		compiled);
	const bool made = compiled && make_scene(&s);
	check(
		"instancing: streams, repeated indices and the target's buffer made, the elements at the instance's rate "
		"patched",
		made);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	// The first frame's colour is whatever eDRAM held; this resolve clears it.
	if (made && end_frame(&s, "the first clear")) {
		check_grids(&s);
		check_strips(&s);
		check_division(&s);
	}
	free_scene(&s);
	check("instancing: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
