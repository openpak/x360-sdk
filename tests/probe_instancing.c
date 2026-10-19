#include <altivec.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe_gpu.h"
#include "probe_instancing_cases.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Milestone 47 step 1 (docs/scope/47-instancing.md): whether a vertex fetch takes its index from what an exec of ALU
// alone worked out, the prologue that gives each vertex its instance, the division on the console's ALU, what an
// instance costs drawn each way, and how fast the CPU reads write-combined memory. game:\instancing_start.txt may name
// one case, by number or name, to run alone.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	CASES = 5,
	// A vertex or an instance in its stream: a position, then a colour.
	ELEMENT_FLOATS = 8,
	ELEMENT_BYTES = ELEMENT_FLOATS * 4,
	// A draw packet counts 16 bits of vertices or indices (xenia:src/xenia/gpu/registers.h:327).
	MAX_DRAW = 0xFFFF,
	// A stream holds twice a packet past the first instance, with an element before it, so a floor one off, or a break
	// build, fetching an instance's width either side of the vertex still reads the probe's own memory.
	STREAM_ELEMENTS = 2 * MAX_DRAW + PROBE_FIRST_INSTANCE + 1,
	FENCE_TIMEOUT_MS = 2000,
	CLEAR_ARGB = 0xFF202020,
	CHECK = 0x5A,
	// Cases 1 and 2: 64 cells of 40 pixels, 8 to a row, from the top left.
	GRID_CELLS = 64,
	GRID_COLUMNS = 8,
	GRID_PIXELS = 40,
	QUAD_VERTICES = 6,
	QUAD_CORNERS = 4,
	QUAD_STEP = 4,
	// Case 3: each point the top left pixel of a cell of 2 by 2, so a point a pixel off lands in no other's pixel.
	POINT_CELLS_ACROSS = WIDTH / 2,
	COLOUR_CYCLE = 251,
	// Case 4: probe_gpu_draw_cost's batches, a cell of 20 by 18 pixels an instance, each 4 pixels across.
	SMALL = 500,
	LARGE = 2500,
	SIZES = 2,
	TIMED_ROUNDS = 5,
	RECORDED_ROUNDS = 9,
	COST_COLUMNS = 64,
	COST_ROWS = 40,
	COST_CELL_WIDTH = WIDTH / COST_COLUMNS,
	COST_CELL_HEIGHT = HEIGHT / COST_ROWS,
	SHAPE_PIXELS = 4,
	CUBE_VERTICES = 24,
	CUBE_INDICES = 36,
	// The power of two above the cube's largest index.
	CUBE_STEP = 32,
	// Where case 4's meshes sit in stream 0: the cube, its 36 corners, the 6-vertex quad and the 4-vertex quad.
	CUBE_AT = 0,
	CORNERS_AT = CUBE_AT + CUBE_VERTICES,
	QUAD_AT = CORNERS_AT + CUBE_INDICES,
	QUAD_CORNERS_AT = QUAD_AT + QUAD_VERTICES,
	MAX_INSTANCE_DWORDS = 64,
	// The time base ticks at 49.875 MHz, every 64 CPU clocks (src/time.c).
	TICKS_PER_MS = 49875,
	CLOCKS_PER_TICK = 64,
	CLOCKS_PER_US = 3192,
	READ_BYTES = 64 * 1024,
	READ_WORDS = READ_BYTES / 4,
	READ_ROUNDS = 8,
	WRONG_LOGGED = 4,
	TEXT_BYTES = 400,
};

_Static_assert(LARGE <= COST_COLUMNS * COST_ROWS && GRID_CELLS * QUAD_VERTICES <= MAX_DRAW, "the cells fit");

typedef enum { PASSED, DIFFERED, STOPPED } Result;

typedef struct {
	const char *name;
	const char *what;
	Result (*run)(void);
} Case;

// A repeated index buffer: copies of a mesh's indices, each moved on by step.
typedef struct {
	uint32_t address;
	uint32_t mesh_indices;
	uint32_t step;
	uint32_t copies;
	bool wide;
} Copies;

// Case 3's points: each instance a block of cells, as wide as its vertices up to a row, blocks across a row.
typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t across;
} Blocks;

typedef struct {
	uint32_t right;
	uint32_t near;
	uint32_t wrong;
	char text[TEXT_BYTES];
} Tally;

typedef enum { WAY_LAYER, WAY_GROUP, WAY_ONE_DRAW, WAY_COPIES } Way;

typedef struct {
	const char *name;
	bool cube;
	Way way;
	uint32_t copies;
} CostVariant;

typedef struct {
	uint32_t at;
	uint32_t vertices;
} Mesh;

static const char *const start_path = "game:\\instancing_start.txt";

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .stream = 0, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .stream = 0, .offset_bytes = 16 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT4, .stream = 1, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .usage_index = 1, .format = XSDK_VERTEX_FLOAT4, .stream = 1, .offset_bytes = 16 },
};
static const uint32_t strides[] = { ELEMENT_BYTES, ELEMENT_BYTES };
// The layer's way: stream 1 pointed at the instance's element, read with a stride of 0 (docs/HARDWARE.md, "Instances
// and long strips, vulkan4xenon milestone 24").
static const uint32_t layer_strides[] = { ELEMENT_BYTES, 0 };

// A quad's two triangles in units of its size, and a 4-vertex quad's corners and indices drawing the same.
static const uint8_t quad_corners[QUAD_VERTICES][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
static const uint8_t corner_places[QUAD_CORNERS][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
static const uint32_t quad_indices[QUAD_VERTICES] = { 0, 1, 2, 2, 1, 3 };

static const CostVariant cost_variants[] = {
	{ "the cube as the layer draws an instance now: the group, stream 1 at the instance, the indexed draw", true,
		WAY_LAYER, 1 },
	{ "the cube a draw an instance, only the group between: its 36 corners auto-indexed", true, WAY_GROUP, 0 },
	{ "the cube in one draw: its 36 corners auto-indexed, pieces of 1,820 instances", true, WAY_ONE_DRAW, 0 },
	{ "the cube from its 36 indices in copies of 1", true, WAY_COPIES, 1 },
	{ "the cube from its 36 indices in copies of 8", true, WAY_COPIES, 8 },
	{ "the cube from its 36 indices in copies of 64", true, WAY_COPIES, 64 },
	{ "the cube from its 36 indices in copies of 512", true, WAY_COPIES, 512 },
	{ "the quad as the layer draws an instance now: the group, stream 1 at the instance, a draw of 6", false, WAY_LAYER,
		0 },
	{ "the quad a draw an instance, only the group between: 6 vertices auto-indexed", false, WAY_GROUP, 0 },
	{ "the quad in one draw: 6 vertices auto-indexed, pieces of 10,922 instances", false, WAY_ONE_DRAW, 0 },
	{ "the quad from a 4-vertex quad's 6 indices in copies of 1", false, WAY_COPIES, 1 },
	{ "the quad from a 4-vertex quad's 6 indices in copies of 8", false, WAY_COPIES, 8 },
	{ "the quad from a 4-vertex quad's 6 indices in copies of 64", false, WAY_COPIES, 64 },
	{ "the quad from a 4-vertex quad's 6 indices in copies of 512", false, WAY_COPIES, 512 },
};

enum { COST_VARIANTS = sizeof cost_variants / sizeof cost_variants[0] };

static volatile uint32_t *vertices;
static volatile uint32_t *instances;
static volatile uint32_t *indices;
static volatile uint32_t *image;
static XsdkShader copy_shader;
static XsdkShader instanced_shader;
static XsdkShader plain_shader;
static XsdkShader pixel_shader;
static uint32_t cube_indices[CUBE_INDICES];

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

static uint64_t time_base(void)
{
	uint32_t upper;
	uint32_t lower;
	uint32_t check;
	// The upper half can tick between the two reads in 32-bit mode (src/time.c).
	do {
		__asm__ volatile("mftbu %0" : "=r"(upper));
		__asm__ volatile("mftb %0" : "=r"(lower));
		__asm__ volatile("mftbu %0" : "=r"(check));
	} while (upper != check);
	return (uint64_t)upper << 32 | lower;
}

static void append(char *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

static void append(char *text, const char *format, ...)
{
	const size_t used = strlen(text);
	va_list args;
	va_start(args, format);
	vsnprintf(text + used, TEXT_BYTES - used, format, args);
	va_end(args);
}

// A byte as a colour, a quarter into its step, so a resolve that rounds or truncates keeps it.
static float byte_colour(uint32_t byte)
{
	return (byte + 0.25f) / 255.0f;
}

// What a resolve stores for red, green, blue and alpha 255: blue, green, red, alpha from the top byte.
static uint32_t stored(uint32_t red, uint32_t green, uint32_t blue)
{
	return blue << 24 | green << 16 | red << 8 | 0xFF;
}

// Instance element e's colour holds e's low and high bytes; its vertex adds blue.
static uint32_t instance_stored(uint32_t element, uint32_t blue)
{
	return stored(element & 0xFF, element >> 8 & 0xFF, blue);
}

static void put_element(volatile uint32_t *stream, uint32_t n, const float values[ELEMENT_FLOATS])
{
	uint32_t words[ELEMENT_FLOATS];
	memcpy(words, values, sizeof words);
	for (uint32_t i = 0; i < ELEMENT_FLOATS; i++)
		stream[n * ELEMENT_FLOATS + i] = words[i];
}

// Pixel x's centre at x in clip space, as xsdk sets the GPU's pixel centres (src/gpu_setup.c, PIX_CENTER_D3D;
// xenia:src/xenia/gpu/draw_util.cc:344-348), and an offset of so many pixels.
static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// A vertex dx, dy pixels from its instance's place, adding blue to the instance's colour.
static void put_vertex(uint32_t n, float dx, float dy, float z, uint32_t blue)
{
	const float values[ELEMENT_FLOATS] = { dx / (WIDTH / 2.0f), -dy / (HEIGHT / 2.0f), z, 1.0f, 0.0f, 0.0f,
		byte_colour(blue), 0.0f };
	put_element(vertices, n, values);
}

// Instance element e at pixel x, y, its colour e's.
static void put_instance(uint32_t e, float x, float y)
{
	const float values[ELEMENT_FLOATS] = { clip_x(x), clip_y(y), 0.0f, 0.0f, byte_colour(e & 0xFF),
		byte_colour(e >> 8 & 0xFF), 0.0f, 1.0f };
	put_element(instances, e, values);
}

static void hand_to_gpu(volatile uint32_t *memory, uint32_t size_bytes)
{
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
}

// Points stream 0 at the first count vertices and stream 1 at the instances', after the CPU wrote them.
static void hand_streams(uint32_t vertex_count, uint32_t instance_count)
{
	hand_to_gpu(vertices, vertex_count * ELEMENT_BYTES);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(vertices), vertex_count * ELEMENT_BYTES);
	if (!instance_count)
		return;
	hand_to_gpu(instances, instance_count * ELEMENT_BYTES);
	xsdk_gpu_set_vertices(1, xsdk_gpu_address(instances), instance_count * ELEMENT_BYTES);
}

// The design's group: 1/S, 0.5/S or 0 for a power of two, S and the piece's first instance, 1/S worked out by the CPU
// (docs/scope/47-instancing.md, "The design").
static void set_group(uint32_t divisor, uint32_t first)
{
	const bool power = (divisor & (divisor - 1)) == 0;
	const float values[4] = { (float)(1.0 / divisor), power ? 0.0f : (float)(0.5 / divisor), (float)divisor,
		(float)first };
	xsdk_gpu_set_vertex_constants(PROBE_GROUP, values, 1);
}

// count instances of an auto-indexed mesh, in pieces of up to per_piece whole instances, each after its group.
static void draw_auto(uint32_t primitive, uint32_t vertex_count, uint32_t first, uint32_t count, uint32_t per_piece)
{
	for (uint32_t done = 0; done < count; done += per_piece) {
		set_group(vertex_count, first + done);
		xsdk_gpu_draw(primitive, min_u32(count - done, per_piece) * vertex_count);
	}
}

// count instances from repeated indices, a piece as many instances as there are copies, each after its group.
static void draw_copies(uint32_t primitive, const Copies *c, uint32_t first, uint32_t count)
{
	for (uint32_t done = 0; done < count; done += c->copies) {
		set_group(c->step, first + done);
		xsdk_gpu_draw_indexed(primitive, c->address, min_u32(count - done, c->copies) * c->mesh_indices, c->wide);
	}
}

// copies of the mesh's indices, or of 0 to mesh_indices - 1 without one, each moved on by step, 16 or 32 bits
// big-endian (src/gpu_draw.c, xsdk_gpu_draw_indexed).
static Copies make_copies(const uint32_t *mesh, uint32_t mesh_indices, uint32_t step, uint32_t copies, bool wide)
{
#ifdef INSTANCING_BREAK_NOSTEP
	const uint32_t moved = 0;
#else
	const uint32_t moved = step;
#endif
	const uint32_t total = copies * mesh_indices;
	uint32_t pair = 0;
	for (uint32_t n = 0; n < total; n++) {
		const uint32_t m = n % mesh_indices;
		const uint32_t value = (mesh ? mesh[m] : m) + n / mesh_indices * moved;
		if (wide)
			indices[n] = value;
		else if (n % 2)
			indices[n / 2] = pair | value;
		else
			pair = value << 16;
	}
	if (!wide && total % 2)
		indices[total / 2] = pair;
	hand_to_gpu(indices, wide ? total * 4 : (total + 1) / 2 * 4);
	return (Copies){ xsdk_gpu_address(indices), mesh_indices, step, copies, wide };
}

static void set_draw_state(const XsdkShader *shader)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(shader, &pixel_shader);
}

static bool wait_logged(uint32_t case_number)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("instancing probe: case %u: the fence didn't land: 0x%08X", (unsigned)case_number, (unsigned)status);
	probe_report_cp("instancing probe", "after the hang");
	return false;
}

static bool resolve_and_wait(uint32_t case_number)
{
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	return wait_logged(case_number);
}

static uint32_t copy_colour(uint32_t cell)
{
	return stored(cell, 255 - cell, CHECK);
}

static uint32_t grid_colour(uint32_t cell)
{
	return instance_stored(PROBE_FIRST_INSTANCE + cell, CHECK);
}

// Each grid cell's centre against want(cell): one line, then true if every cell held its colour.
static bool check_grid(const char *what, uint32_t (*want)(uint32_t))
{
	uint32_t right = 0;
	char wrong[TEXT_BYTES] = "";
	for (uint32_t cell = 0; cell < GRID_CELLS; cell++) {
		const uint32_t x = cell % GRID_COLUMNS * GRID_PIXELS + GRID_PIXELS / 2;
		const uint32_t y = cell / GRID_COLUMNS * GRID_PIXELS + GRID_PIXELS / 2;
		const uint32_t held = image[tiled_index(x, y, WIDTH)];
		if (held == want(cell))
			right++;
		else if (cell - right < WRONG_LOGGED)
			append(wrong, " cell %u held %08X, wanted %08X;", (unsigned)cell, (unsigned)held, (unsigned)want(cell));
	}
	xsdk_log_write("instancing probe: %s: %s, %u of %u cells%s%s", what, right == GRID_CELLS ? "PASS" : "DIFFERS",
		(unsigned)right, (unsigned)GRID_CELLS, wrong[0] ? "; first wrong:" : "", wrong);
	return right == GRID_CELLS;
}

// A cell's top left corner, half a pixel up and left of its first pixel's centre.
static float grid_x(uint32_t cell)
{
	return cell % GRID_COLUMNS * GRID_PIXELS - 0.5f;
}

static float grid_y(uint32_t cell)
{
	return cell / GRID_COLUMNS * GRID_PIXELS - 0.5f;
}

// Case 1. The copy is an exec of ALU alone and the fetches lead the next exec, their vertex cache mask one bit a fetch
// as xost sets it (docs/HARDWARE.md, "A fifth vertex fetch"); the first fetch reading the copy is serialized, as xost
// serializes a fetch reading what the ALU wrote, and the first export reading a fetch's result (docs/HARDWARE.md,
// "Serialized fetches", "Slow fetches across execs"). tools/gen_instancing_probe.py builds it.
static Result run_fetch(void)
{
	for (uint32_t v = 0; v < GRID_CELLS * QUAD_VERTICES; v++) {
		const uint32_t cell = v / QUAD_VERTICES;
		const uint8_t *const corner = quad_corners[v % QUAD_VERTICES];
		const float values[ELEMENT_FLOATS] = { clip_x(grid_x(cell) + corner[0] * GRID_PIXELS),
			clip_y(grid_y(cell) + corner[1] * GRID_PIXELS), 0.0f, 1.0f, byte_colour(cell), byte_colour(255 - cell),
			byte_colour(CHECK), 1.0f };
		put_element(vertices, v, values);
	}
	set_draw_state(&copy_shader);
	hand_streams(GRID_CELLS * QUAD_VERTICES, 0);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, GRID_CELLS * QUAD_VERTICES);
	if (!resolve_and_wait(1))
		return STOPPED;
	return check_grid("case 1: 64 quads fetched at a copy of r0.x", copy_colour) ? PASSED : DIFFERED;
}

// Case 2's instances, each a cell from the top left.
static void put_grid_instances(void)
{
	for (uint32_t k = 0; k < GRID_CELLS; k++)
		put_instance(PROBE_FIRST_INSTANCE + k, grid_x(k), grid_y(k));
}

static Result run_prologue(void)
{
	put_grid_instances();
	for (uint32_t v = 0; v < QUAD_VERTICES; v++)
		put_vertex(v, quad_corners[v][0] * GRID_PIXELS, quad_corners[v][1] * GRID_PIXELS, 0.0f, CHECK);
	xsdk_log_write("instancing probe: case 2: about to draw 64 instances of a 6-vertex quad, auto-indexed");
	set_draw_state(&instanced_shader);
	hand_streams(QUAD_VERTICES, PROBE_FIRST_INSTANCE + GRID_CELLS);
	draw_auto(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES, PROBE_FIRST_INSTANCE, GRID_CELLS, GRID_CELLS);
	if (!resolve_and_wait(2))
		return STOPPED;
	const bool auto_indexed = check_grid("case 2: 64 instances of a 6-vertex quad, auto-indexed", grid_colour);

	for (uint32_t v = 0; v < QUAD_CORNERS; v++)
		put_vertex(v, corner_places[v][0] * GRID_PIXELS, corner_places[v][1] * GRID_PIXELS, 0.0f, CHECK);
	const Copies copies = make_copies(quad_indices, QUAD_VERTICES, QUAD_STEP, GRID_CELLS, false);
	xsdk_log_write("instancing probe: case 2: about to draw 64 copies of a 4-vertex quad's 6 indices, moved on by 4");
	set_draw_state(&instanced_shader);
	hand_streams(QUAD_CORNERS, PROBE_FIRST_INSTANCE + GRID_CELLS);
	draw_copies(XSDK_PRIMITIVE_TRIANGLE_LIST, &copies, PROBE_FIRST_INSTANCE, GRID_CELLS);
	if (!resolve_and_wait(2))
		return STOPPED;
	const bool indexed = check_grid("case 2: 64 copies of a 4-vertex quad's 6 indices, 16-bit", grid_colour);
	return auto_indexed && indexed ? PASSED : DIFFERED;
}

static Blocks blocks_of(uint32_t vertex_count)
{
	const uint32_t width = min_u32(vertex_count, POINT_CELLS_ACROSS);
	return (Blocks){ width, (vertex_count + width - 1) / width, POINT_CELLS_ACROSS / width };
}

// Point j of instance k: its block's top left cell plus its place in the block, 2 pixels a cell.
static void point_pixel(const Blocks *b, uint32_t k, uint32_t j, uint32_t *x, uint32_t *y)
{
	*x = 2 * (k % b->across * b->width + j % b->width);
	*y = 2 * (k / b->across * b->height + j / b->width);
}

// Stream 0 holds each point's place in its block and blue j mod 251; stream 1 each instance's block and its colour.
static void put_points(uint32_t vertex_count, uint32_t instance_count, const Blocks *b)
{
	for (uint32_t j = 0; j < vertex_count; j++)
		put_vertex(j, 2.0f * (j % b->width), 2.0f * (j / b->width), 0.0f, j % COLOUR_CYCLE);
	for (uint32_t k = 0; k < instance_count; k++) {
		uint32_t x;
		uint32_t y;
		point_pixel(b, k, 0, &x, &y);
		put_instance(PROBE_FIRST_INSTANCE + k, (float)x, (float)y);
	}
}

// Whether one of the pixels around x, y holds want.
static bool held_near(uint32_t x, uint32_t y, uint32_t want)
{
	for (uint32_t n = 0; n < 9; n++) {
		const uint32_t px = x + n % 3 - 1;
		const uint32_t py = y + n / 3 - 1;
		if (n != 4 && px < WIDTH && py < HEIGHT && image[tiled_index(px, py, WIDTH)] == want)
			return true;
	}
	return false;
}

static void check_point(Tally *t, const Blocks *b, uint32_t step, uint32_t k, uint32_t j)
{
	uint32_t x;
	uint32_t y;
	point_pixel(b, k, j, &x, &y);
	const uint32_t want = instance_stored(PROBE_FIRST_INSTANCE + k, j % COLOUR_CYCLE);
	const uint32_t held = image[tiled_index(x, y, WIDTH)];
	if (held == want) {
		t->right++;
		return;
	}
	if (held_near(x, y, want))
		t->near++;
	else
		t->wrong++;
	if (t->near + t->wrong <= WRONG_LOGGED)
		append(t->text, " index %u, instance %u, vertex %u, at (%u,%u) held %08X, wanted %08X;",
			(unsigned)(k * step + j), (unsigned)k, (unsigned)j, (unsigned)x, (unsigned)y, (unsigned)held,
			(unsigned)want);
}

// One count's points drawn, resolved and each checked where the CPU says it lands, its index's instance k and vertex
// j: auto-indexed of vertex_count each, or from copies; false if the fence didn't land.
static bool draw_points(
	const char *what, uint32_t vertex_count, uint32_t instance_count, const Copies *copies, uint32_t *differed)
{
	xsdk_log_write("instancing probe: case 3: about to draw %s", what);
	const Blocks b = blocks_of(vertex_count);
	put_points(vertex_count, instance_count, &b);
	set_draw_state(&instanced_shader);
	hand_streams(vertex_count, PROBE_FIRST_INSTANCE + instance_count);
	if (copies)
		draw_copies(XSDK_PRIMITIVE_POINT_LIST, copies, PROBE_FIRST_INSTANCE, instance_count);
	else
		draw_auto(XSDK_PRIMITIVE_POINT_LIST, vertex_count, PROBE_FIRST_INSTANCE, instance_count, instance_count);
	if (!resolve_and_wait(3))
		return false;
	Tally t = { 0 };
	for (uint32_t k = 0; k < instance_count; k++) {
		for (uint32_t j = 0; j < vertex_count; j++)
			check_point(&t, &b, copies ? copies->step : vertex_count, k, j);
	}
	const uint32_t points = vertex_count * instance_count;
	xsdk_log_write("instancing probe: case 3: %s: %s, %u of %u points right, %u a pixel off, %u elsewhere%s%s", what,
		t.right == points ? "PASS" : "DIFFERS", (unsigned)t.right, (unsigned)points, (unsigned)t.near,
		(unsigned)t.wrong, t.text[0] ? "; first wrong:" : "", t.text);
	*differed += t.right != points;
	return true;
}

// Case 3: as many instances as one packet holds at each count, at most 65,535 points a draw, each point read once
// unless it's wrong; 14 draws.
static Result run_division(void)
{
	const uint32_t auto_counts = sizeof probe_auto_counts / sizeof probe_auto_counts[0];
	const uint32_t forms = sizeof probe_indexed_forms / sizeof probe_indexed_forms[0];
	uint32_t differed = 0;
	char what[TEXT_BYTES];
	for (uint32_t c = 0; c < auto_counts; c++) {
		const uint32_t v = probe_auto_counts[c];
		const uint32_t n = MAX_DRAW / v;
		snprintf(what, sizeof what, "V %u, auto-indexed: %u instances from %u, %u points", (unsigned)v, (unsigned)n,
			(unsigned)PROBE_FIRST_INSTANCE, (unsigned)(v * n));
		if (!draw_points(what, v, n, NULL, &differed))
			return STOPPED;
	}
	for (uint32_t f = 0; f < forms; f++) {
		const uint32_t *const form = probe_indexed_forms[f];
		const Copies copies = make_copies(NULL, form[0], form[1], form[2], true);
		snprintf(what, sizeof what, "S %u, indexed: %u copies of %u, 32-bit, indices to %u", (unsigned)form[1],
			(unsigned)form[2], (unsigned)form[0], (unsigned)((form[2] - 1) * form[1] + form[0] - 1));
		if (!draw_points(what, form[0], form[2], &copies, &differed))
			return STOPPED;
	}
	xsdk_log_write("instancing probe: case 3: %s, %u of %u counts right", differed ? "DIFFERS" : "PASS",
		(unsigned)(auto_counts + forms - differed), (unsigned)(auto_counts + forms));
	return differed ? DIFFERED : PASSED;
}

// vkbench's cube (vulkan4xenon:bench/assets.c), as tests/probe_gpu_draw_cost.c builds it: corner c of each face.
static void cube_corner(uint32_t vertex, float out[3])
{
	static const float faces[6][3][3] = {
		{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
		{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
		{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
	};
	const float (*const face)[3] = faces[vertex / 4];
	const float su = vertex & 1 ? 1.0f : -1.0f;
	const float sv = vertex & 2 ? 1.0f : -1.0f;
	for (uint32_t axis = 0; axis < 3; axis++)
		out[axis] = 0.5f * (face[0][axis] + su * face[1][axis] + sv * face[2][axis]);
}

// A cube vertex 4 pixels across, z as probe_gpu_draw_cost's matrix puts it.
static void put_cube_vertex(uint32_t n, uint32_t vertex)
{
	float p[3];
	cube_corner(vertex, p);
	put_vertex(n, p[0] * SHAPE_PIXELS, p[1] * SHAPE_PIXELS, 0.5f + 0.25f * p[2], CHECK);
}

// Case 4's meshes in stream 0, and each instance at its cell's centre.
static void put_cost_meshes(void)
{
	for (uint32_t n = 0; n < CUBE_INDICES; n++)
		cube_indices[n] = n / QUAD_VERTICES * QUAD_CORNERS + quad_indices[n % QUAD_VERTICES];
	for (uint32_t v = 0; v < CUBE_VERTICES; v++)
		put_cube_vertex(CUBE_AT + v, v);
	for (uint32_t n = 0; n < CUBE_INDICES; n++)
		put_cube_vertex(CORNERS_AT + n, cube_indices[n]);
	for (uint32_t v = 0; v < QUAD_VERTICES; v++)
		put_vertex(QUAD_AT + v, (quad_corners[v][0] - 0.5f) * SHAPE_PIXELS, (quad_corners[v][1] - 0.5f) * SHAPE_PIXELS,
			0.5f, CHECK);
	for (uint32_t v = 0; v < QUAD_CORNERS; v++)
		put_vertex(QUAD_CORNERS_AT + v, (corner_places[v][0] - 0.5f) * SHAPE_PIXELS,
			(corner_places[v][1] - 0.5f) * SHAPE_PIXELS, 0.5f, CHECK);
	for (uint32_t e = 0; e < LARGE; e++)
		put_instance(e, e % COST_COLUMNS * COST_CELL_WIDTH + COST_CELL_WIDTH / 2,
			e / COST_COLUMNS * COST_CELL_HEIGHT + COST_CELL_HEIGHT / 2);
	hand_to_gpu(vertices, (QUAD_CORNERS_AT + QUAD_CORNERS) * ELEMENT_BYTES);
	hand_to_gpu(instances, LARGE * ELEMENT_BYTES);
}

static bool indexed(const CostVariant *v)
{
	return v->way == WAY_COPIES || (v->way == WAY_LAYER && v->cube);
}

static Mesh cost_mesh(const CostVariant *v)
{
	if (v->cube)
		return indexed(v) ? (Mesh){ CUBE_AT, CUBE_VERTICES } : (Mesh){ CORNERS_AT, CUBE_INDICES };
	return indexed(v) ? (Mesh){ QUAD_CORNERS_AT, QUAD_CORNERS } : (Mesh){ QUAD_AT, QUAD_VERTICES };
}

static Copies cost_copies(const CostVariant *v)
{
	if (!indexed(v))
		return (Copies){ 0 };
	return v->cube ? make_copies(cube_indices, CUBE_INDICES, CUBE_STEP, v->copies, false)
				   : make_copies(quad_indices, QUAD_VERTICES, QUAD_STEP, v->copies, false);
}

static void cost_state(const CostVariant *v)
{
	const Mesh m = cost_mesh(v);
	set_draw_state(v->way == WAY_LAYER ? &plain_shader : &instanced_shader);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(vertices) + m.at * ELEMENT_BYTES, m.vertices * ELEMENT_BYTES);
	xsdk_gpu_set_vertices(1, xsdk_gpu_address(instances), LARGE * ELEMENT_BYTES);
}

// As vulkan4xenon draws an instance now (vulkan4xenon:src/queue.c, draw_instance and draw_indexed_instance):
// gl_InstanceIndex's group, the stream at the instance's rate pointed at its element, and the draw.
static void record_layer(const CostVariant *v, const Copies *copies, uint32_t count)
{
	const uint32_t base = xsdk_gpu_address(instances);
	for (uint32_t e = 0; e < count; e++) {
		const float index[4] = { (float)e, (float)e, (float)e, (float)e };
		xsdk_gpu_set_vertex_constants(PROBE_GROUP, index, 1);
		xsdk_gpu_set_vertices(1, base + e * ELEMENT_BYTES, ELEMENT_BYTES);
		if (v->cube)
			xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, copies->address, CUBE_INDICES, false);
		else
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

static void record_instances(const CostVariant *v, const Copies *copies, uint32_t count)
{
	const uint32_t vertex_count = cost_mesh(v).vertices;
	if (v->way == WAY_LAYER)
		record_layer(v, copies, count);
	else if (v->way == WAY_GROUP)
		draw_auto(XSDK_PRIMITIVE_TRIANGLE_LIST, vertex_count, 0, count, 1);
	else if (v->way == WAY_ONE_DRAW)
		draw_auto(XSDK_PRIMITIVE_TRIANGLE_LIST, vertex_count, 0, count, MAX_DRAW / vertex_count);
	else
		draw_copies(XSDK_PRIMITIVE_TRIANGLE_LIST, copies, 0, count);
}

// The large batch recorded RECORDED_ROUNDS times, the GPU never running it, as tests/probe_draw_cost.c times the CPU:
// the best in ticks. The last recording stays for the GPU; then the small batch.
static bool record_batches(const CostVariant *v, const Copies *copies, XsdkCommandBuffer buffers[SIZES], uint64_t *best)
{
	for (uint32_t round = 0; round < RECORDED_ROUNDS; round++) {
		xsdk_gpu_record_begin(&buffers[1]);
		const uint64_t start = time_base();
		record_instances(v, copies, LARGE);
		const uint64_t ticks = time_base() - start;
		if (!NT_SUCCESS(xsdk_gpu_record_end()))
			return false;
		if (round == 0 || ticks < *best)
			*best = ticks;
	}
	xsdk_gpu_record_begin(&buffers[0]);
	record_instances(v, copies, SMALL);
	return NT_SUCCESS(xsdk_gpu_record_end());
}

// eDRAM cleared through a resolve, the state set and the GPU idle; then the batch, timed from its kick to its fence, as
// tests/probe_gpu_draw_cost.c times it.
static bool time_batch(const CostVariant *v, const XsdkCommandBuffer *buffer, uint64_t *ticks)
{
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	cost_state(v);
	if (!wait_logged(4))
		return false;
	const uint64_t start = time_base();
	xsdk_gpu_execute(buffer);
	const uint32_t fence = xsdk_gpu_fence();
	while (!xsdk_gpu_fence_done(fence)) {
		if (time_base() - start > (uint64_t)FENCE_TIMEOUT_MS * TICKS_PER_MS) {
			xsdk_log_write("instancing probe: case 4: %s: the batch's fence didn't land", v->name);
			probe_report_cp("instancing probe", "after the hang");
			return false;
		}
	}
	*ticks = time_base() - start;
	return true;
}

// The cells whose centre shows its instance's colour, after the large batch.
static uint32_t cells_drawn(void)
{
	uint32_t drawn = 0;
	for (uint32_t e = 0; e < LARGE; e++) {
		const uint32_t x = e % COST_COLUMNS * COST_CELL_WIDTH + COST_CELL_WIDTH / 2;
		const uint32_t y = e / COST_COLUMNS * COST_CELL_HEIGHT + COST_CELL_HEIGHT / 2;
		drawn += image[tiled_index(x, y, WIDTH)] == instance_stored(e, CHECK);
	}
	return drawn;
}

static double microseconds(uint64_t ticks)
{
	return (double)ticks * 1000.0 / TICKS_PER_MS;
}

static Result cost_variant(const CostVariant *v, XsdkCommandBuffer buffers[SIZES])
{
	xsdk_log_write("instancing probe: case 4: timing %s", v->name);
	const Copies copies = cost_copies(v);
	uint64_t recorded = 0;
	// The way's streams set first: a stream's fetch constant goes out with its whole group as xsdk keeps it, so the
	// layer's way, recording stream 1, records stream 0 as it stands (src/gpu_draw.c, xsdk_gpu_set_vertices).
	cost_state(v);
	if (!record_batches(v, &copies, buffers, &recorded)) {
		xsdk_log_write("instancing probe: case 4: %s: a batch outgrew its buffer", v->name);
		return DIFFERED;
	}
	const uint32_t dwords = buffers[1].count;
	uint64_t best[SIZES] = { 0 };
	for (uint32_t round = 0; round < TIMED_ROUNDS; round++) {
		for (uint32_t s = 0; s < SIZES; s++) {
			uint64_t ticks;
			if (!time_batch(v, &buffers[s], &ticks))
				return STOPPED;
			if (round == 0 || ticks < best[s])
				best[s] = ticks;
		}
	}
	if (!resolve_and_wait(4))
		return STOPPED;
	const uint32_t drawn = cells_drawn();
	const double small = microseconds(best[0]);
	const double large = microseconds(best[1]);
	const double clocks = (double)recorded * CLOCKS_PER_TICK / LARGE;
	xsdk_log_write(
		"instancing probe: case 4: %s: GPU %.3f us an instance, %.1f us for %u, %.1f for %u; CPU %.0f clocks an "
		"instance recorded (%.3f us), %.1f dwords; %u of %u cells drawn",
		v->name, (large - small) / (LARGE - SMALL), small, (unsigned)SMALL, large, (unsigned)LARGE, clocks,
		clocks / CLOCKS_PER_US, (double)dwords / LARGE, (unsigned)drawn, (unsigned)LARGE);
	return drawn == LARGE ? PASSED : DIFFERED;
}

static Result run_cost(void)
{
	put_cost_meshes();
	XsdkCommandBuffer buffers[SIZES];
	if (!NT_SUCCESS(xsdk_gpu_command_buffer_create(&buffers[0], SMALL * MAX_INSTANCE_DWORDS)) ||
		!NT_SUCCESS(xsdk_gpu_command_buffer_create(&buffers[1], LARGE * MAX_INSTANCE_DWORDS))) {
		xsdk_log_write("instancing probe: case 4: no memory for the batches");
		return DIFFERED;
	}
	uint32_t differed = 0;
	for (uint32_t v = 0; v < COST_VARIANTS; v++) {
		const Result r = cost_variant(&cost_variants[v], buffers);
		if (r == STOPPED)
			return STOPPED;
		differed += r == DIFFERED;
	}
	xsdk_gpu_command_buffer_free(&buffers[0]);
	xsdk_gpu_command_buffer_free(&buffers[1]);
	xsdk_log_write("instancing probe: case 4: %u ways timed, %u drew every cell", (unsigned)COST_VARIANTS,
		(unsigned)(COST_VARIANTS - differed));
	return differed ? DIFFERED : PASSED;
}

// Volatile 32-bit loads, summed so none is left out; a call, so the timing can't move past them.
__attribute__((noinline)) static uint32_t read_words(const volatile uint32_t *memory)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < READ_WORDS; i++)
		sum += memory[i];
	return sum;
}

// 16-byte VMX loads from a 16-byte aligned start; the barrier makes each call read the memory again.
__attribute__((noinline)) static uint32_t read_quads(const volatile uint32_t *memory)
{
	__asm__ volatile("" ::: "memory");
	const uint32_t *const words = (const uint32_t *)(uintptr_t)memory;
	__vector unsigned int sum = vec_splat_u32(0);
	for (uint32_t i = 0; i < READ_WORDS; i += 4)
		sum = vec_add(sum, vec_ld(0, &words[i]));
	uint32_t lanes[4] __attribute__((aligned(16)));
	vec_st(sum, 0, lanes);
	return lanes[0] + lanes[1] + lanes[2] + lanes[3];
}

// Case 5: what the layer would pay to read an app's indices from mapped write-combined memory.
static Result run_readback(void)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(READ_BYTES);
	if (!memory || ((uintptr_t)memory & 15)) {
		xsdk_log_write("instancing probe: case 5: no 16-byte aligned memory to read");
		return DIFFERED;
	}
	for (uint32_t i = 0; i < READ_WORDS; i++)
		memory[i] = i;
	__asm__ volatile("sync" ::: "memory");
	const uint32_t want = READ_WORDS * (READ_WORDS - 1) / 2;
	uint64_t best[2] = { 0 };
	bool sums = true;
	for (uint32_t round = 0; round < READ_ROUNDS; round++) {
		for (uint32_t way = 0; way < 2; way++) {
			const uint64_t start = time_base();
			const uint32_t sum = way ? read_quads(memory) : read_words(memory);
			const uint64_t ticks = time_base() - start;
			sums = sums && sum == want;
			if (round == 0 || ticks < best[way])
				best[way] = ticks;
		}
	}
	xsdk_gpu_free(memory);
	const double words_us = microseconds(best[0]);
	const double quads_us = microseconds(best[1]);
	xsdk_log_write(
		"instancing probe: case 5: 64 KB of write-combined memory, the best of %u: 32-bit loads %.1f MB/s (%.1f us), "
		"128-bit loads %.1f MB/s (%.1f us); %s",
		(unsigned)READ_ROUNDS, READ_BYTES / words_us, words_us, READ_BYTES / quads_us, quads_us,
		sums ? "every sum right" : "DIFFERS: a sum came out wrong");
	return sums ? PASSED : DIFFERED;
}

static const Case cases[CASES] = {
	{ "fetch", "r0 copied into r1 in an exec of ALU alone, 64 quads' positions and colours fetched at r1.x in the next",
		run_fetch },
	{ "prologue",
		"the design's prologue: 64 instances of a 6-vertex quad, then from a 4-vertex quad's 6 indices in 64 copies "
		"moved on by 4, each instance's cell and colour from stream 1",
		run_prologue },
	{ "division", "points, one a vertex, at the counts that leave the division the least room, every one checked",
		run_division },
	{ "cost", "GPU time over 500 and 2,500 instances and CPU clocks an instance recorded, each way", run_cost },
	{ "readback", "64 KB read from write-combined memory with 32-bit and 128-bit loads", run_readback },
};

// The case game:\instancing_start.txt names, 1 to CASES, or 0 for all of them.
static uint32_t start_case(void)
{
	FILE *const file = fopen(start_path, "r");
	if (!file)
		return 0;
	char name[16] = "";
	const bool read = fscanf(file, "%15s", name) == 1;
	fclose(file);
	for (uint32_t c = 0; read && c < CASES; c++) {
		if (!strcmp(name, cases[c].name) || (name[0] == (char)('1' + c) && !name[1]))
			return c + 1;
	}
	xsdk_log_write(
		"instancing probe: %s names no case: give 1 to %u, or fetch, prologue, division, cost or "
		"readback; running them all",
		start_path, (unsigned)CASES);
	return 0;
}

static bool make_resources(void)
{
	volatile uint32_t *const stream = xsdk_gpu_alloc((1 + STREAM_ELEMENTS) * ELEMENT_BYTES);
	vertices = stream ? stream + ELEMENT_FLOATS : NULL;
	instances = xsdk_gpu_alloc(STREAM_ELEMENTS * ELEMENT_BYTES);
	indices = xsdk_gpu_alloc(MAX_DRAW * sizeof(uint32_t));
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (vertices && instances && indices && image &&
		NT_SUCCESS(xsdk_shader_load(&copy_shader, probe_copy_shader, sizeof probe_copy_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&copy_shader, layout, 2, strides)) &&
		NT_SUCCESS(xsdk_shader_load(&instanced_shader, probe_instanced_shader, sizeof probe_instanced_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&instanced_shader, layout, 4, strides)) &&
		NT_SUCCESS(xsdk_shader_load(&plain_shader, probe_plain_shader, sizeof probe_plain_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&plain_shader, layout, 4, layer_strides)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)))
		return true;
	xsdk_log_write("instancing probe: FAILED, the buffers or the shaders couldn't be made");
	return false;
}

int main(void)
{
	const uint32_t alone = start_case();
	xsdk_log_write("instancing probe: %s%s; the group c%u, instances from %u in cases 2 and 3",
		alone ? "running only case " : "running every case", alone ? cases[alone - 1].name : "", (unsigned)PROBE_GROUP,
		(unsigned)PROBE_FIRST_INSTANCE);
	if (probe_break[0])
		xsdk_log_write("instancing probe: BREAK BUILD: %s", probe_break);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("instancing probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources())
		return 1;
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	if (!resolve_and_wait(0)) {
		xsdk_log_write("instancing probe: FAILED, the first clear's fence didn't land");
		return 1;
	}
	uint32_t passed = 0;
	uint32_t differed = 0;
	for (uint32_t c = 1; c <= CASES; c++) {
		if (alone && c != alone)
			continue;
		xsdk_log_write(
			"instancing probe: about to run case %u, %s: %s", (unsigned)c, cases[c - 1].name, cases[c - 1].what);
		const Result r = cases[c - 1].run();
		if (r == STOPPED) {
			// The GPU is wedged: no more GPU work, and the exit's xsdk_gpu_close only times out.
			xsdk_log_write(
				"instancing probe: FAILED, case %u stopped: the GPU didn't finish it; %u PASS, %u DIFFERS "
				"before it; to run another case alone, put its number in %s",
				(unsigned)c, (unsigned)passed, (unsigned)differed, start_path);
			return 1;
		}
		passed += r == PASSED;
		differed += r == DIFFERED;
	}
	status = xsdk_gpu_close();
	xsdk_log_write("instancing probe: done, %u cases: %u PASS, %u DIFFERS; xsdk_gpu_close 0x%08X",
		(unsigned)(passed + differed), (unsigned)passed, (unsigned)differed, (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
