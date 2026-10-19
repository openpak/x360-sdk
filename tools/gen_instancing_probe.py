#!/usr/bin/env python3
"""gen_instancing_probe.py <out.h>: the vertex shaders for probe_instancing, milestone 47's console probe of instancing
(docs/scope/47-instancing.md, step 1).

Each shader is built by hand with xost's encoders:
- the copy shader (case 1): r0 copied into r1 in an exec of ALU alone, then a position and a colour fetched at r1.x;
- the instanced shader (cases 2 to 4): the scope's prologue, k = floor(r0.x * c.x + c.y), t.x = r0.x - c.z * k and
  t.y = k + c.w, in an exec of ALU alone; then stream 0's position and colour fetched at t.x and stream 1's, the
  instance's, at t.y; the position and the colour exported as the sums of the two streams';
- the plain shader (case 4's way the layer draws an instance now): the same fetches, all at r0.x, stream 1 pointed at
  the instance's element with a stride of 0 (docs/HARDWARE.md, "Instances and long strips, vulkan4xenon milestone 24").

Case 6 draws with xost's own code instead: vk_instance_rate.vert from xost's testdata, a corner at the vertex's rate and
a place and a tint at the instance's, compiled plain, as the layer drew it before milestone 47, and instanced, as it
draws it now (vulkan4xenon:tools/build_shaders.py), the optimiser on, as xsdk_test's instanced shaders are.

The generator stops unless xost's simulator (xenos-toolchain/xdis/simulate.py) gives each case's vertices the exports
the probe expects: the fetch callback gives an element whose numbers name the stream and the element the fetch
floored its index to, so the exports say which vertex and which instance each fetch read. Every vertex of case 1 and
case 2, and for case 3 each count's first and last instances and vertices, the two closest to a wrong floor. It stops
too unless each break build fails a check on the simulator.
"""
import math
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import gen_loop_break_probe as loops  # noqa: E402
import gen_triangle_shaders as tri  # noqa: E402
import gen_xsc_probe as pack  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
import xsc.native as native  # noqa: E402
from gen_triangle_shaders import ce, enc, xu  # noqa: E402
from gen_loop_break_probe import Op, alu, splat, temp  # noqa: E402
from xasm.program import XSDK_REGISTERS  # noqa: E402

V = xu.AluVectorOpcode
# r0 holds the vertex's index (xenia:src/xenia/gpu/dxbc_shader_translator.cc:346-374); then k, t and the four fetched.
INDEX, K, T, VERTEX_POSITION, VERTEX_COLOR, INSTANCE_POSITION, INSTANCE_COLOR = range(7)
REGISTERS = INSTANCE_COLOR + 1
# The vertex constant the group takes: 1/S, 0.5/S or 0, S and the piece's first instance.
GROUP = 0
FIRST_INSTANCE = 5
X, Y = 0, 1
# Case 2's 64 instances: a 6-vertex quad, and a 4-vertex quad's 6 indices in copies moved on by 4.
GRID_INSTANCES = 64
QUAD_VERTICES = 6
QUAD_INDICES = (0, 1, 2, 2, 1, 3)
QUAD_STEP = 4
# A draw packet counts 16 bits of vertices or indices (xenia:src/xenia/gpu/registers.h:327).
MAX_DRAW = 65535
# Case 3's counts that leave the division the least room (docs/scope/47-instancing.md, step 1), each as many instances
# as one packet holds.
AUTO_COUNTS = (3, 7, 255, 257, 4369, 21845, 65535)
# The indexed form's vertices a copy, the copies' step S and the copies, the values reaching to 2^24 - 1, the most a
# vertex index holds (xenia:src/xenia/gpu/xenos.h:38-42).
INDEXED_FORMS = ((2, 2, 32767), (3, 4, 21845), (129, 256, 508), (2049, 4096, 31), (5, 1 << 16, 255),
                 (16, 1 << 20, 16), (1, 1 << 23, 2))
assert all(v * n <= MAX_DRAW for v in AUTO_COUNTS for n in (MAX_DRAW // v,))
assert all(v <= s and v * k <= MAX_DRAW and (k - 1) * s + v <= 1 << 24 for v, s, k in INDEXED_FORMS)
BREAKS = {
    "nofloor": "the prologue without its floor",
    "pervtx": "the instance's stream fetched at t.x, the vertex's index",
    "nostep": "the copies of the indices not moved on by S",
}
# Breaks of the shader; nostep is the probe's own, in how it writes the copies.
SHADER_BREAKS = ("nofloor", "pervtx")
RATE_SPIRV = ROOT / "xenos-toolchain" / "xsc" / "testdata" / "vk_instance_rate.vert.spv"


def const(row, swizzle=0, negate=False):
    return row, False, swizzle, negate


def vfetch(dest, source, component, serialize=False):
    """A template vertex fetch from stream 0's constant, as tri.fetch, at source's x or y (src_swizzle picks one
    component, xenia:src/xenia/gpu/ucode.h:758, shader_interpreter.cc:1007-1015); xsdk's patch keeps the source."""
    words = enc.build_vfetch_full(
        src=source, src_swizzle=component, dest=dest, dest_swizzle=tri.DEST_XYZW, must_be_one=1,
        const_index=tri.VERTEX_FETCH_CONSTANT // 3, const_index_sel=tri.VERTEX_FETCH_CONSTANT % 3,
        data_format=tri.FORMAT_32_32_32_32_FLOAT, is_signed=False, is_normalized=False, stride=0, offset=0)
    fetch = xu.VertexFetchInstruction(*words)
    assert (fetch.src(), fetch.src_swizzle(), fetch.dest()) == (source, component, dest)
    assert not fetch.is_index_rounded()
    return Op(words, fetch=True, serialize=serialize, vertex_fetch=True)


def prologue(brk):
    """k = floor(r0.x * c.x + c.y), t.x = r0.x - c.z * k, t.y = k + c.w: the scope's division, by 1/S the CPU works out,
    so nothing here divides on the GPU and rcp's rounding (docs/HARDWARE.md, "Samples in a loop's pass, milestone 46
    step 6") doesn't touch it."""
    ops = [alu(V.kMad, K, 0xF, temp(INDEX, splat(X)), const(GROUP, splat(0)), const(GROUP, splat(1)))]
    if brk != "nofloor":
        ops.append(alu(V.kFloor, K, 0xF, temp(K)))
    return ops + [alu(V.kMad, T, 0x1, const(GROUP, splat(2), negate=True), temp(K), temp(INDEX)),
                  alu(V.kAdd, T, 0x2, temp(K), const(GROUP, splat(3)))]


def vertex_shader(alu_exec, fetches, exports):
    """An exec of ALU alone if any, the fetches' exec, then the position's and the interpolator's exports. The fetches
    lead their exec, its vertex cache mask one bit a fetch, as xost's shaders have them (docs/HARDWARE.md, "A fifth
    vertex fetch"); the first fetch reading what the ALU wrote and the first ALU instruction reading what the fetches
    wrote are serialized, as xost serializes them (docs/HARDWARE.md, "Serialized fetches", "Slow fetches across
    execs"; src/xsc_program.c, exec_entry)."""
    position, interpolator = exports
    items = ([("exec", alu_exec)] if alu_exec else []) + [
        ("exec", fetches), ("alloc", tri.ALLOC_POSITION), ("exec", [position]), ("alloc", tri.ALLOC_INTERPOLATORS),
        ("exec", [interpolator])]
    dwords, first = loops.control_flow(items)
    usages = {VERTEX_POSITION: (tri.USAGE_POSITION, 0), VERTEX_COLOR: (ce.USAGE_COLOR, 0),
              INSTANCE_POSITION: (ce.USAGE_TEXCOORD, 0), INSTANCE_COLOR: (ce.USAGE_TEXCOORD, 1)}
    elements = [ce.VertexElement(first + len(alu_exec) + n, *usages[xu.VertexFetchInstruction(*op.words).dest()])
                for n, op in enumerate(fetches)]
    blob = ce.build_container(
        "vertex", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(vs_num_reg=REGISTERS - 1, vs_resource=1, vs_export_count=0),
        context_misc=0, interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        vertex_elements=elements, creator=tri.CREATOR)
    return list(struct.unpack(f">{len(blob) // 4}I", blob))


def copy_shader():
    copy = [alu(V.kMax, K, 0xF, temp(INDEX), temp(INDEX))]
    fetches = [vfetch(VERTEX_POSITION, K, X, serialize=True), vfetch(VERTEX_COLOR, K, X)]
    exports = (alu(V.kMax, tri.EXPORT_POSITION, 0xF, temp(VERTEX_POSITION), temp(VERTEX_POSITION), export=True,
                   serialize=True),
               alu(V.kMax, tri.EXPORT_INTERPOLATOR_0, 0xF, temp(VERTEX_COLOR), temp(VERTEX_COLOR), export=True))
    return vertex_shader(copy, fetches, exports)


def sums():
    """The position and the colour, each the vertex's plus the instance's."""
    return (alu(V.kAdd, tri.EXPORT_POSITION, 0xF, temp(VERTEX_POSITION), temp(INSTANCE_POSITION), export=True,
                serialize=True),
            alu(V.kAdd, tri.EXPORT_INTERPOLATOR_0, 0xF, temp(VERTEX_COLOR), temp(INSTANCE_COLOR), export=True))


def instanced_shader(brk):
    at = X if brk == "pervtx" else Y
    fetches = [vfetch(VERTEX_POSITION, T, X, serialize=True), vfetch(VERTEX_COLOR, T, X),
               vfetch(INSTANCE_POSITION, T, at), vfetch(INSTANCE_COLOR, T, at)]
    return vertex_shader(prologue(brk), fetches, sums())


def plain_shader():
    fetches = [vfetch(dest, INDEX, X) for dest in (VERTEX_POSITION, VERTEX_COLOR, INSTANCE_POSITION, INSTANCE_COLOR)]
    return vertex_shader([], fetches, sums())


def group(divisor):
    """The group's x to w for S: 1/S and 0.5/S as the probe works them out, in doubles rounded to floats, 0 for a power
    of two."""
    power = divisor & (divisor - 1) == 0
    return [simulate.single(1 / divisor), 0.0 if power else simulate.single(0.5 / divisor), float(divisor),
            float(FIRST_INSTANCE)]


def fetched(fetch, index):
    """What the callback gives a fetch: stream 0's element e as (e, 0, 0, 1) and colour (0, e, 0, 0), stream 1's as
    (0, 0, e, 0) and (0, 0, 0, e), e the index floored, as the fetch takes it (xenia:src/xenia/gpu/ucode.h:735-742)."""
    e = float(math.floor(index))
    return {VERTEX_POSITION: [e, 0.0, 0.0, 1.0], VERTEX_COLOR: [0.0, e, 0.0, 0.0],
            INSTANCE_POSITION: [0.0, 0.0, e, 0.0], INSTANCE_COLOR: [0.0, 0.0, 0.0, e]}[fetch.dest()]


def exports(words, index, divisor):
    blob = struct.pack(f">{len(words)}I", *words)
    result = simulate.run(blob, {INDEX: [float(index), 0.0, 0.0, 0.0]}, {GROUP: group(divisor)},
                          vertex_fetch=lambda n, fetch, i: fetched(fetch, i))
    return result.exports.get(tri.EXPORT_POSITION), result.exports.get(tri.EXPORT_INTERPOLATOR_0)


def wanted(vertex, instance):
    """The exports of a vertex of an instance: both streams' numbers, the instance's from the first instance on."""
    element = float(instance + FIRST_INSTANCE)
    return [float(vertex), 0.0, element, 1.0], [0.0, float(vertex), 0.0, element]


def ends(count):
    """A count's first two and last two, the places closest to a wrong floor."""
    return sorted({n for n in (0, 1, count // 2, count - 2, count - 1) if 0 <= n < count})


def checks(step=None):
    """Each check's name and its points: the index r0.x holds, S, and the vertex and instance it is. step moves the
    copies on, S unless a break says otherwise."""
    quad = [(i, QUAD_VERTICES, i % QUAD_VERTICES, i // QUAD_VERTICES) for i in range(GRID_INSTANCES * QUAD_VERTICES)]
    copies = [(c * (QUAD_STEP if step is None else step) + m, QUAD_STEP, m, c)
              for c in range(GRID_INSTANCES) for m in QUAD_INDICES]
    out = [("2 auto", quad), ("2 indexed", copies)]
    for v in AUTO_COUNTS:
        out.append((f"3 V {v}", [(k * v + j, v, j, k) for k in ends(MAX_DRAW // v) for j in ends(v)]))
    for v, s, n in INDEXED_FORMS:
        out.append((f"3 S {s}", [(k * (s if step is None else step) + j, s, j, k) for k in ends(n) for j in ends(v)]))
    return out


def failing(words, step=None):
    """The names of the checks whose points the simulator gives other exports than wanted."""
    names = []
    for name, points in checks(step):
        if any(exports(words, i, divisor) != wanted(vertex, instance) for i, divisor, vertex, instance in points):
            names.append(name)
    return names


def check_copy(words):
    """Stops unless the copy shader exports each vertex's own position and colour."""
    for i in range(GRID_INSTANCES * QUAD_VERTICES):
        if exports(words, i, 1) != ([float(i), 0.0, 0.0, 1.0], [0.0, float(i), 0.0, 0.0]):
            sys.exit(f"gen_instancing_probe: the copy shader reads another vertex than {i}")


def check_plain(words):
    """Stops unless the plain shader fetches both streams at the vertex's index, stream 1's stride 0 leaving its
    element the one the stream points at."""
    for i in (0, 5, 35):
        position, color = exports(words, i, 1)
        if position != [float(i), 0.0, float(i), 1.0] or color != [0.0, float(i), 0.0, float(i)]:
            sys.exit(f"gen_instancing_probe: the plain shader doesn't fetch at vertex {i}")


def rate_shaders():
    """Case 6's vk_instance_rate.vert, plain and instanced, as words, and the register the instanced one's group takes.
    Stops unless the plain one reads no constant, so the baseline's group, written where case 4 writes it, changes
    nothing it draws."""
    spirv = RATE_SPIRV.read_bytes()
    compiled = [native.compile_spirv(spirv, XSDK_REGISTERS.pixel, XSDK_REGISTERS.vertex, True, instanced=instanced)
                for instanced in (False, True)]
    (plain, plain_needs, plain_resources), (instanced, needs, resources) = compiled
    if plain_needs or plain_resources or needs or [r[0] for r in resources] != ["instance"]:
        sys.exit("gen_instancing_probe: vk_instance_rate.vert reads constants other than its group: "
                 f"{plain_needs} {plain_resources} {needs} {resources}")
    as_words = [list(struct.unpack(f">{len(c) // 4}I", c)) for c in (plain, instanced)]
    return as_words[0], as_words[1], resources[0][3]


def shader_lines(name, variants):
    """The shader's words, each shader break's own where it differs."""
    normal = variants[None]
    changed = [(brk, words) for brk, words in variants.items() if brk and words != normal]
    lines = []
    for n, (brk, words) in enumerate(changed):
        lines += [f"#{'el' if n else ''}if defined(INSTANCING_BREAK_{brk.upper()})",
                  f"static const uint32_t {name}[] = {{", pack.word_lines(words), "};"]
    lines += (["#else"] if changed else []) + [f"static const uint32_t {name}[] = {{", pack.word_lines(normal), "};"]
    return lines + (["#endif"] if changed else []) + [""]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    copy, plain = copy_shader(), plain_shader()
    instanced = {brk: instanced_shader(brk) for brk in (None, *SHADER_BREAKS)}
    check_copy(copy)
    check_plain(plain)
    wrong = failing(instanced[None])
    if wrong:
        sys.exit(f"gen_instancing_probe: on the simulator the instanced shader fails {', '.join(wrong)}")
    fails = {brk: failing(instanced[brk]) for brk in SHADER_BREAKS}
    fails["nostep"] = failing(instanced[None], step=0)
    for brk, names in fails.items():
        if not names:
            sys.exit(f"gen_instancing_probe: the {brk} break build fails no check on the simulator")
    rate_plain, rate_instanced, rate_group = rate_shaders()
    groups = f"PROBE_GROUP = {GROUP}, PROBE_FIRST_INSTANCE = {FIRST_INSTANCE}, PROBE_RATE_GROUP = {rate_group}"
    lines = ["// Generated by tools/gen_instancing_probe.py; don't edit.", "", f"enum {{ {groups} }};", "",
             "static const uint32_t probe_copy_shader[] = {", pack.word_lines(copy), "};", "",
             "static const uint32_t probe_plain_shader[] = {", pack.word_lines(plain), "};", "",
             "// xost's vk_instance_rate.vert, plain and instanced.",
             "static const uint32_t probe_rate_plain_shader[] = {", pack.word_lines(rate_plain), "};", "",
             "static const uint32_t probe_rate_instanced_shader[] = {", pack.word_lines(rate_instanced), "};", ""]
    lines += shader_lines("probe_instanced_shader", instanced)
    lines += ["static const uint32_t probe_auto_counts[] = { " + ", ".join(map(str, AUTO_COUNTS)) + " };", "",
              "// Vertices a copy, the copies' step S, and the copies.",
              "static const uint32_t probe_indexed_forms[][3] = {"]
    lines += [f"\t{{ {v}, {s}, {n} }}," for v, s, n in INDEXED_FORMS] + ["};", ""]
    for n, (brk, what) in enumerate(BREAKS.items()):
        lines += [f"#{'el' if n else ''}if defined(INSTANCING_BREAK_{brk.upper()})",
                  f'static const char probe_break[] = "{what}; on the simulator it fails {", ".join(fails[brk])}";']
    lines += ["#else", 'static const char probe_break[] = "";', "#endif", ""]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")


if __name__ == "__main__":
    main()
