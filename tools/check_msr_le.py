#!/usr/bin/env python3
"""Check the probe's executable byte orders and relocation-free embedded code before making a XEX."""

import argparse
import struct
from pathlib import Path


def read_elf(path):
    data = path.read_bytes()
    if data[:5] != b"\x7fELF\x01" or data[5] not in (1, 2):
        raise ValueError(f"{path}: expected ELF32")
    order = "<" if data[5] == 1 else ">"
    header = struct.unpack_from(order + "16sHHIIIIIHHHHHH", data)
    if header[2] != 20 or header[11] != 40:
        raise ValueError(f"{path}: expected PowerPC section headers")
    raw = [struct.unpack_from(order + "10I", data, header[6] + 40 * i) for i in range(header[12])]
    names = raw[header[13]][4]
    sections = {}
    for index, section in enumerate(raw):
        name_at = names + section[0]
        name = data[name_at:data.index(0, name_at)].decode()
        sections[name] = {
            "index": index,
            "kind": section[1],
            "address": section[3],
            "data": data[section[4]:section[4] + section[5]] if section[1] != 8 else b"",
            "link": section[6],
            "info": section[7],
        }
    symbols = {}
    for section in sections.values():
        if section["kind"] != 2:
            continue
        strings = raw[section["link"]][4]
        for name, value, *_ in struct.iter_unpack(order + "IIIBBH", section["data"]):
            at = strings + name
            symbols[data[at:data.index(0, at)].decode()] = value
    return order, sections, symbols


def require(condition, message):
    if not condition:
        raise ValueError(message)


def reversed_words(data):
    require(len(data) % 4 == 0, "instruction bank is not word-aligned")
    return b"".join(data[i:i + 4][::-1] for i in range(0, len(data), 4))


def check_banks(big, little):
    for name in (".text.payload", ".text.restore"):
        for sections in (big, little):
            index = sections[name]["index"]
            require(not any(s["kind"] in (4, 9) and s["info"] == index and s["data"]
                            for s in sections.values()), f"{name}: embedded code has relocations")
    payload = bytearray(big[".text.payload"]["data"])
    require(payload[8:12] == bytes.fromhex("3be00000"), "BE fetch marker is not li r31,0")
    payload[8:12] = bytes.fromhex("3be00001")
    require(little[".text.payload"]["data"] == reversed_words(payload), "LE payload differs beyond its fetch marker")
    restore = big[".text.restore"]["data"]
    gate = bytes.fromhex("08000048 4800000c 2c01004c 2004804e 4c00012c 4e800420")
    require(restore.count(gate) == 1, "restore must contain one gate with both isync/bctr encodings")
    at = restore.index(gate)
    expected = reversed_words(restore[:at]) + gate + reversed_words(restore[at + len(gate):])
    require(little[".text.restore"]["data"] == expected, "LE restore instructions differ")
    words = list(struct.unpack(">" + "I" * (len(payload) // 4), payload))
    require(0xBBCF0000 in words and 0xBFD00000 in words, "missing explicit lmw/stmw test opcodes")
    for offset, word in enumerate(words):
        opcode = word >> 26
        require(opcode != 17, "unexpected raw system call in payload")
        if opcode in (16, 18):
            bits = 16 if opcode == 16 else 26
            displacement = word & ((1 << bits) - 4)
            if displacement & (1 << (bits - 1)):
                displacement -= 1 << bits
            require(not word & 3 and 0 <= offset * 4 + displacement < len(payload),
                    "payload branch leaves its bank or uses an absolute/link branch")


def check_linked(sections, symbols, little):
    text = sections[".text"]

    def at(address, size):
        offset = address - text["address"]
        require(0 <= offset <= len(text["data"]) - size, "probe symbol is outside executable text")
        return text["data"][offset:offset + size]

    for gate_name, target_name, section_name in (
        ("xsdk_msr_le_gate", "xsdk_msr_le_payload", ".text.payload"),
        ("xsdk_msr_le_recover", "xsdk_msr_le_restore", ".text.restore"),
    ):
        address = symbols[gate_name]
        first, branch = struct.unpack(">II", at(address, 8))
        require(first == 0x08000048, "fetch discriminator is not BE tdi / LE b .+8")
        require(branch & 0xFC000003 == 0x48000000, "BE discriminator exit is not a relative branch")
        displacement = branch & 0x03FFFFFC
        if displacement & 0x02000000:
            displacement -= 0x04000000
        require(address + 4 + displacement == symbols[target_name], "BE gate branches to the wrong function")
        blob = little[section_name]["data"]
        require(at(address + 8, len(blob)) == blob, "LE gate does not enter the exact LE instruction bank")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("big", type=Path)
    parser.add_argument("little", type=Path)
    parser.add_argument("linked", type=Path)
    args = parser.parse_args()
    be_order, big, _ = read_elf(args.big)
    le_order, little, _ = read_elf(args.little)
    linked_order, linked, symbols = read_elf(args.linked)
    require((be_order, le_order, linked_order) == (">", "<", ">"), "wrong ELF byte orders")
    check_banks(big, little)
    check_linked(linked, symbols, little)
    print("msr_le: PASS relocation-free BE/LE banks, opcodes, local branches, and linked fetch/recovery gates")


if __name__ == "__main__":
    main()
