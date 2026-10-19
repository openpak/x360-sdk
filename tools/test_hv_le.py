#!/usr/bin/env python3
"""Check assembled HV windows under explicit endian models, not as Xenon hardware evidence."""

import argparse
import hashlib
import struct
import unittest
from pathlib import Path

from build_hv_le import CODE_OFFSET, IMAGE_BYTES, check_payload, check_windows, package, section
from check_msr_le import read_elf


MASK64 = (1 << 64) - 1
ORIGINAL_MSR = 0x9000000000009000
INPUT = bytes.fromhex("1122334455667788")
BUILD = Path("build")


def signed(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class WindowModel:
    def __init__(self, payload, symbols, operation, fetch_flips, ignored=False, restore_fails=False):
        self.payload = payload
        self.symbols = symbols
        self.operation = operation
        self.fetch_flips = fetch_flips
        self.restore_fails = restore_fails
        self.msr = (ORIGINAL_MSR & ~0x8000) | (operation != 2 and not ignored)
        self.registers = [0] * 32
        for register, value in ((14, ORIGINAL_MSR), (16, 0x10000), (17, 0x20000), (18, operation),
                                (19, symbols["xsdk_hv_le_returned"]), (26, 0x11223344)):
            self.registers[register] = value
        self.scratch = bytearray(8)
        self.accesses = []
        self.equal = False
        self.ctr = 0
        self.pc = symbols["xsdk_hv_le_gate"]

    def memory(self, opcode, target, base, immediate):
        address = self.registers[base] + signed(immediate, 16)
        size = {32: 4, 40: 2, 58: 8, 36: 4}[opcode]
        if address % size:
            raise AssertionError("unaligned model access")
        order = "little" if self.msr & 1 else "big"
        self.accesses.append((opcode, address, size))
        if opcode == 36:
            if address != 0x20000:
                raise AssertionError("store escaped scratch")
            self.scratch[:size] = (self.registers[target] & 0xFFFFFFFF).to_bytes(size, order)
        else:
            if address != 0x10000:
                raise AssertionError("load escaped input")
            self.registers[target] = int.from_bytes(INPUT[:size], order)

    def run(self):
        for _ in range(100):
            if self.pc == self.symbols["xsdk_hv_le_returned"] and not self.msr & 1:
                return "returned"
            if not 0 <= self.pc <= len(self.payload) - 4 or self.pc % 4:
                raise AssertionError("fetch escaped payload")
            order = "little" if self.fetch_flips and self.msr & 1 else "big"
            word = int.from_bytes(self.payload[self.pc:self.pc + 4], order)
            opcode, target, base = word >> 26, (word >> 21) & 31, (word >> 16) & 31
            immediate = word & 0xFFFF
            next_pc = self.pc + 4
            if word in (0x08000048, 0x4C00012C, 0x7C0004AC):
                pass
            elif opcode == 18:
                if word & 3:
                    raise AssertionError("unexpected absolute/link branch")
                next_pc = self.pc + signed(word & 0x03FFFFFC, 26)
            elif opcode == 16:
                if word & 3 or base != 2 or target not in (4, 12):
                    raise AssertionError("unexpected conditional branch")
                if self.equal == (target == 12):
                    next_pc = self.pc + signed(word & 0xFFFC, 16)
            elif opcode == 11:
                self.equal = signed(self.registers[base] & 0xFFFFFFFF, 32) == signed(immediate, 16)
            elif opcode in (14, 15):
                value = signed(immediate, 16) << (16 if opcode == 15 else 0)
                self.registers[target] = ((self.registers[base] if base else 0) + value) & MASK64
            elif opcode == 24:
                self.registers[base] = self.registers[target] | immediate
            elif opcode == 28:
                self.registers[base] = self.registers[target] & immediate
                self.equal = self.registers[base] == 0
            elif word & 0xFC1FFFFF == 0x7C0000A6:
                self.registers[target] = self.msr
            elif word & 0xFC1FFFFF == 0x7C000164:
                self.msr = self.registers[target] | self.restore_fails
            elif word & 0xFC1FFFFF == 0x7C0903A6:
                self.ctr = self.registers[target]
            elif word == 0x4E800420:
                next_pc = self.ctr
            elif opcode in (32, 40, 58, 36):
                self.memory(opcode, target, base, immediate)
            else:
                raise AssertionError(f"unsupported instruction {word:08X} at {self.pc:X}")
            if next_pc == self.pc:
                return "parked"
            self.pc = next_pc
        raise AssertionError("window exceeded instruction bound")


class ProbeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.payload, cls.symbols = section(BUILD / "tests/hv_le_payload.o", ".text.hv_payload", ">")
        cls.big, _ = section(BUILD / "tests/hv_le_window_be.o", ".text.hv_window", ">")
        cls.little, _ = section(BUILD / "tests/hv_le_window_le.o", ".text.hv_window", "<")

    def model(self, operation, fetch_flips, **kwargs):
        return WindowModel(self.payload, self.symbols, operation, fetch_flips, **kwargs)

    def test_assembled_controls_and_endian_cases(self):
        for operation in (2, 3, 4):
            for fetch_flips in (False, True):
                with self.subTest(operation=operation, fetch_flips=fetch_flips):
                    model = self.model(operation, fetch_flips)
                    expected_inside = model.msr
                    self.assertEqual(model.run(), "returned")
                    self.assertEqual(model.msr, ORIGINAL_MSR)
                    self.assertEqual(model.registers[20], expected_inside)
                    self.assertEqual(model.registers[21], int(fetch_flips and operation != 2))
                    self.assertEqual(model.registers[22], 0x13579BDF)
                    if operation == 3:
                        self.assertEqual(model.accesses, [])
                        self.assertEqual(model.scratch, bytes(8))
                    else:
                        little = operation == 4
                        self.assertEqual(model.registers[23:26], [0x44332211, 0x2211, 0x8877665544332211]
                                         if little else [0x11223344, 0x1122, 0x1122334455667788])
                        self.assertEqual(model.scratch, bytes.fromhex("4433221100000000" if little
                                                                     else "1122334400000000"))
                        self.assertEqual(len(model.accesses), 4)

    def test_ignored_le_request_avoids_data(self):
        for fetch_flips in (False, True):
            model = self.model(4, fetch_flips, ignored=True)
            self.assertEqual(model.run(), "returned")
            self.assertEqual(model.registers[20] & 1, 0)
            self.assertEqual(model.accesses, [])

    def test_failed_restore_parks_before_report_or_stack(self):
        for fetch_flips in (False, True):
            model = self.model(3, fetch_flips, restore_fails=True)
            self.assertEqual(model.run(), "parked")
            self.assertEqual(model.msr & 1, 1)
            self.assertEqual(model.accesses, [])

    def test_corrupted_banks_and_gate_are_rejected(self):
        check_windows(self.big, self.little)
        check_payload(self.payload, self.symbols, self.big, self.little)
        broken = bytearray(self.little)
        broken[0] ^= 1
        with self.assertRaises(ValueError):
            check_windows(self.big, broken)
        broken = bytearray(self.payload)
        broken[self.symbols["xsdk_hv_le_gate"]] ^= 1
        with self.assertRaises(ValueError):
            check_payload(broken, self.symbols, self.big, self.little)

    def test_unsigned_page_and_linked_image(self):
        image, expansion_id, digest = package(self.payload)
        self.assertEqual(image, (BUILD / "tests/hv_le.exp").read_bytes())
        self.assertEqual(len(image), IMAGE_BYTES)
        magic, flags, end = struct.unpack_from(">3I", image)
        self.assertEqual((magic, flags), (0x48585052, 0))
        self.assertEqual(image[0x20:0x130], bytes(0x110))
        self.assertEqual(image[0xC:0x20], hashlib.sha1(image[0x130:end]).digest())
        self.assertEqual(image[CODE_OFFSET:CODE_OFFSET + len(self.payload)], self.payload)
        self.assertEqual(image[end:], bytes(IMAGE_BYTES - end))
        self.assertEqual(expansion_id, int(digest[:8], 16))
        with self.assertRaises(ValueError):
            package(bytes(IMAGE_BYTES))
        order, sections, symbols = read_elf(BUILD / "probe_hv_le.elf")
        self.assertEqual(order, ">")
        address = symbols["xsdk_hv_le_expansion"]
        copies = []
        for value in sections.values():
            offset = address - value["address"]
            if 0 <= offset <= len(value["data"]) - len(image):
                copies.append(value["data"][offset:offset + len(image)])
        self.assertEqual(copies, [image])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, nargs="?", default=BUILD)
    args = parser.parse_args()
    BUILD = args.build
    unittest.main(argv=[__file__], verbosity=2)
