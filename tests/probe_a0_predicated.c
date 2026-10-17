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

// How the GPU handles a0 in a pixel shader's code on the predicate, after xsdk_test drew xost's vk_dynamic_if.frag
// (3f35da3) wrong and hung the GPU. Each case's pixel shader draws a grid of quads, each with indices i, j and l of its
// own, the arms' condition true, false, and true over the top half then false below; each quad's centre names the
// constant it read, or for xost's program holds its colour. game:\a0pred_start.txt may name the case to start from.

enum {
	MODES = 2,
	COMBOS = 32,
};

typedef struct {
	const char *name;
	const uint32_t *container;
	uint32_t bytes;
	uint32_t rounds;
	// The model's reads in words, the condition true and false.
	const char *model[MODES];
	// The constant Xenia's model reads for each combination of indices, or PROBE_MARKER for nothing written; for
	// xost's program, NULL, and its colour and the colour if the arm's read at a0 wrote nothing instead.
	const int16_t (*reads)[COMBOS];
	const uint32_t (*argb)[COMBOS];
	const uint32_t (*silent)[COMBOS];
} ProbeCase;

#include "probe_a0_predicated_cases.h"

_Static_assert(sizeof probe_combos / sizeof probe_combos[0] == COMBOS, "the generator's combinations");

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	// The grid: 128 by 72 quads of 10 pixels, as the raster test's, each checked at its centre.
	CELL_PIXELS = 10,
	COLUMNS = WIDTH / CELL_PIXELS,
	ROWS = HEIGHT / CELL_PIXELS,
	CELLS = COLUMNS * ROWS,
	HALF_CELLS = CELLS / 2,
	QUAD_VERTICES = 6,
	// A position, then the interpolator: i + 0.5, j + 0.5, l + 0.5 and w.
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	GRID_BYTES = CELLS * QUAD_BYTES,
	GRID_VERTICES = CELLS * QUAD_VERTICES,
	// Cell n's combination in the varying layout; 7 rows on, so neighbours above and below differ too.
	ROW_STEP = 7,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	// What a resolve stores for the clear colour and for the marker, 0 from r0 - r0: blue, green, red, alpha.
	CLEAR_STORED = 0x202020FF,
	MARKER_STORED = 0,
	// c[n]'s z holds n ^ CHECK (tools/gen_a0_predicated_probe.py).
	CHECK = 0x5A,
	// The rows read at a0 are 16 apart from 32 to 143, each block's base a multiple of 32.
	A0_ROWS_FIRST = 32,
	A0_ROWS_END = 144,
	INDICES = 16,
	// A colour within this many steps a channel, as the raster test's colour_near.
	TOLERANCE = 2,
	CELLS_LOGGED = 4,
	CELL_TEXT_BYTES = 96,
	COUNTS_TEXT_BYTES = 512,
	LAYOUTS = 2,
	CASE_COUNT = sizeof probe_cases / sizeof probe_cases[0],
};

// A draw's vertex count is 16 bits (xenia:src/xenia/gpu/registers.h:327, VGT_DRAW_INITIATOR's num_indices).
_Static_assert(GRID_VERTICES <= 0xFFFF, "the grid in one draw");

static const char *const start_path = "game:\\a0pred_start.txt";

// The draws a round makes of a case's grid: the condition true, false, and both, true over the top half.
typedef enum { TAKEN, NOT_TAKEN, BOTH, DRAWS } Draw;

static const char *const draw_names[DRAWS] = { "true", "false", "true over the top half, false below" };
static const char *const layout_names[LAYOUTS] = { "one set of indices", "indices per quad" };

typedef enum {
	AS_MODEL,
	NOT_DRAWN,
	MARKER,
	AT_I,
	AT_J,
	AT_L,
	OTHER_CONSTANT,
	NO_CONSTANT,
	ARM_SILENT,
	OTHER_COLOUR,
	OUTCOMES,
} Outcome;

static const char *const outcome_names[OUTCOMES] = {
	"as the model",
	"the clear colour, not drawn",
	"the marker, nothing written",
	"a row at i",
	"a row at j",
	"a row at l",
	"another constant",
	"no constant",
	"the colour with the arm's read at a0 writing nothing",
	"another colour",
};

typedef struct {
	uint32_t passed;
	uint32_t differed;
} Totals;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 16 },
};

// The unit square's two triangles, each corner its left or right and top or bottom.
static const uint8_t corners[QUAD_VERTICES][2] = {
	{ 0, 0 },
	{ 1, 0 },
	{ 0, 1 },
	{ 0, 1 },
	{ 1, 0 },
	{ 1, 1 },
};

static float constants[XSDK_FLOAT_CONSTANTS][4];
static volatile uint32_t *grids[LAYOUTS];
static volatile uint32_t *image;
static XsdkShader vertex;
// Each draw's constants hold n plus a shift of its own, so a constant an earlier draw left reads as another.
static uint32_t shift;

static float clip_x(uint32_t column)
{
	return (column - 0.5f) / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(uint32_t row)
{
	return 1.0f - (row - 0.5f) / (HEIGHT / 2.0f);
}

static uint32_t cell_combo(uint32_t layout_index, uint32_t cell)
{
	return layout_index ? (cell % COLUMNS + ROW_STEP * (cell / COLUMNS)) % COMBOS : PROBE_UNIFORM_COMBO;
}

// The cells row by row from the top left, each quad's interpolator its combination's.
static volatile uint32_t *make_grid(uint32_t layout_index)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(GRID_BYTES);
	if (!memory)
		return NULL;
	for (uint32_t v = 0; v < GRID_VERTICES; v++) {
		const uint32_t cell = v / QUAD_VERTICES;
		const uint32_t x = cell % COLUMNS * CELL_PIXELS + corners[v % QUAD_VERTICES][0] * CELL_PIXELS;
		const uint32_t y = cell / COLUMNS * CELL_PIXELS + corners[v % QUAD_VERTICES][1] * CELL_PIXELS;
		const float *const combo = probe_combos[cell_combo(layout_index, cell)];
		const float values[VERTEX_FLOATS] = { clip_x(x), clip_y(y), 0.0f, 1.0f, combo[0], combo[1], combo[2],
			combo[3] };
		uint32_t words[VERTEX_FLOATS];
		memcpy(words, values, sizeof words);
		for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
			memory[v * VERTEX_FLOATS + i] = words[i];
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), GRID_BYTES);
	return memory;
}

// c[n] holds m = n + shift, its low 8 bits: m, 255 - m and m ^ CHECK, each a quarter into its 8-bit step, and w 1,
// as tools/gen_relative_constant_probe.py fills them; xost's program gets the raster test's rows instead.
static void fill_constants(const ProbeCase *c)
{
	if (c->reads) {
		for (uint32_t n = 0; n < XSDK_FLOAT_CONSTANTS; n++) {
			const uint32_t m = (n + shift) & 0xFF;
			constants[n][0] = (m + 0.25f) / 255.0f;
			constants[n][1] = (255 - m + 0.25f) / 255.0f;
			constants[n][2] = ((m ^ CHECK) + 0.25f) / 255.0f;
			constants[n][3] = 1.0f;
		}
		return;
	}
	memset(constants, 0, sizeof constants);
	memcpy(constants, probe_xost_rows, sizeof probe_xost_rows);
}

static void set_mode(const ProbeCase *c, uint32_t mode)
{
	const uint32_t row = c->reads ? PROBE_MODE_ROW : PROBE_XOST_MODE_ROW;
	memcpy(constants[row], c->reads ? probe_mode_values[mode] : probe_xost_modes[mode], sizeof constants[row]);
}

// The constant a pixel holds under the draw's shift, or -1 for none whole; a resolve stores blue, green, red,
// alpha from the top byte, as tests/probe_constants.c reads it.
static int constant_read(uint32_t stored)
{
	const uint32_t m = stored >> 8 & 0xFF;
	const bool whole = (stored >> 16 & 0xFF) == 255 - m && stored >> 24 == (m ^ CHECK) && (stored & 0xFF) == 0xFF;
	return whole ? (int)((m - shift) & 0xFF) : -1;
}

static int register_read(uint32_t stored)
{
	return stored == MARKER_STORED ? PROBE_MARKER : constant_read(stored);
}

static Outcome decoded_outcome(uint32_t stored, int want, const uint8_t *indices)
{
	const int got = register_read(stored);
	if (got == want)
		return AS_MODEL;
	if (stored == CLEAR_STORED)
		return NOT_DRAWN;
	if (got == PROBE_MARKER)
		return MARKER;
	if (got < 0)
		return NO_CONSTANT;
	const uint32_t row = (uint32_t)got;
	if (row < A0_ROWS_FIRST || row >= A0_ROWS_END || row / INDICES % 2)
		return OTHER_CONSTANT;
	const uint32_t index = row % INDICES;
	return index == indices[0] ? AT_I : index == indices[1] ? AT_J : index == indices[2] ? AT_L : OTHER_CONSTANT;
}

// Whether a resolved pixel, its bytes B, G, R and A, is within TOLERANCE of the colour a channel.
static bool colour_near(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	bool near = true;
	for (uint32_t shift_bits = 8; shift_bits <= 24; shift_bits += 8) {
		const uint32_t got = stored >> shift_bits & 0xFF;
		const uint32_t wanted = want >> shift_bits & 0xFF;
		near &= (got > wanted ? got - wanted : wanted - got) <= TOLERANCE;
	}
	return near;
}

static Outcome colour_outcome(uint32_t stored, uint32_t argb, uint32_t silent)
{
	if (colour_near(stored, argb))
		return AS_MODEL;
	if (stored == CLEAR_STORED)
		return NOT_DRAWN;
	return colour_near(stored, silent) ? ARM_SILENT : OTHER_COLOUR;
}

static void append_register(char *text, size_t size, int reg)
{
	const size_t used = strlen(text);
	if (reg == PROBE_MARKER)
		snprintf(text + used, size - used, "marker");
	else if (reg < 0)
		snprintf(text + used, size - used, "none");
	else
		snprintf(text + used, size - used, "c%d", reg);
}

// One wrong cell for the log: where, its indices, what the model has it read and what the pixel held.
static void append_cell(
	char *text, size_t size, const ProbeCase *c, uint32_t cell, uint32_t mode, uint32_t combo, uint32_t stored)
{
	const uint8_t *const indices = probe_combo_indices[combo];
	size_t used = strlen(text);
	snprintf(text + used, size - used, " (%u,%u) i %u j %u l %u: model ", (unsigned)(cell % COLUMNS),
		(unsigned)(cell / COLUMNS), (unsigned)indices[0], (unsigned)indices[1], (unsigned)indices[2]);
	used = strlen(text);
	if (!c->reads) {
		snprintf(text + used, size - used, "%08X, arm silent %08X, held %08X;",
			(unsigned)__builtin_bswap32(c->argb[mode][combo]), (unsigned)__builtin_bswap32(c->silent[mode][combo]),
			(unsigned)stored);
		return;
	}
	append_register(text, size, c->reads[mode][combo]);
	used = strlen(text);
	snprintf(text + used, size - used, ", read ");
	append_register(text, size, stored == CLEAR_STORED ? -1 : register_read(stored));
	used = strlen(text);
	snprintf(text + used, size - used, ", held %08X;", (unsigned)stored);
}

// Counts each cell's outcome in the draw just resolved; the first wrong cells go into text.
static void check_cells(
	const ProbeCase *c, uint32_t layout_index, Draw draw, uint32_t counts[OUTCOMES], char *text, size_t size)
{
	uint32_t logged = 0;
	for (uint32_t cell = 0; cell < CELLS; cell++) {
		const uint32_t mode = draw == BOTH ? cell >= HALF_CELLS : draw == NOT_TAKEN;
		const uint32_t combo = cell_combo(layout_index, cell);
		const uint32_t x = cell % COLUMNS * CELL_PIXELS + CELL_PIXELS / 2;
		const uint32_t y = cell / COLUMNS * CELL_PIXELS + CELL_PIXELS / 2;
		const uint32_t stored = image[tiled_index(x, y, WIDTH)];
		const Outcome outcome = c->reads ? decoded_outcome(stored, c->reads[mode][combo], probe_combo_indices[combo])
										 : colour_outcome(stored, c->argb[mode][combo], c->silent[mode][combo]);
		counts[outcome]++;
		if (outcome != AS_MODEL && logged++ < CELLS_LOGGED)
			append_cell(text, size, c, cell, mode, combo, stored);
	}
}

static void log_result(uint32_t number, Draw draw, uint32_t layout_index, uint32_t round, const ProbeCase *c,
	const uint32_t counts[OUTCOMES], const char *cells)
{
	char text[COUNTS_TEXT_BYTES] = "";
	for (uint32_t o = AS_MODEL + 1; o < OUTCOMES; o++) {
		const size_t used = strlen(text);
		if (counts[o])
			snprintf(text + used, sizeof text - used, "; %u %s", (unsigned)counts[o], outcome_names[o]);
	}
	const bool same = counts[AS_MODEL] == CELLS;
	char model[COUNTS_TEXT_BYTES];
	if (draw == BOTH)
		snprintf(model, sizeof model, "%s over the top half, %s below", c->model[0], c->model[1]);
	else
		snprintf(model, sizeof model, "%s", c->model[draw]);
	xsdk_log_write("a0 predicated probe: case %u [%s, %s] round %u: model %s; %u of %u quads as it, %s%s",
		(unsigned)number, draw_names[draw], layout_names[layout_index], (unsigned)round, model,
		(unsigned)counts[AS_MODEL], (unsigned)CELLS, same ? "PASS" : "DIFFERS", text);
	if (!same)
		xsdk_log_write("a0 predicated probe: case %u [%s, %s] round %u, first wrong quads:%s", (unsigned)number,
			draw_names[draw], layout_names[layout_index], (unsigned)round, cells);
}

static void draw_cells(uint32_t layout_index, uint32_t first, uint32_t count)
{
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(grids[layout_index]) + first * QUAD_BYTES, count * QUAD_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, count * QUAD_VERTICES);
}

// The draw, or for BOTH the two, then the resolve; false, logged, if the fence didn't land.
static bool draw_and_wait(const ProbeCase *c, const XsdkShader *pixel, uint32_t layout_index, Draw draw)
{
	fill_constants(c);
	set_mode(c, draw == NOT_TAKEN);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_pixel_constants(0, constants[0], XSDK_FLOAT_CONSTANTS);
	xsdk_gpu_set_shaders(&vertex, pixel);
	if (draw == BOTH) {
		draw_cells(layout_index, 0, HALF_CELLS);
		set_mode(c, 1);
		const uint32_t row = c->reads ? PROBE_MODE_ROW : PROBE_XOST_MODE_ROW;
		xsdk_gpu_set_pixel_constants(row, constants[row], 1);
		draw_cells(layout_index, HALF_CELLS, CELLS - HALF_CELLS);
	} else {
		draw_cells(layout_index, 0, CELLS);
	}
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("a0 predicated probe: the fence didn't land: 0x%08X", (unsigned)status);
	probe_report_cp("a0 predicated probe", "after the hang");
	return false;
}

// One round's draw of the case; false if the GPU didn't finish it.
static bool run_draw(const ProbeCase *c, uint32_t number, const XsdkShader *pixel, uint32_t layout_index, Draw draw,
	uint32_t round, Totals *t)
{
	xsdk_log_write("a0 predicated probe: about to draw case %u [%s, %s] round %u of %u: %s", (unsigned)number,
		draw_names[draw], layout_names[layout_index], (unsigned)round, (unsigned)c->rounds, c->name);
	shift++;
	if (!draw_and_wait(c, pixel, layout_index, draw))
		return false;
	uint32_t counts[OUTCOMES] = { 0 };
	char cells[CELLS_LOGGED * CELL_TEXT_BYTES] = "";
	check_cells(c, layout_index, draw, counts, cells, sizeof cells);
	log_result(number, draw, layout_index, round, c, counts, cells);
	t->passed += counts[AS_MODEL] == CELLS;
	t->differed += counts[AS_MODEL] != CELLS;
	return true;
}

// False if the GPU didn't finish a draw, after which it's wedged and the shader stays loaded.
static bool run_case(const ProbeCase *c, uint32_t number, Totals *t)
{
	XsdkShader pixel;
	if (!NT_SUCCESS(xsdk_shader_load(&pixel, c->container, c->bytes))) {
		xsdk_log_write(
			"a0 predicated probe: case %u, %s: its container didn't load; skipped", (unsigned)number, c->name);
		return true;
	}
	for (uint32_t round = 1; round <= c->rounds; round++) {
		for (uint32_t l = 0; l < LAYOUTS; l++) {
			for (uint32_t d = 0; d < DRAWS; d++) {
				if (!run_draw(c, number, &pixel, l, (Draw)d, round, t))
					return false;
			}
		}
	}
	xsdk_shader_free(&pixel);
	return true;
}

// The case game:\a0pred_start.txt names, from 1, or the first.
static uint32_t start_case(void)
{
	FILE *const file = fopen(start_path, "r");
	if (!file)
		return 1;
	unsigned number = 0;
	const bool read = fscanf(file, "%u", &number) == 1;
	fclose(file);
	if (read && number >= 1 && number <= CASE_COUNT)
		return number;
	xsdk_log_write(
		"a0 predicated probe: %s doesn't hold a case from 1 to %u; starting at 1", start_path, (unsigned)CASE_COUNT);
	return 1;
}

static void log_setup(uint32_t start)
{
	xsdk_log_write(
		"a0 predicated probe: %u cases from case %u; each a grid of %u by %u quads of %u pixels, a draw "
		"each with the arms' condition %s, %s, and %s, with %s and with %s, every quad's centre checked",
		(unsigned)CASE_COUNT, (unsigned)start, (unsigned)COLUMNS, (unsigned)ROWS, (unsigned)CELL_PIXELS,
		draw_names[TAKEN], draw_names[NOT_TAKEN], draw_names[BOTH], layout_names[0], layout_names[1]);
	xsdk_log_write(
		"a0 predicated probe: c[n] draws as red n, green 255 - n, blue n ^ 0x%02X; the marker, 0, means "
		"nothing wrote the result; one set of indices is i %u, j %u, l %u",
		(unsigned)CHECK, (unsigned)probe_combo_indices[PROBE_UNIFORM_COMBO][0],
		(unsigned)probe_combo_indices[PROBE_UNIFORM_COMBO][1], (unsigned)probe_combo_indices[PROBE_UNIFORM_COMBO][2]);
}

// Loads the vertex shader and makes the grids and the image; false, logged, on a failure.
static bool make_resources(void)
{
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	for (uint32_t l = 0; l < LAYOUTS; l++)
		grids[l] = make_grid(l);
	if (image && grids[0] && grids[1] &&
		NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES })))
		return true;
	xsdk_log_write("a0 predicated probe: FAILED, the buffers or the vertex shader couldn't be made");
	return false;
}

// Clears eDRAM so a quad no draw reaches holds the clear colour; false if the GPU didn't finish.
static bool clear_target(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (NT_SUCCESS(status))
		return true;
	xsdk_log_write("a0 predicated probe: FAILED, the first clear's fence didn't land: 0x%08X", (unsigned)status);
	probe_report_cp("a0 predicated probe", "after the clear");
	return false;
}

int main(void)
{
	const uint32_t start = start_case();
	log_setup(start);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("a0 predicated probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources() || !clear_target())
		return 1;
	Totals t = { 0 };
	for (uint32_t number = start; number <= CASE_COUNT; number++) {
		if (!run_case(&probe_cases[number - 1], number, &t)) {
			// The GPU is wedged: no more GPU work, and the exit's xsdk_gpu_close only times out.
			xsdk_log_write(
				"a0 predicated probe: FAILED, the GPU didn't finish case %u; %u PASS, %u DIFFERS before "
				"it; to go on past it, put %u in %s",
				(unsigned)number, (unsigned)t.passed, (unsigned)t.differed, (unsigned)(number + 1), start_path);
			return 1;
		}
	}
	status = xsdk_gpu_close();
	xsdk_log_write("a0 predicated probe: done, %u draws: %u PASS, %u DIFFERS; xsdk_gpu_close 0x%08X",
		(unsigned)(t.passed + t.differed), (unsigned)t.passed, (unsigned)t.differed, (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
