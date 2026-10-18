#include "xsdk/gpu.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "gpu_private.h"
#include "xsdk/log.h"

// Tiled passes at one sample (docs/scope/44-tiling-and-msaa.md, "Tiled passes in xsdk"), on what the console showed
// of the window offset, the moved resolve and depth, and predication (docs/HARDWARE.md, "Tiling's registers and
// packets, milestone 44 step 1").

enum {
	// The window offset's and the scissors' y from bit 16, the offset 15 bits signed
	// (xenia:src/xenia/gpu/registers.h:586-633).
	Y_SHIFT = 16,
	OFFSET_MASK = 0x7FFF,
	// Inside the window offset's 15 bits and the 8192-texel textures (xenia:src/xenia/gpu/xenos.h:1139-1141).
	MAX_SIDE = 8192,
	// D3D's screen scissor, which xsdk's set-up leaves (src/gpu_setup.c).
	SCREEN_WHOLE = 8192,
	// A bin register's two words, sent high first as Xenia reads them; the console gave the same either way round.
	BIN_DWORDS = 3,
	WINDOW_DWORDS = 2 + 3,
	// Hierarchical depth stays on for a band's depth up to the 1280x1024 milestone 30 checked, half of eDRAM.
	HIZ_MOST_TILES = XSDK_EDRAM_TILES / 2,
};

static const uint64_t ALL_BINS = UINT64_MAX;

// The pass being recorded or last queued: how its bands and targets lie, and whether a draw narrowed its bins.
static bool tiling;
static XsdkTiling pass;
static XsdkCommandBuffer *pass_buffer;
static uint32_t band_rows;
static XsdkColorTarget placed[XSDK_COLOR_TARGETS];
static bool narrowed;
// The pass whose last band's resolve left the band clear, to that depth, while nothing has changed eDRAM since.
static XsdkTiling cleared;
static uint32_t cleared_rows;
static uint32_t cleared_depth;
static bool band_clear;

bool xsdk_gpu_tiling(void)
{
	return tiling;
}

void xsdk_gpu_tiles_forget(void)
{
	band_clear = false;
}

// A band of rows: its colour targets and its depth, or the whole depth that stays.
static uint32_t band_tiles(const XsdkTiling *t, uint32_t rows)
{
	uint32_t tiles = t->depth ? xsdk_gpu_target_tiles(t->width, t->depth_whole ? t->height : rows) : 0;
	for (uint32_t i = 0; i < t->count; i++)
		tiles += xsdk_gpu_color_target_tiles(t->width, rows, t->targets[i].format);
	return tiles;
}

// As few bands as fit, each as many rows, rounded up to a multiple of XSDK_TILE_ROWS; 0 if none fit.
static uint32_t rows_of(const XsdkTiling *t)
{
	if (t->band_rows)
		return t->band_rows;
	for (uint32_t bands = 1; bands <= XSDK_TILE_BANDS; bands++) {
		const uint32_t rows = ((t->height + bands - 1) / bands + XSDK_TILE_ROWS - 1) / XSDK_TILE_ROWS * XSDK_TILE_ROWS;
		if (band_tiles(t, rows) <= XSDK_EDRAM_TILES)
			return rows;
	}
	return 0;
}

uint32_t xsdk_gpu_tiling_bands(const XsdkTiling *t)
{
	const bool valid = t->samples == 1 && t->width && t->width <= MAX_SIDE && t->height && t->height <= MAX_SIDE &&
		t->count <= XSDK_COLOR_TARGETS && (t->count || t->depth) && (t->depth || !t->depth_whole) &&
		t->band_rows % XSDK_TILE_ROWS == 0;
	const uint32_t rows = valid ? rows_of(t) : 0;
	const uint32_t bands = rows ? (t->height + rows - 1) / rows : 0;
	return bands <= XSDK_TILE_BANDS && rows && band_tiles(t, rows) <= XSDK_EDRAM_TILES ? bands : 0;
}

static uint32_t band_count(void)
{
	return (pass.height + band_rows - 1) / band_rows;
}

static uint32_t rows_in(uint32_t band)
{
	const uint32_t top = band * band_rows;
	return pass.height - top < band_rows ? pass.height - top : band_rows;
}

// The depth from tile 0, the band's or the whole of it, then the colour targets one after another.
static void place_targets(void)
{
	uint32_t tile = pass.depth ? xsdk_gpu_target_tiles(pass.width, pass.depth_whole ? pass.height : band_rows) : 0;
	for (uint32_t i = 0; i < pass.count; i++) {
		placed[i] = (XsdkColorTarget){ .format = pass.targets[i].format, .tile = tile };
		tile += xsdk_gpu_color_target_tiles(pass.width, band_rows, pass.targets[i].format);
	}
}

// Depth whole moves its base down by the band's rows.
static uint32_t depth_tile_of(uint32_t band)
{
	return pass.depth_whole ? band * xsdk_gpu_target_tiles(pass.width, band_rows) : 0;
}

NTSTATUS xsdk_gpu_begin_tiles(const XsdkTiling *t, XsdkCommandBuffer *buffer)
{
	assert(!tiling);
	if (t->samples == 2 || t->samples == 4)
		return STATUS_NOT_IMPLEMENTED;
	if (!xsdk_gpu_tiling_bands(t)) {
		xsdk_log_write("gpu: a %ux%u pass of %u colour targets%s can't be tiled", (unsigned)t->width,
			(unsigned)t->height, (unsigned)t->count, t->depth ? " and depth" : "");
		return STATUS_INVALID_PARAMETER;
	}
	pass = *t;
	band_rows = rows_of(t);
	narrowed = false;
	place_targets();
	xsdk_gpu_set_band_targets(pass.width, pass.height, band_rows, placed, pass.count, 0, false);
	xsdk_gpu_record_begin(buffer);
	pass_buffer = buffer;
	tiling = true;
	// Each band's resolve clears its depth with the hierarchical buffer, so the recording can test it.
	const uint32_t depth_tiles = xsdk_gpu_target_tiles(pass.width, band_rows);
	if (pass.depth && !pass.depth_whole && depth_tiles <= HIZ_MOST_TILES)
		xsdk_gpu_hiz_cleared(pass.width, band_rows, xsdk_gpu_depth_clear());
	return STATUS_SUCCESS;
}

static void put_bins(GpuPackets *packets, uint32_t opcode, uint64_t bins)
{
	gpu_put_type3(packets, opcode, 2);
	gpu_put(packets, (uint32_t)(bins >> 32));
	gpu_put(packets, (uint32_t)bins);
}

// The draws after carry the predicate bit under a bin mask of the bands the rows cross.
void xsdk_gpu_set_draw_rows(uint32_t top, uint32_t bottom)
{
	assert(tiling && top < bottom && bottom <= pass.height);
	const uint32_t first = top / band_rows;
	const uint32_t last = (bottom - 1) / band_rows;
	const bool every = first == 0 && last == band_count() - 1;
	xsdk_gpu_predicate_draws(!every);
	if (every)
		return;
	GpuPackets packets = gpu_reserve_packets(BIN_DWORDS);
	put_bins(&packets, PM4_SET_BIN_MASK, ((2ull << last) - 1) & ~((1ull << first) - 1));
	gpu_packets_done(&packets);
	narrowed = true;
}

static bool same_layout(void)
{
	bool same = band_clear && cleared_rows == band_rows && cleared_depth == xsdk_gpu_depth_clear() &&
		cleared.width == pass.width && cleared.height == pass.height && cleared.count == pass.count &&
		cleared.depth == pass.depth && cleared.depth_whole == pass.depth_whole;
	for (uint32_t i = 0; same && i < pass.count; i++)
		same = cleared.targets[i].format == pass.targets[i].format;
	return same;
}

// A band's resolves, the last clearing depth too unless it stays whole.
static void resolve_band(const XsdkTileResolve *resolves, uint32_t count, uint32_t band, bool clear)
{
	for (uint32_t i = 0; i < count; i++) {
		const XsdkTileResolve *const r = &resolves[i];
		assert(r->target < pass.count || (r->target == XSDK_TILE_DEPTH && pass.depth));
		assert(r->texture->width == pass.width && r->texture->height == pass.height);
		const bool depth = clear && pass.depth && !pass.depth_whole && i == count - 1;
		xsdk_gpu_resolve_band(r->target, r->texture, r->clear_rgba, band * band_rows, rows_in(band), clear, depth);
	}
}

// The band's targets, its rows moved up to eDRAM's top with the screen scissor at their size, as Mesa draws a tile
// (mesa:src/gallium/drivers/freedreno/a2xx/fd2_gmem.c:684-688, 711-713), gl_FragCoord's top, and the band's bin.
static void put_band(uint32_t band)
{
	const uint32_t top = band * band_rows;
	xsdk_gpu_set_band_targets(pass.width, pass.height, band_rows, placed, pass.count, depth_tile_of(band), true);
#ifdef XSDK_TILES_BREAK_NOOFFSET
	// Break build: every band drawn at the target's own rows.
	const uint32_t window = 0;
#else
	const uint32_t window = ((uint32_t)-(int32_t)top & OFFSET_MASK) << Y_SHIFT;
#endif
	const uint32_t screen[] = { 0, SCREEN_WHOLE | rows_in(band) << Y_SHIFT };
	GpuPackets packets = gpu_reserve_packets(WINDOW_DWORDS + (narrowed ? BIN_DWORDS : 0));
	gpu_put_register(&packets, PA_SC_WINDOW_OFFSET, window);
	gpu_put_registers(&packets, PA_SC_SCREEN_SCISSOR_TL, screen, 2);
#ifdef XSDK_TILES_BREAK_NOSELECT
	// Break build: every bin selected, so draws narrowed to other bands run in each.
	if (narrowed)
		put_bins(&packets, PM4_SET_BIN_SELECT, ALL_BINS);
#else
	if (narrowed)
		put_bins(&packets, PM4_SET_BIN_SELECT, 1ull << band);
#endif
	gpu_packets_done(&packets);
#ifdef XSDK_TILES_BREAK_NOFRAGCOORD
	// Break build: gl_FragCoord's constant left at band 0's.
	xsdk_gpu_put_frag_coord_top(0);
#else
	xsdk_gpu_put_frag_coord_top(top);
#endif
}

// The screen scissor whole, the bins all ones as XAM leaves them, and gl_FragCoord's constant as outside a pass.
static void put_whole(void)
{
	const uint32_t screen[] = { 0, SCREEN_WHOLE | SCREEN_WHOLE << Y_SHIFT };
	GpuPackets packets = gpu_reserve_packets(3 + (narrowed ? 2 * BIN_DWORDS : 0));
	gpu_put_registers(&packets, PA_SC_SCREEN_SCISSOR_TL, screen, 2);
	if (narrowed) {
		put_bins(&packets, PM4_SET_BIN_MASK, ALL_BINS);
		put_bins(&packets, PM4_SET_BIN_SELECT, ALL_BINS);
	}
	gpu_packets_done(&packets);
	xsdk_gpu_put_frag_coord_top(0);
}

NTSTATUS xsdk_gpu_end_tiles(const XsdkTileResolve *resolves, uint32_t count)
{
	assert(tiling && count >= 1);
	tiling = false;
	xsdk_gpu_predicate_draws(false);
	const NTSTATUS status = xsdk_gpu_record_end();
	if (!NT_SUCCESS(status))
		return status;
	// Band 0's clear: its resolves of what eDRAM holds, which the band then draws over.
	if (!same_layout())
		resolve_band(resolves, count, 0, true);
#ifdef XSDK_TILES_BREAK_NOCLEAR
	// Break build: the bands' resolves leave colour and depth to the band after.
	const bool clear = false;
#else
	const bool clear = true;
#endif
	for (uint32_t band = 0; band < band_count(); band++) {
		put_band(band);
		xsdk_gpu_execute(pass_buffer);
		resolve_band(resolves, count, band, clear);
	}
	put_whole();
	cleared = pass;
	cleared_rows = band_rows;
	cleared_depth = xsdk_gpu_depth_clear();
	band_clear = true;
	return STATUS_SUCCESS;
}
