#!/usr/bin/env python3
"""gen_a0_predicated_probe.py <out.h>: the pixel shaders for probe_a0_predicated, the console probe of how the GPU
handles a0 in a pixel shader's code on the predicate. xsdk_test's raster test drew xost's vk_dynamic_if.frag, built at
3f35da3, wrong and then hung the GPU: on a uniform, one arm of an if/else, in execs on the predicate, loads a0 and reads
at it, and the other reads at the a0 loaded before the arms.

Each hand-assembled case varies one thing about that shape: where a0 is loaded and read, which arm, how the code is
predicated. A case reads c[k+a0] or a plain constant into r2 and exports it; r2 starts as r0 - r0, 0, the marker that
nothing wrote it, and the last plain constant read before the arms is STALE_ROW. c[n] holds n, 255 - n and n ^ CHECK,
so a pixel names the constant read. The arms' condition is MODE_ROW's x > 0, a uniform, or the cell's w. Two cases are
vk_dynamic_if.frag itself as xost compiles it without the fix (d8206eb) and with it (3f35da3), drawn with the raster
test's constants. The riskiest run last: shapes xost never writes, the condition each cell's own, and the fixed program
over 15 rounds.

The generator stops unless the builders write xost's words for vk_dynamic_if.frag's control flow and a0 instructions,
and unless xost's simulator (xenos-toolchain/xdis/simulate.py, which models a0, and the predicate for each pixel on its
own) runs each hand-assembled case to a whole constant or the marker for every cell's indices, under the condition true
and false; that is what the probe expects. For vk_dynamic_if.frag it gives the colour, and the colour if the arm's read
at a0 wrote nothing, its r2 left at the literal 0.5.
"""
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "xenos-toolchain"))

import gen_triangle_shaders as tri  # noqa: E402
import gen_xsc_probe as pack  # noqa: E402
import xdis.container as container  # noqa: E402
import xdis.simulate as simulate  # noqa: E402
from gen_triangle_shaders import ce, enc, xu  # noqa: E402

V = xu.AluVectorOpcode
S = xu.AluScalarOpcode
CF = xu.ControlFlowOpcode
CONSTANTS = 256
# c[n]'s z, so a pixel decodes only from one whole constant.
CHECK = 0x5A
# r0 holds the interpolator: i + 0.5, j + 0.5, l + 0.5 and w, as the raster test passes i and j.
INDEX, PREDICATE, RESULT, SCRATCH = 0, 1, 2, 3
REGISTERS = 4
I, J, L, W = 0, 1, 2, 3
# Rows read at a0, 16 apart from the next so a pixel names the base and the index; plain rows past them, below the
# group from c252 that xsdk keeps (include/xsdk/gpu.h, XSDK_PIXEL_CONSTANTS).
ARM_ROW, OTHER_ROW, AFTER_ROW, BEFORE_ROW = 32, 64, 96, 128
PLAIN_TRUE, PLAIN_FALSE, STALE_ROW, MODE_ROW = 160, 176, 192, 240
# MODE_ROW's value, the arms' condition true and false.
MODES = ((1.0,) * 4, (-1.0,) * 4)
EXEC_MOST = 6
# Each cell's indices: i, j never i, l neither, and w 1 or -1 for the cases whose condition is the cell's own.
COMBOS = 32
UNIFORM_COMBO = 7
MARKER = -2

# vk_dynamic_if.frag as xost compiles it with the optimiser on and 64 registers, from 3f35da3's
# xenos-toolchain/xsc/testdata/vk_dynamic_if.frag.spv: by main at d8206eb (no fix) and by the a0-fixes branch at
# 3f35da3 (the fix, maxasf r0.x again at slot 16); one block of 33 rows at c0, the literal 0.5 at c36.
UNFIXED = (
    0x102A1100, 0x00000124, 0x000000F0, 0x00000000, 0x00000024, 0x00000000, 0x00000100, 0x00000000, 0x00000000,
    0x000000D8, 0x0000001C, 0x000000D3, 0xFFFF0300, 0x00000004, 0x0000001C, 0x00001000, 0x000000CC, 0x0000006C,
    0x00020000, 0x00100000, 0x00000074, 0x00000000, 0x00000084, 0x00020010, 0x00100000, 0x0000008C, 0x00000000,
    0x0000009C, 0x00020020, 0x00010000, 0x000000A4, 0x00000000, 0x000000B4, 0x00020024, 0x00010000, 0x000000BC,
    0x00000000, 0x636F6C6F, 0x727300AB, 0x00010003, 0x00010004, 0x00010000, 0x00000000, 0x74696E74, 0x7300ABAB,
    0x00010003, 0x00010004, 0x00010000, 0x00000000, 0x6D6F6465, 0x00ABABAB, 0x00010003, 0x00010004, 0x00010000,
    0x00000000, 0x5F6C6974, 0x333600AB, 0x00010003, 0x00010004, 0x00010000, 0x00000000, 0x70735F33, 0x5F300078,
    0x6F737400, 0x00000000, 0x000000F0, 0x10000400, 0x00000008, 0x00020000, 0x00002023, 0x00010007, 0x00000001,
    0x0000F0A0, 0x00000000, 0x5003C400, 0x10000000, 0x00004008, 0x300C5600, 0x52000000, 0x0000500F, 0x00002200,
    0x00000000, 0x60000000, 0x0000006C, 0xE2000000, 0xC80F0001, 0xE0000000, 0x22000000, 0xC80F0002, 0x00000000,
    0x22242400, 0xC80F0003, 0x026C0000, 0x60200200, 0x74000000, 0x0000006C, 0xE2000003, 0x60000000, 0x180000B1,
    0xE2000000, 0xC80F0002, 0xF8000000, 0x22101000, 0xC80F0003, 0x18B1B100, 0x22202000, 0xC80F0004, 0x18006C00,
    0xE1020300, 0xC80F0002, 0xF0000000, 0x22101000, 0xC80F0003, 0x10C6C600, 0x22202000, 0xC80F0004, 0x10006C00,
    0xE1020300, 0xC80F0002, 0x00000000, 0xE1010400, 0xC80F0001, 0xE0000000, 0x22000000, 0xC80F0003, 0x00000000,
    0xE0020100, 0xC80F0001, 0xE0000000, 0x22000000, 0xC80F8000, 0x00000000, 0xE0030100,
)
FIXED = (
    0x102A1100, 0x00000124, 0x000000FC, 0x00000000, 0x00000024, 0x00000000, 0x00000100, 0x00000000, 0x00000000,
    0x000000D8, 0x0000001C, 0x000000D3, 0xFFFF0300, 0x00000004, 0x0000001C, 0x00001000, 0x000000CC, 0x0000006C,
    0x00020000, 0x00100000, 0x00000074, 0x00000000, 0x00000084, 0x00020010, 0x00100000, 0x0000008C, 0x00000000,
    0x0000009C, 0x00020020, 0x00010000, 0x000000A4, 0x00000000, 0x000000B4, 0x00020024, 0x00010000, 0x000000BC,
    0x00000000, 0x636F6C6F, 0x727300AB, 0x00010003, 0x00010004, 0x00010000, 0x00000000, 0x74696E74, 0x7300ABAB,
    0x00010003, 0x00010004, 0x00010000, 0x00000000, 0x6D6F6465, 0x00ABABAB, 0x00010003, 0x00010004, 0x00010000,
    0x00000000, 0x5F6C6974, 0x333600AB, 0x00010003, 0x00010004, 0x00010000, 0x00000000, 0x70735F33, 0x5F300078,
    0x6F737400, 0x00000000, 0x000000FC, 0x10000400, 0x00000008, 0x00020000, 0x00002023, 0x00010007, 0x00000001,
    0x0000F0A0, 0x00000000, 0x5003C400, 0x10000000, 0x00004008, 0x300C5600, 0x52000000, 0x0000600F, 0x00002200,
    0x00000000, 0x60000000, 0x0000006C, 0xE2000000, 0xC80F0001, 0xE0000000, 0x22000000, 0xC80F0002, 0x00000000,
    0x22242400, 0xC80F0003, 0x026C0000, 0x60200200, 0x74000000, 0x0000006C, 0xE2000003, 0x60000000, 0x180000B1,
    0xE2000000, 0xC80F0002, 0xF8000000, 0x22101000, 0xC80F0003, 0x18B1B100, 0x22202000, 0xC80F0004, 0x18006C00,
    0xE1020300, 0xC80F0002, 0xF0000000, 0x22101000, 0xC80F0003, 0x10C6C600, 0x22202000, 0xC80F0004, 0x10006C00,
    0xE1020300, 0xC80F0002, 0x00000000, 0xE1010400, 0x60000000, 0x0000006C, 0xE2000000, 0xC80F0001, 0xE0000000,
    0x22000000, 0xC80F0003, 0x00000000, 0xE0020100, 0xC80F0001, 0xE0000000, 0x22000000, 0xC80F8000, 0x00000000,
    0xE0030100,
)
XOST_LITERALS = ((36, (0.5, 0.5, 0.5, 0.5)),)
# The raster test's rows for vk_dynamic_if.frag (tests/test_raster.c at 3f35da3, make_uniform_if): colors[n] index n's
# colour over 4, tints[n] index 15 - n's, and the mode, x 1 taking the arm that reads at j, 0 the other.
XOST_INDICES = 16
XOST_TINT_ROW = 16
XOST_MODE_ROW = 32
XOST_MODES = ((1.0, 1.0, 1.0, 0.0), (0.0, 1.0, 1.0, 0.0))
# xost's slots in vk_dynamic_if.frag: maxasf r0.x, a read of c[0+a0], max r2, c36, setp_gt r3.x, (p0) maxasf r0.y,
# (p0) and (!p0) reads of c[16+a0] into r2.
XOST_SLOTS = {"maxasf i": 3, "read": 4, "plain": 5, "setp": 7, "maxasf j": 8, "read true": 9, "read false": 12}


def splat(component):
    """A component-relative swizzle reading one component in all four (xenia:src/xenia/gpu/ucode.h:1977-1982)."""
    return sum(((component - k) % 4) << (2 * k) for k in range(4))


def maxasf(component):
    """xost's a0 load, maxasf from an index of r0 (src/xsc_lower_block.c, set_a0)."""
    return dict(scalar_opcode=S.kMaxAsf, src3_reg=INDEX, src3_swizzle=splat(component))


def read(row, dest=RESULT):
    """xost's c[row+a0] read, max of it with itself (src/xsc_lower_block.c, xsdk_xsc_block_register)."""
    return dict(vector_dest=dest, vector_write_mask=0xF, is_const_0_addressed=True, is_const_1_addressed=True,
                is_const_address_register_relative=True, src1_reg=row, src1_is_temp=False, src2_reg=row,
                src2_is_temp=False)


def plain(row, dest):
    return dict(vector_dest=dest, vector_write_mask=0xF, src1_reg=row, src1_is_temp=False, src2_reg=row,
                src2_is_temp=False)


def setp(reg, component=0):
    """setp_gt of a component, as xost tests its condition (src/xsc_lower_flow.c, emit_setp)."""
    return dict(scalar_opcode=S.kSetpGt, src3_reg=reg, src3_swizzle=splat(component))


MARK = dict(vector_opcode=V.kAdd, vector_dest=RESULT, vector_write_mask=0xF, src1_reg=INDEX, src2_reg=INDEX,
            src2_negate=True)
EXPORT = dict(vector_dest=tri.EXPORT_COLOR_0, vector_write_mask=0xF, is_export=True, src1_reg=RESULT,
              src2_reg=RESULT, src3_is_temp=False)


@dataclass
class Exec:
    """An exec's instructions; on the predicate, its condition, whether the exec itself is on it (cond_exec_pred) and
    whether each instruction is; and whether it leaves the predicate as it was."""
    instructions: list
    condition: bool | None = None
    on_predicate: bool = True
    each_predicated: bool = True
    clean: bool = True


def arm(condition, *instructions, on_predicate=True, each_predicated=True):
    return Exec(list(instructions), condition, on_predicate, each_predicated)


def last(*instructions):
    return Exec([*instructions, EXPORT])


def prologue(load=True, before=True, from_w=False):
    """The exec before the arms, as xost's: a0 loaded at i and read, the condition, then the marker into r2 and a plain
    read of STALE_ROW last, the setp leaving the predicate unclean."""
    instructions = [maxasf(I)] if load else []
    instructions += [read(BEFORE_ROW, SCRATCH)] if before else []
    instructions += [plain(MODE_ROW, PREDICATE), MARK, plain(STALE_ROW, SCRATCH),
                     setp(INDEX, W) if from_w else setp(PREDICATE)]
    return Exec(instructions, clean=False)


def control_flow_entry(e, address, end):
    if e.condition is not None and e.on_predicate:
        return enc.build_cf_cond_exec_pred(CF.kCondExecPred, address=address, count=len(e.instructions),
                                           is_predicate_clean=e.clean, condition=e.condition)
    return enc.build_cf_exec(CF.kExecEnd if end else CF.kExec, address=address, count=len(e.instructions),
                             is_predicate_clean=e.clean)


def alu(fields, e):
    if e.condition is not None and e.each_predicated:
        fields = {**fields, "is_predicated": True, "predicate_condition": e.condition}
    return enc.build_alu(**fields)


def control_flow(execs):
    """alloc colors first, as xost lays a pixel shader out, then the execs, the last ending the program; and the
    instructions."""
    entries = [enc.build_cf_alloc(alloc_type=tri.ALLOC_COLORS)]
    address = (len(execs) + 2) // 2
    instructions = []
    for n, e in enumerate(execs):
        assert 1 <= len(e.instructions) <= EXEC_MOST
        entries.append(control_flow_entry(e, address, n == len(execs) - 1))
        instructions += [alu(fields, e) for fields in e.instructions]
        address += len(e.instructions)
    return entries, instructions


def pixel_shader(execs):
    dwords = tri.pack(*control_flow(execs))
    return ce.build_container(
        "pixel", struct.pack(f">{len(dwords)}I", *dwords), constants=[],
        program_control=tri.program_control(ps_num_reg=REGISTERS - 1, ps_export_mode=tri.PS_EXPORT_MODE_COLOR),
        context_misc=tri.CONTEXT_MISC_CENTROIDS_AND_CENTERS,
        interpolators=[ce.Interpolator(tri.EXPORT_INTERPOLATOR_0, ce.USAGE_COLOR, 0)],
        ps_field18=tri.PS_INTERPOLATOR_WORD, creator=tri.CREATOR)


def vk_dynamic_if_arms(on_predicate=True, each_predicated=True):
    """vk_dynamic_if.frag's arms: (p0) loads j and reads at it, (!p0) reads at the i loaded before."""
    return [arm(True, maxasf(J), read(ARM_ROW), on_predicate=on_predicate, each_predicated=each_predicated),
            arm(False, read(OTHER_ROW), on_predicate=on_predicate, each_predicated=each_predicated)]


def hand_cases():
    """Each hand-assembled case's name and execs, in the order they run."""
    p = prologue
    return [
        ("control: plain reads in the arms, no a0",
         [p(load=False, before=False), arm(True, plain(PLAIN_TRUE, RESULT)), arm(False, plain(PLAIN_FALSE, RESULT)),
          last()]),
        ("control: a read at a0 after the setp, no arms", [p(), last(read(AFTER_ROW))]),
        ("(p0) arm loads j and reads at a0", [p(), arm(True, maxasf(J), read(ARM_ROW)), last()]),
        ("(!p0) arm loads j and reads at a0", [p(), arm(False, maxasf(J), read(ARM_ROW)), last()]),
        ("(p0) arm reads at the a0 loaded before the arms", [p(), arm(True, read(ARM_ROW)), last()]),
        ("(!p0) arm reads at the a0 loaded before the arms", [p(), arm(False, read(ARM_ROW)), last()]),
        ("(p0) arm loads j alone, read after the arms", [p(), arm(True, maxasf(J)), last(read(AFTER_ROW))]),
        ("(p0) arm loads j and reads, read after the arms",
         [p(), arm(True, maxasf(J), read(ARM_ROW, SCRATCH)), last(read(AFTER_ROW))]),
        ("as 7, maxasf i again after the arms", [p(), arm(True, maxasf(J)), last(maxasf(I), read(AFTER_ROW))]),
        ("as 8, maxasf i again after the arms, the fix's shape",
         [p(), arm(True, maxasf(J), read(ARM_ROW, SCRATCH)), last(maxasf(I), read(AFTER_ROW))]),
        ("both arms load, j and l, read after the arms",
         [p(), arm(True, maxasf(J)), arm(False, maxasf(L)), last(read(AFTER_ROW))]),
        ("vk_dynamic_if.frag's arms: (p0) loads j and reads, (!p0) reads at i", [p(), *vk_dynamic_if_arms(), last()]),
        ("as 12, the (p0) arm reading at i, no load in it",
         [p(), arm(True, read(ARM_ROW)), arm(False, read(OTHER_ROW)), last()]),
        ("as 12, the (!p0) arm loading i again",
         [p(), arm(True, maxasf(J), read(ARM_ROW)), arm(False, maxasf(I), read(OTHER_ROW)), last()]),
        ("as 12, the (!p0) arm first", [p(), *reversed(vk_dynamic_if_arms()), last()]),
        ("as 12, the (!p0) arm loading l",
         [p(), arm(True, maxasf(J), read(ARM_ROW)), arm(False, maxasf(L), read(OTHER_ROW)), last()]),
        ("as 12, no read at a0 before the arms", [p(before=False), *vk_dynamic_if_arms(), last()]),
    ]


def predication_cases():
    """Shapes xost never writes, after its own programs: the arms predicated by the execs alone or the instructions
    alone, a load predicated off running for every pixel."""
    p = prologue
    return [
        ("as 12, plain execs, each instruction predicated", [p(), *vk_dynamic_if_arms(on_predicate=False), last()]),
        ("as 12, execs on the predicate, no instruction predicated",
         [p(), *vk_dynamic_if_arms(each_predicated=False), last()]),
        ("as 3, a plain exec, each instruction predicated",
         [p(), arm(True, maxasf(J), read(ARM_ROW), on_predicate=False), last()]),
        ("as 3, an exec on the predicate, no instruction predicated",
         [p(), arm(True, maxasf(J), read(ARM_ROW), each_predicated=False), last()]),
    ]


def own_condition_cases():
    """The riskiest hand-assembled cases: the condition the cell's w, so pixels of one vector take different arms."""
    return [
        ("as 12, the condition each cell's own", [prologue(from_w=True), *vk_dynamic_if_arms(), last()]),
        ("as 3, the condition each cell's own",
         [prologue(from_w=True), arm(True, maxasf(J), read(ARM_ROW)), last()]),
    ]


def combo(n):
    i = n % 16
    return i, (i + 1 + n // 16 * 4) % 16, (i + 11) % 16, 1.0 if n % 2 == 0 else -1.0


def interpolator(n):
    i, j, l, w = combo(n)
    return [i + 0.5, j + 0.5, l + 0.5, w]


def fill(n):
    """c[n]: n, 255 - n and n ^ CHECK, each a quarter into its 8-bit step, and w 1."""
    return [simulate.single((v + 0.25) / 255) for v in (n, 255 - n, n ^ CHECK)] + [1.0]


def pattern(mode):
    constants = {n: fill(n) for n in range(CONSTANTS)}
    constants[MODE_ROW] = list(MODES[mode])
    return constants


def xost_rows(mode, tint=None):
    """The raster test's rows for vk_dynamic_if.frag, its literal, and the tints all one value if given."""
    rows = {n: [0.0] * 4 for n in range(CONSTANTS)}
    for n in range(XOST_INDICES):
        colour, other = indexed_argb(n), indexed_argb(XOST_INDICES - 1 - n)
        rows[n] = [simulate.single(channel(colour, 16 - 8 * c) / 4.0) for c in range(3)] + [1.0]
        rows[XOST_TINT_ROW + n] = [simulate.single(channel(other, 16 - 8 * c)) for c in range(3)] + [1.0]
        if tint is not None:
            rows[XOST_TINT_ROW + n] = [tint] * 4
    rows[XOST_MODE_ROW] = list(XOST_MODES[mode])
    for reg, values in XOST_LITERALS:
        rows[reg] = list(values)
    return rows


def indexed_argb(n):
    """The raster test's index colour: red 16n + 8, green its complement, blue it xor 0x5A."""
    m = 16 * n + 8
    return 0xFF000000 | m << 16 | (255 - m) << 8 | (m ^ CHECK)


def channel(argb, shift):
    return simulate.single((argb >> shift & 0xFF) / 255.0)


def to_byte(value):
    return int(min(max(value, 0.0), 1.0) * 255.0 + 0.5)


def export(name, blob, n, constants):
    value = simulate.run(blob, {INDEX: interpolator(n)}, constants).exports.get(tri.EXPORT_COLOR_0)
    if value is None:
        sys.exit(f"gen_a0_predicated_probe: {name}: no colour export")
    return value


def register_read(name, blob, n, mode):
    """The constant Xenia's model has the case export for the combination's indices, or MARKER for none."""
    value = export(name, blob, n, pattern(mode))
    if value == [0.0] * 4:
        return MARKER
    reg = round(value[0] * 255 - 0.25)
    if not 0 <= reg < CONSTANTS or value != fill(reg):
        sys.exit(f"gen_a0_predicated_probe: {name}: indices {combo(n)} export {value}, no whole constant")
    return reg


def argb(value):
    return to_byte(value[3]) << 24 | to_byte(value[0]) << 16 | to_byte(value[1]) << 8 | to_byte(value[2])


def describe(regs, combos):
    """The model's reads in words: one constant, the marker, or a row at one of the indices."""
    if all(r == MARKER for r in regs):
        return "marker"
    if all(r == regs[0] for r in regs):
        return f"c{regs[0]}"
    for name, k in (("i", I), ("j", J), ("l", L)):
        base = regs[0] - combo(combos[0])[k]
        if all(r == base + combo(n)[k] for r, n in zip(regs, combos)):
            return f"c[{base}+{name}]"
    return None


def model_text(name, regs):
    """The model's reads over all combinations, split by w where the condition is the cell's own."""
    text = describe(regs, list(range(COMBOS)))
    if text:
        return text
    halves = [[n for n in range(COMBOS) if (combo(n)[W] > 0) == positive] for positive in (True, False)]
    texts = [describe([regs[n] for n in half], half) for half in halves]
    if None in texts:
        sys.exit(f"gen_a0_predicated_probe: {name}: the model's reads {regs} have no description")
    return f"{texts[0]} where w > 0, {texts[1]} elsewhere"


def words_of(blob):
    return list(struct.unpack(f">{len(blob) // 4}I", blob))


def ucode(words):
    return container.Container(struct.pack(f">{len(words)}I", *words)).ucode_dwords


def check_encodings():
    """Stops unless the builders write xost's control flow and a0 instructions for vk_dynamic_if.frag."""
    for words, after in ((UNFIXED, 5), (FIXED, 6)):
        code = ucode(words)
        execs = [Exec([{}] * 5, clean=False), arm(True, *[{}] * 4), arm(False, *[{}] * 3), Exec([{}] * after)]
        flow = tri.pack(control_flow(execs)[0], [])
        if list(code[:len(flow)]) != flow:
            sys.exit("gen_a0_predicated_probe: the control flow builders don't write xost's")
        slot = {name: tuple(code[3 * s:3 * s + 3]) for name, s in XOST_SLOTS.items()}
        ours = {"maxasf i": enc.build_alu(**maxasf(I)), "read": enc.build_alu(**read(0, dest=1)),
                "plain": enc.build_alu(**plain(36, RESULT)), "setp": enc.build_alu(**setp(3)),
                "maxasf j": alu(maxasf(J), arm(True)), "read true": alu(read(16), arm(True)),
                "read false": alu(read(16), arm(False))}
        for name, want in slot.items():
            if ours[name] != want:
                sys.exit(f"gen_a0_predicated_probe: the {name} builder doesn't write xost's words")


def c_float(value):
    return f"{value!r}f" if value != int(value) else f"{value:.1f}f"


def vector_lines(name, kind, vectors, form=c_float):
    return [f"static const {kind} {name} = {{"] + \
        ["\t{ " + ", ".join(form(v) for v in vector) + " }," for vector in vectors] + ["};", ""]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    check_encodings()
    xost_row_count = max(r for r, _ in XOST_LITERALS) + 1
    rows = xost_rows(0)
    lines = ["// Generated by tools/gen_a0_predicated_probe.py; don't edit.", "",
             f"enum {{ PROBE_UNIFORM_COMBO = {UNIFORM_COMBO}, PROBE_MODE_ROW = {MODE_ROW}, "
             f"PROBE_MARKER = {MARKER}, PROBE_XOST_ROWS = {xost_row_count}, PROBE_XOST_MODE_ROW = {XOST_MODE_ROW} }};",
             ""]
    lines += vector_lines("probe_combos[COMBOS][4]", "float", [interpolator(n) for n in range(COMBOS)])
    lines += vector_lines("probe_combo_indices[COMBOS][3]", "uint8_t", [combo(n)[:3] for n in range(COMBOS)],
                          str)
    lines += vector_lines("probe_mode_values[MODES][4]", "float", MODES)
    lines += vector_lines("probe_xost_rows[PROBE_XOST_ROWS][4]", "float", [rows[n] for n in range(xost_row_count)])
    lines += vector_lines("probe_xost_modes[MODES][4]", "float", XOST_MODES)
    lines += cases_c()
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines), encoding="utf-8")


def cases():
    """Each case's name, container words, rounds and whether it's xost's program, in the order they run: the
    riskiest last."""
    own = [(name, words_of(pixel_shader(execs)), 1, False) for name, execs in hand_cases()]
    own += [("vk_dynamic_if.frag without the fix, xost at d8206eb", list(UNFIXED), 1, True),
            ("vk_dynamic_if.frag with the fix, xost at 3f35da3", list(FIXED), 1, True)]
    own += [(name, words_of(pixel_shader(execs)), 1, False)
            for name, execs in predication_cases() + own_condition_cases()]
    return own + [("vk_dynamic_if.frag with the fix, 15 rounds", list(FIXED), 15, True)]


def table_lines(label, kind, values, form):
    return [f"static const {kind} {label}[MODES][COMBOS] = {{"] + \
        ["\t{ " + ", ".join(form(v) for v in row) + " }," for row in values] + ["};", ""]


def cases_c():
    """Each case's container and what the model has it export, and the table of cases."""
    lines, table = [], []
    for number, (name, words, rounds, xost) in enumerate(cases()):
        blob = struct.pack(f">{len(words)}I", *words)
        lines += [f"static const uint32_t case_{number}[] = {{", pack.word_lines(words), "};", ""]
        if xost:
            colours = [[argb(export(name, blob, n, xost_rows(m))) for n in range(COMBOS)] for m in range(2)]
            silent = [[argb(export(name, blob, n, xost_rows(m, 0.5))) for n in range(COMBOS)] for m in range(2)]
            lines += table_lines(f"argb_{number}", "uint32_t", colours, lambda v: f"0x{v:08X}")
            lines += table_lines(f"silent_{number}", "uint32_t", silent, lambda v: f"0x{v:08X}")
            texts, reads, argbs = '"its colour", "its colour"', "NULL", (f"argb_{number}", f"silent_{number}")
        else:
            regs = [[register_read(name, blob, n, m) for n in range(COMBOS)] for m in range(2)]
            lines += table_lines(f"reads_{number}", "int16_t", regs, str)
            texts = ", ".join(f'"{model_text(name, row)}"' for row in regs)
            reads, argbs = f"reads_{number}", ("NULL", "NULL")
        table.append(f'\t{{ "{name}", case_{number}, sizeof case_{number}, {rounds}, {{ {texts} }}, {reads}, '
                     f"{argbs[0]}, {argbs[1]} }},")
    return lines + ["static const ProbeCase probe_cases[] = {"] + table + ["};", ""]


if __name__ == "__main__":
    main()
