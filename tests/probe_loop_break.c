#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Milestone 46 step 1 (docs/scope/46-loops.md): how the GPU runs hardware loops whose pixels or vertices leave at
// different passes, nested loops, a0 around a loop and a sample in a pass. game:\loopbrk_start.txt may name the case to
// start from, or a vertex case, which runs alone.

typedef enum { GRID_PIXELS, GRID_THIN, GRID_VERTICES, GRIDS } Grid;

typedef enum { CHECK_COUNTER, CHECK_GATED, CHECK_FIXED, CHECK_ROW, CHECK_NONE } Check;

typedef struct {
	const char *name;
	const char *what;
	const uint32_t *container;
	uint32_t bytes;
	// A vertex shader drawn with triangle_pixel_shader, else a pixel shader behind triangle_vertex_shader.
	bool vertex;
	// Whether LOOP_END breaks, so in Xenia's model the ungated count stops at B.
	bool breaks;
	Grid grid;
	Check check;
	// B = k . (px, py, base, 1) + j where px + py is odd; for a vertex shader k . (the quad's B, 0, 0, 1).
	float k[4];
	float j;
	// Each loop's count, outermost first.
	uint8_t counts[4];
	// What CHECK_FIXED wants every pixel to store.
	uint32_t fixed;
	bool timed;
	bool texture;
} ProbeDraw;

#include "probe_loop_break_cases.h"

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	CELL_PIXELS = 10,
	COLUMNS = WIDTH / CELL_PIXELS,
	ROWS = HEIGHT / CELL_PIXELS,
	CELLS = COLUMNS * ROWS,
	QUAD_VERTICES = 6,
	THIN_VERTICES = 3,
	// A position, then the interpolator or the quad's data.
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	THIN_ACROSS = 8,
	THIN_DOWN = 2,
	LEVELS = 4,
	// Pixel shaders' loop constants start at 16, as xost splits them (src/xsc_lower_flow.c:9-16).
	PIXEL_FIRST_LOOP = 16,
	FENCE_TIMEOUT_MS = 2000,
	CLEAR_ARGB = 0xFF202020,
	// What a resolve stores for the clear colour: blue, green, red, alpha from the top byte.
	CLEAR_STORED = 0x202020FF,
	CHECK = 0x5A,
	BYTE_MOST = 255,
	ROUNDS = 3,
	// The GPU's clock counts 498.75 a microsecond (docs/HARDWARE.md, "Queries and events").
	GPU_CLOCKS_PER_MS = 498750,
	TEXTURE_SIZE = 128,
	TEXTURE_LEVELS = 8,
	WINDOW_WIDTH = 40,
	WINDOW_HEIGHT = 20,
	WRONG_LOGGED = 4,
	TEXT_BYTES = 400,
	DRAW_COUNT = sizeof probe_draws / sizeof probe_draws[0],
};

// A draw's vertex count is 16 bits (xenia:src/xenia/gpu/registers.h:327, VGT_DRAW_INITIATOR's num_indices).
_Static_assert(CELLS *QUAD_VERTICES <= 0xFFFF, "the grid in one draw");

static const char *const start_path = "game:\\loopbrk_start.txt";

typedef enum {
	AS_MODEL,
	PAST,
	BEFORE,
	OTHER_B,
	WRONG_VALUES,
	MARKER,
	OTHER_ROW,
	OTHER,
	NOT_DRAWN,
	OUTCOMES,
} Outcome;

static const char *const outcome_names[OUTCOMES] = {
	"as Xenia's model",
	"more passes than its own",
	"fewer passes than its own",
	"its B not the probe's",
	"i or w not its own pass's",
	"the marker, nothing written",
	"another row",
	"something else",
	"the clear colour, not drawn",
};

// count - its own pass: span s holds differences below span_ends[s], the last the rest.
enum { SPANS = 6 };
static const char *const span_names[SPANS] = { "< -9", "-9..-1", "0", "1..9", "10..49", ">= 50" };
static const int32_t span_ends[SPANS - 1] = { -9, 0, 1, 10, 50 };

typedef struct {
	uint32_t outcomes[OUTCOMES];
	// The ungated count against its own pass, a counter's or a gated shape's n.
	uint32_t spans[SPANS];
	char wrong[TEXT_BYTES];
	uint32_t wrong_logged;
} Tally;

typedef struct {
	uint32_t passed;
	uint32_t differed;
} Totals;

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t b;
	uint32_t base;
} Point;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
};

// Each cell's corners in pixels: a quad's two triangles, and a thin triangle.
static const uint8_t quad_corners[QUAD_VERTICES][2] = {
	{ 0, 0 },
	{ CELL_PIXELS, 0 },
	{ 0, CELL_PIXELS },
	{ 0, CELL_PIXELS },
	{ CELL_PIXELS, 0 },
	{ CELL_PIXELS, CELL_PIXELS },
};
static const uint8_t thin_corners[THIN_VERTICES][2] = { { 0, 0 }, { THIN_ACROSS, 0 }, { 0, THIN_DOWN } };

static float constants[XSDK_PIXEL_CONSTANTS][4];
static volatile uint32_t *grids[GRIDS];
static volatile uint32_t *image;
static volatile uint32_t *clocks;
static XsdkShader pass_vertex;
static XsdkShader pass_pixel;
static XsdkTexture texture;
static XsdkSamplerState sampler;
static uint32_t level_texels[TEXTURE_SIZE * TEXTURE_SIZE];

// As tools/gen_loop_break_probe.py has them.
static uint32_t cell_base(uint32_t column, uint32_t row)
{
	return 1 + (7 * column + 13 * row) % 101;
}

static uint32_t quad_break(uint32_t cell)
{
	return 1 + (37 * cell) % 200;
}

static float clip_x(uint32_t x)
{
	return (x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(uint32_t y)
{
	return 1.0f - (y - 0.5f) / (HEIGHT / 2.0f);
}

static uint32_t cell_vertices(Grid grid)
{
	return grid == GRID_THIN ? THIN_VERTICES : QUAD_VERTICES;
}

static uint32_t min_u32(uint32_t a, uint32_t b)
{
	return a < b ? a : b;
}

// The cells row by row from the top left: a position, then u and v across the cell, its base plus a half and 1.5, or
// for vertex shaders the quad's B, 0, 0 and 1.
static volatile uint32_t *make_grid(Grid grid)
{
	const uint32_t per_cell = cell_vertices(grid);
	volatile uint32_t *const memory = xsdk_gpu_alloc(CELLS * per_cell * STRIDE_BYTES);
	if (!memory)
		return NULL;
	for (uint32_t v = 0; v < CELLS * per_cell; v++) {
		const uint32_t cell = v / per_cell;
		const uint32_t column = cell % COLUMNS;
		const uint32_t row = cell / COLUMNS;
		const uint8_t *const corner = grid == GRID_THIN ? thin_corners[v % per_cell] : quad_corners[v % per_cell];
		const bool quad_data = grid == GRID_VERTICES;
		const float values[VERTEX_FLOATS] = { clip_x(column * CELL_PIXELS + corner[0]),
			clip_y(row * CELL_PIXELS + corner[1]), 0.0f, 1.0f, quad_data ? (float)quad_break(cell) : corner[0],
			quad_data ? 0.0f : corner[1], quad_data ? 0.0f : cell_base(column, row) + 0.5f, quad_data ? 1.0f : 1.5f };
		uint32_t words[VERTEX_FLOATS];
		memcpy(words, values, sizeof words);
		for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
			memory[v * VERTEX_FLOATS + i] = words[i];
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), CELLS * per_cell * STRIDE_BYTES);
	return memory;
}

// 128x128, 8 levels, level L all red 16 + 32L, green 1 << L, blue L ^ CHECK, as the generator's texel.
static bool make_texture(void)
{
	if (!NT_SUCCESS(
			xsdk_texture_create(&texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIZE, TEXTURE_SIZE, TEXTURE_LEVELS, true)))
		return false;
	for (uint32_t level = 0; level < TEXTURE_LEVELS; level++) {
		const uint32_t argb = 0xFF000000 | (16 + 32 * level) << 16 | (1u << level) << 8 | (level ^ CHECK);
		const uint32_t side = TEXTURE_SIZE >> level;
		for (uint32_t i = 0; i < side * side; i++)
			level_texels[i] = argb;
		xsdk_texture_upload(&texture, level, level_texels, side * sizeof level_texels[0]);
	}
	xsdk_gpu_invalidate(texture.address, texture.size_bytes);
	const XsdkSampler point_clamped = { .mag_filter = XSDK_FILTER_POINT,
		.min_filter = XSDK_FILTER_POINT,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_CLAMP,
		.address_v = XSDK_ADDRESS_CLAMP,
		.address_w = XSDK_ADDRESS_CLAMP };
	xsdk_texture_sampler_state(&sampler, &point_clamped);
	return true;
}

// c[n] below the fill rows holds n, 255 - n and n ^ CHECK, each a quarter into its 8-bit step, and w 1; then the
// generator's literals and the draw's k and j.
static void fill_constants(const ProbeDraw *d)
{
	memset(constants, 0, sizeof constants);
	for (uint32_t n = 0; n < PROBE_FILL_ROWS; n++) {
		constants[n][0] = (n + 0.25f) / 255.0f;
		constants[n][1] = (255 - n + 0.25f) / 255.0f;
		constants[n][2] = ((n ^ CHECK) + 0.25f) / 255.0f;
		constants[n][3] = 1.0f;
	}
	memcpy(constants[PROBE_LITERAL_FIRST], probe_literals, sizeof probe_literals);
	memcpy(constants[PROBE_ROW_K], d->k, sizeof d->k);
	// The shader adds this times frc((px + py) / 2), which is 0 or a half.
	for (uint32_t c = 0; c < 4; c++)
		constants[PROBE_ROW_J][c] = 2.0f * d->j;
}

static void set_state(const ProbeDraw *d, const XsdkShader *shader)
{
	fill_constants(d);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	if (d->vertex) {
		xsdk_gpu_set_vertex_constants(0, constants[0], XSDK_PIXEL_CONSTANTS);
		xsdk_gpu_set_shaders(shader, &pass_pixel);
	} else {
		xsdk_gpu_set_pixel_constants(0, constants[0], XSDK_PIXEL_CONSTANTS);
		xsdk_gpu_set_shaders(&pass_vertex, shader);
	}
	uint32_t loops[LEVELS];
	for (uint32_t l = 0; l < LEVELS; l++)
		loops[l] = xsdk_gpu_loop_constant(d->counts[l], 0, 1);
	xsdk_gpu_set_loop_constants(d->vertex ? 0 : PIXEL_FIRST_LOOP, loops, LEVELS);
	if (d->texture)
		xsdk_gpu_set_texture(PROBE_TEXTURE_SLOT, &texture, &sampler);
}

static void queue_draw(const ProbeDraw *d)
{
	const uint32_t vertices = CELLS * cell_vertices(d->grid);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(grids[d->grid]), vertices * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, vertices);
}

static bool wait_logged(const ProbeDraw *d)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("loop break probe: case %s: the fence didn't land: 0x%08X", d->name, (unsigned)status);
	probe_report_cp("loop break probe", "after the hang");
	return false;
}

// The GPU's clocks for one draw, the best of ROUNDS, its clock copied with the GPU idle before and after.
static bool time_draw(const ProbeDraw *d, uint64_t *best)
{
	const uint32_t address = xsdk_gpu_address(clocks);
	for (uint32_t round = 0; round < ROUNDS; round++) {
		xsdk_gpu_write_clock(address, true);
		queue_draw(d);
		xsdk_gpu_write_clock(address + XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t), true);
		if (!wait_logged(d))
			return false;
		const uint64_t taken = xsdk_gpu_clock_value(clocks + XSDK_GPU_CLOCK_WORDS) - xsdk_gpu_clock_value(clocks);
		if (round == 0 || taken < *best)
			*best = taken;
	}
	return true;
}

// The draw, timed if asked, then the resolve; false, logged, if a fence didn't land.
static bool draw_and_wait(const ProbeDraw *d, const XsdkShader *shader, uint64_t *best)
{
	set_state(d, shader);
	if (d->timed) {
		if (!time_draw(d, best))
			return false;
	} else {
		queue_draw(d);
	}
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	return wait_logged(d);
}

static uint32_t point_count(const ProbeDraw *d)
{
	return d->grid == GRID_VERTICES ? CELLS : PIXELS;
}

// Every pixel, or a vertex shader's quad centres, with the B the generator gives it.
static Point point_at(const ProbeDraw *d, uint32_t n)
{
	if (d->grid == GRID_VERTICES) {
		const uint32_t x = n % COLUMNS * CELL_PIXELS + CELL_PIXELS / 2;
		const uint32_t y = n / COLUMNS * CELL_PIXELS + CELL_PIXELS / 2;
		return (Point){ x, y, (uint32_t)(d->k[0] * quad_break(n) + d->k[3]), 0 };
	}
	const uint32_t x = n % WIDTH;
	const uint32_t y = n / WIDTH;
	const uint32_t px = x % CELL_PIXELS;
	const uint32_t py = y % CELL_PIXELS;
	const uint32_t base = cell_base(x / CELL_PIXELS, y / CELL_PIXELS);
	const float b = d->k[0] * px + d->k[1] * py + d->k[2] * base + d->k[3] + ((px + py) % 2 ? d->j : 0.0f);
	return (Point){ x, y, (uint32_t)b, base };
}

// A resolved pixel's export bytes, red, green, blue and alpha, as a resolve stores them.
static uint32_t red(uint32_t stored)
{
	return stored >> 8 & 0xFF;
}

static uint32_t green(uint32_t stored)
{
	return stored >> 16 & 0xFF;
}

static uint32_t blue(uint32_t stored)
{
	return stored >> 24;
}

static uint32_t alpha(uint32_t stored)
{
	return stored & 0xFF;
}

static void add_span(Tally *t, uint32_t count, uint32_t own)
{
	const int32_t difference = (int32_t)count - (int32_t)own;
	uint32_t span = 0;
	while (span < SPANS - 1 && difference >= span_ends[span])
		span++;
	t->spans[span]++;
}

static Outcome count_outcome(uint32_t count, uint32_t own)
{
	return count == own ? AS_MODEL : count > own ? PAST : BEFORE;
}

// A counter's bytes: its count, its B, CHECK and 255; its own pass is B, or the loop's count if fewer.
static Outcome counter_outcome(const ProbeDraw *d, uint32_t stored, const Point *p, Tally *t)
{
	if (blue(stored) != CHECK || alpha(stored) != BYTE_MOST)
		return OTHER;
	if (green(stored) != min_u32(p->b, BYTE_MOST))
		return OTHER_B;
	const uint32_t own = min_u32(p->b, d->counts[0]);
	add_span(t, red(stored), own);
	return count_outcome(red(stored), own);
}

// The gated shape's i, w, B and ungated n: i B and w B + PROBE_GATED_W whatever the GPU's passes, n B if it breaks.
static Outcome gated_outcome(const ProbeDraw *d, uint32_t stored, const Point *p, Tally *t)
{
	if (blue(stored) != p->b)
		return OTHER_B;
	add_span(t, alpha(stored), d->breaks ? p->b : d->counts[0]);
	return red(stored) == p->b && green(stored) == p->b + PROBE_GATED_W ? AS_MODEL : WRONG_VALUES;
}

// c[n] as the a0 shapes read it: red n, green 255 - n, blue n ^ CHECK, alpha 255.
static Outcome row_outcome(uint32_t stored, const Point *p)
{
	if (stored == 0)
		return MARKER;
	const uint32_t n = red(stored);
	if (green(stored) != 255 - n || blue(stored) != (n ^ CHECK) || alpha(stored) != BYTE_MOST)
		return OTHER;
	return n == PROBE_A0_ROW + p->base ? AS_MODEL : OTHER_ROW;
}

static Outcome outcome(const ProbeDraw *d, uint32_t stored, const Point *p, Tally *t)
{
	if (stored == CLEAR_STORED)
		return NOT_DRAWN;
	switch (d->check) {
	case CHECK_COUNTER:
		return counter_outcome(d, stored, p, t);
	case CHECK_GATED:
		return gated_outcome(d, stored, p, t);
	case CHECK_ROW:
		return row_outcome(stored, p);
	default:
		return stored == d->fixed ? AS_MODEL : OTHER;
	}
}

static void check_points(const ProbeDraw *d, Tally *t)
{
	for (uint32_t n = 0; n < point_count(d); n++) {
		const Point p = point_at(d, n);
		const uint32_t stored = image[tiled_index(p.x, p.y, WIDTH)];
		const Outcome o = outcome(d, stored, &p, t);
		t->outcomes[o]++;
		if (o == AS_MODEL || t->wrong_logged++ >= WRONG_LOGGED)
			continue;
		const size_t used = strlen(t->wrong);
		snprintf(t->wrong + used, sizeof t->wrong - used, " (%u,%u) B %u held %08X;", (unsigned)p.x, (unsigned)p.y,
			(unsigned)p.b, (unsigned)stored);
	}
}

static void append(char *text, size_t size, const char *format, uint32_t a, const char *name)
{
	const size_t used = strlen(text);
	snprintf(text + used, size - used, format, (unsigned)a, name);
}

// Which way the counts lean: each pixel's own pass, held back by its vector, or leaving with its first pixel.
static const char *counter_verdict(const Tally *t)
{
	const uint32_t *const o = t->outcomes;
	if (o[OTHER_B] || o[OTHER] || o[NOT_DRAWN])
		return "FAILED: points whose B or bytes aren't the shader's as the probe works them out";
	if (!o[PAST] && !o[BEFORE])
		return "each pixel its own pass, as Xenia breaks each one (xenia:src/xenia/gpu/spirv_shader_translator.cc:986)";
	if (!o[BEFORE])
		return "held back, never early: a vector runs till its last pixel leaves (xenia:src/xenia/gpu/ucode.h:372)";
	if (!o[PAST])
		return "left early, never held back: a vector leaves with its first pixel";
	return "FAILED: no model matches, both more and fewer passes than their own";
}

// The share of aligned tiles of each size whose pixels all hold one count, to show which pixels share a vector.
static void log_tiles(void)
{
	static const uint8_t sizes[][2] = { { 2, 2 }, { 4, 4 }, { 8, 8 }, { 16, 16 }, { 16, 4 }, { 4, 16 }, { 32, 2 } };
	char text[TEXT_BYTES] = "";
	for (uint32_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		const uint32_t w = sizes[s][0];
		const uint32_t h = sizes[s][1];
		uint32_t same = 0;
		for (uint32_t ty = 0; ty < HEIGHT / h; ty++) {
			for (uint32_t tx = 0; tx < WIDTH / w; tx++) {
				const uint32_t first = red(image[tiled_index(tx * w, ty * h, WIDTH)]);
				bool all = true;
				for (uint32_t i = 0; i < w * h && all; i++)
					all = red(image[tiled_index(tx * w + i % w, ty * h + i / w, WIDTH)]) == first;
				same += all;
			}
		}
		const size_t used = strlen(text);
		snprintf(text + used, sizeof text - used, " %ux%u %u%%;", (unsigned)w, (unsigned)h,
			(unsigned)(same * 100 / (WIDTH / w * (HEIGHT / h))));
	}
	xsdk_log_write("loop break probe: case 1: aligned tiles whose pixels all hold one count:%s", text);
}

// The counts over the top left's window, a row a line, to see the shapes vectors take.
static void log_window(void)
{
	xsdk_log_write("loop break probe: case 1: counts over pixels (0,0) to (%u,%u), B there px + 10 py + base:",
		(unsigned)WINDOW_WIDTH - 1, (unsigned)WINDOW_HEIGHT - 1);
	for (uint32_t y = 0; y < WINDOW_HEIGHT; y++) {
		char line[WINDOW_WIDTH * 4 + 1] = "";
		for (uint32_t x = 0; x < WINDOW_WIDTH; x++)
			snprintf(line + 4 * x, sizeof line - 4 * x, "%3u ", (unsigned)red(image[tiled_index(x, y, WIDTH)]));
		xsdk_log_write("loop break probe: case 1: y %2u: %s", (unsigned)y, line);
	}
}

// One line of outcomes and the verdict, then the first wrong points; true if the draw passed.
static bool log_result(const ProbeDraw *d, const Tally *t, uint64_t clocks_taken)
{
	char text[TEXT_BYTES] = "";
	for (uint32_t o = AS_MODEL; o < OUTCOMES; o++) {
		if (t->outcomes[o])
			append(text, sizeof text, "; %u %s", t->outcomes[o], outcome_names[o]);
	}
	char timing[64] = "";
	if (d->timed)
		snprintf(timing, sizeof timing, "%.1f us, ", (double)clocks_taken * 1000.0 / GPU_CLOCKS_PER_MS);
	const uint32_t points = point_count(d);
	bool passed = t->outcomes[AS_MODEL] == points;
	const char *verdict = passed ? "PASS" : "DIFFERS";
	if (d->check == CHECK_COUNTER) {
		verdict = counter_verdict(t);
		passed = strncmp(verdict, "FAILED", 6) != 0;
	} else if (d->check == CHECK_NONE) {
		verdict = "timed only";
		passed = true;
	}
	xsdk_log_write("loop break probe: case %s: %s%u points%s; %s", d->name, timing, (unsigned)points, text, verdict);
	if (d->check == CHECK_COUNTER || d->check == CHECK_GATED) {
		char spans[TEXT_BYTES] = "";
		for (uint32_t s = 0; s < SPANS; s++)
			append(spans, sizeof spans, " %u at %s;", t->spans[s], span_names[s]);
		xsdk_log_write("loop break probe: case %s: ungated passes less its own pass:%s", d->name, spans);
	}
	if (t->wrong_logged && d->check != CHECK_NONE)
		xsdk_log_write("loop break probe: case %s: first points not as the model:%s", d->name, t->wrong);
	return passed;
}

// False if the GPU didn't finish the draw, after which it's wedged and the shader stays loaded.
static bool run_draw(const ProbeDraw *d, Totals *totals)
{
	xsdk_log_write("loop break probe: about to draw case %s: %s", d->name, d->what);
	XsdkShader shader;
	NTSTATUS status = xsdk_shader_load(&shader, d->container, d->bytes);
	if (NT_SUCCESS(status) && d->vertex)
		status = xsdk_shader_set_vertex_layout(
			&shader, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES });
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("loop break probe: case %s: its shader didn't load: 0x%08X; skipped", d->name, (unsigned)status);
		totals->differed++;
		return true;
	}
	uint64_t clocks_taken = 0;
	if (!draw_and_wait(d, &shader, &clocks_taken))
		return false;
	xsdk_shader_free(&shader);
	Tally t = { 0 };
	if (d->check != CHECK_NONE)
		check_points(d, &t);
	const bool passed = log_result(d, &t, clocks_taken);
	if (d->check == CHECK_COUNTER && !d->timed && !d->vertex) {
		log_tiles();
		log_window();
	}
	totals->passed += passed;
	totals->differed += !passed;
	return true;
}

// The draw game:\loopbrk_start.txt names, or the first of the pixel case it numbers; else the first.
static uint32_t start_draw(void)
{
	FILE *const file = fopen(start_path, "r");
	if (!file)
		return 0;
	char name[8] = "";
	const bool read = fscanf(file, "%7s", name) == 1;
	fclose(file);
	const size_t length = strlen(name);
	for (uint32_t n = 0; read && n < DRAW_COUNT; n++) {
		const char *const draw = probe_draws[n].name;
		if (!strcmp(draw, name) || (!probe_draws[n].vertex && !strncmp(draw, name, length) && draw[length] >= 'a'))
			return n;
	}
	xsdk_log_write(
		"loop break probe: %s names no case: give a pixel case's number, 1 to 5 or 7, or a draw's name, "
		"as 2b or 6a; starting at case 1",
		start_path);
	return 0;
}

static void log_setup(uint32_t start)
{
	const bool alone = probe_draws[start].vertex;
	xsdk_log_write("loop break probe: %u draws; %s case %s%s; each over 128 by 72 quads of 10 pixels",
		(unsigned)DRAW_COUNT, alone ? "running only" : "starting at", probe_draws[start].name,
		alone ? ", a vertex case" : ", then the pixel cases after it");
	xsdk_log_write(
		"loop break probe: a pixel leaves at pass B = k . (px, py, base, 1), plus j where px + py is odd, its place "
		"px, py in its quad and base 1 + (7 column + 13 row) mod 101; a vertex quad's B is 1 + 37 cell mod 200");
	xsdk_log_write(
		"loop break probe: the colours' alloc comes first in every pixel shader, so no alloc lies between an "
		"a0 load and its read; a vertex shader's allocs all come after its loop");
	if (probe_break[0])
		xsdk_log_write("loop break probe: BREAK BUILD: %s", probe_break);
}

// Loads the pass-through shaders and makes the grids, the image, the clock's words and the texture; false, logged, on
// a failure.
static bool make_resources(void)
{
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	clocks = xsdk_gpu_alloc(2 * XSDK_GPU_CLOCK_WORDS * sizeof(uint32_t));
	bool made = image && clocks && make_texture();
	for (uint32_t g = 0; g < GRIDS && made; g++) {
		grids[g] = make_grid((Grid)g);
		made = grids[g] != NULL;
	}
	if (made && NT_SUCCESS(xsdk_shader_load(&pass_vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&pass_vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_load(&pass_pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)))
		return true;
	xsdk_log_write("loop break probe: FAILED, the buffers, the texture or the shaders couldn't be made");
	return false;
}

// Clears eDRAM so a pixel no draw reaches holds the clear colour; false if the GPU didn't finish.
static bool clear_target(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("loop break probe: FAILED, the first clear's fence didn't land: 0x%08X", (unsigned)status);
	probe_report_cp("loop break probe", "after the clear");
	return false;
}

int main(void)
{
	const uint32_t start = start_draw();
	log_setup(start);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("loop break probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources() || !clear_target())
		return 1;
	Totals t = { 0 };
	for (uint32_t n = start; n < DRAW_COUNT && (n == start || !probe_draws[n].vertex); n++) {
		if (run_draw(&probe_draws[n], &t))
			continue;
		// The GPU is wedged: no more GPU work, and the exit's xsdk_gpu_close only times out.
		const bool next = n + 1 < DRAW_COUNT && !probe_draws[n + 1].vertex && !probe_draws[n].vertex;
		xsdk_log_write(
			"loop break probe: FAILED, case %s stopped: the GPU didn't finish it; %u PASS, %u DIFFERS "
			"before it%s%s%s%s",
			probe_draws[n].name, (unsigned)t.passed, (unsigned)t.differed, next ? "; to go on past it, put " : "",
			next ? probe_draws[n + 1].name : "", next ? " in " : "", next ? start_path : "");
		return 1;
	}
	status = xsdk_gpu_close();
	xsdk_log_write("loop break probe: done, %u draws: %u PASS, %u DIFFERS or FAILED; xsdk_gpu_close 0x%08X",
		(unsigned)(t.passed + t.differed), (unsigned)t.passed, (unsigned)t.differed, (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
