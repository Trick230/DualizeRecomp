#!/usr/bin/env python3
"""Identify SDK functions in a DOL by matching Dolphin's signature database.

Finds functions the way Dolphin's PPCAnalyst does (bl targets, then whatever
follows each function's final blr), hashes them with
HashSignatureDB::ComputeCodeChecksum and looks the hashes up in a .dsy file.

Usage: find_sdk_symbols.py main.dol totaldb.dsy [-o symbols.txt] [--map out.map]

The output has one line per function: address, size, confidence, library
and name ("-" and "?" if unknown). Confidence is:

  strong  hash and size match, the hash is unique in the DOL and the function
          is at least MIN_STRONG_SIZE bytes
  range   a weaker match (small or duplicated hash) that lies inside the
          address range spanned by strong matches of the same library
  weak    any other match; usually a collision, do not rely on it

--map writes "address size name" lines for DolRecomp's --map, with the strong
matches plus range matches whose name occurs only once (identical-bodied
functions such as GXPokeDither/GXPokeAlphaRead cannot be told apart).
"""

import argparse
import re
import struct
import sys
from collections import defaultdict

PADDING = (0x00000000, 0x60000000)
MIN_STRONG_SIZE = 0x20
# Gap tolerated between strong matches of one library when building its range.
RANGE_SLACK = 0x2000
# Primary opcodes that do not exist on Gekko/Broadway.
INVALID_PRIMARY = {0, 1, 2, 5, 6, 9, 22, 30, 58, 62}


class Dol:
    def __init__(self, data):
        self.sections = []
        for i in range(18):
            off = struct.unpack_from(">I", data, 0x00 + i * 4)[0]
            addr = struct.unpack_from(">I", data, 0x48 + i * 4)[0]
            size = struct.unpack_from(">I", data, 0x90 + i * 4)[0]
            if size:
                self.sections.append((addr, data[off:off + size], i < 7))
        self.text = [(a, d) for a, d, is_text in self.sections if is_text]

    def read(self, addr):
        for base, data, is_text in self.sections:
            if is_text and base <= addr < base + len(data):
                return struct.unpack_from(">I", data, addr - base)[0]
        return None


def checksum(dol, start, end):
    total = 0
    for addr in range(start, end + 4, 4):
        opcode = dol.read(addr)
        op = opcode & 0xFC000000
        op2 = op3 = 0
        aux = op >> 26
        if aux == 4:
            op2 = opcode & 0x3F
            if op2 in (0, 8, 16, 21, 22):
                op3 = opcode & 0x7C0
        elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
            op2 = opcode & 0x03FF0000
        elif aux in (19, 31, 63):
            op2 = opcode & 0x7FF
        elif aux == 59:
            op2 = opcode & 0x3F
            if op2 < 16:
                op3 = opcode & 0x7C0
        elif 32 <= aux < 56:
            op2 = opcode & 0x03FF0000
        total = ((total << 17) & 0xFFFE0000) | ((total >> 15) & 0x1FFFF)
        total ^= op | op2 | op3
    return total


def sext(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


def branch_target(instr, pc):
    opcd = instr >> 26
    if opcd == 18:
        target = sext(instr & 0x03FFFFFC, 26)
    elif opcd == 16:
        target = sext(instr & 0xFFFC, 16)
    else:
        return None
    if not instr & 2:
        target += pc
    return target & 0xFFFFFFFF


def analyze(dol, start):
    """Port of PPCAnalyst::AnalyzeFunction (without max_size). Returns
    (size, calls) or None."""
    farthest = start
    calls = []
    addr = start
    while True:
        instr = dol.read(addr)
        if instr is None or (instr >> 26) in INVALID_PRIMARY:
            return None
        if instr in (0x4E800020, 0x4C000064):
            if farthest <= addr:
                return addr + 4 - start, calls
        elif instr not in (0x4E800021, 0x4E800420, 0x4E800421):
            target = branch_target(instr, addr)
            if target is not None:
                if instr & 1 or target < start:
                    calls.append(target)
                elif instr >> 26 == 16 and target > farthest:
                    farthest = target
        addr += 4
        if addr - start > 0x100000:
            return None


def find_functions(dol):
    funcs = {}

    def add(addr):
        if addr in funcs:
            return funcs[addr]
        result = analyze(dol, addr)
        if result is None:
            return None
        funcs[addr] = result[0]
        return result[0]

    for base, data in dol.text:
        for off in range(0, len(data), 4):
            instr = struct.unpack_from(">I", data, off)[0]
            if instr >> 26 == 18 and instr & 1:
                target = branch_target(instr, base + off)
                if dol.read(target) is not None:
                    add(target)

    for addr, size in list(funcs.items()):
        location = addr + size
        while True:
            instr = dol.read(location)
            while instr is not None and location & 0xF and instr in PADDING:
                location += 4
                instr = dol.read(location)
            if instr is None or (instr >> 26) in INVALID_PRIMARY:
                break
            if location in funcs:
                break
            size = add(location)
            if size is None:
                break
            location += size
    return funcs


def load_dsy(path):
    with open(path, "rb") as f:
        data = f.read()
    count = struct.unpack_from("<I", data, 0)[0]
    db = {}
    for i in range(count):
        checksum_, size, raw = struct.unpack_from("<II128s", data, 4 + i * 136)
        # Entries are "<name> <archive>.a", where name may be a demangled
        # C++ signature with spaces, or just "<name>".
        name, _, lib = raw.split(b"\0", 1)[0].decode("latin-1").strip().rpartition(" ")
        if not lib.endswith((".a", ".o")):
            name, lib = f"{name} {lib}".strip(), "-"
        db[checksum_] = (name, lib, size)
    return db


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dol")
    parser.add_argument("dsy")
    parser.add_argument("-o", "--output")
    parser.add_argument("--map")
    args = parser.parse_args()

    with open(args.dol, "rb") as f:
        dol = Dol(f.read())
    db = load_dsy(args.dsy)
    funcs = find_functions(dol)

    hashes = {addr: checksum(dol, addr, addr + size - 4) for addr, size in funcs.items()}
    by_hash = defaultdict(list)
    for addr, h in hashes.items():
        by_hash[h].append(addr)

    matches = {}
    for addr, size in funcs.items():
        entry = db.get(hashes[addr])
        if entry is None:
            continue
        name, lib, db_size = entry
        strong = (db_size == size and len(by_hash[hashes[addr]]) == 1
                  and size >= MIN_STRONG_SIZE)
        matches[addr] = (name, lib, strong)

    ranges = defaultdict(list)
    for addr in sorted(matches):
        name, lib, strong = matches[addr]
        if not strong:
            continue
        spans = ranges[lib]
        if spans and addr - spans[-1][1] <= RANGE_SLACK:
            spans[-1][1] = addr + funcs[addr]
        else:
            spans.append([addr, addr + funcs[addr]])

    def in_range(lib, addr):
        return any(lo <= addr < hi for lo, hi in ranges.get(lib, ()))

    lines = []
    map_entries = []
    tags = defaultdict(int)
    for addr in sorted(funcs):
        size = funcs[addr]
        if addr not in matches:
            tag, lib, name = "unknown", "-", "?"
        else:
            name, lib, strong = matches[addr]
            tag = "strong" if strong else ("range" if in_range(lib, addr) else "weak")
        tags[tag] += 1
        if tag in ("strong", "range"):
            map_entries.append((addr, size, tag, name))
        lines.append(f"{addr:08X} {size:6X} {tag:7} {lib:16} {name}")

    if args.map:
        name_counts = defaultdict(int)
        for _, _, tag, name in map_entries:
            name_counts[name] += 1
        with open(args.map, "w") as f:
            for addr, size, tag, name in map_entries:
                if tag == "range" and name_counts[name] > 1:
                    continue
                ident = re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_")
                f.write(f"{addr:08X} {size:08X} {ident}\n")

    out = open(args.output, "w") if args.output else sys.stdout
    out.write("\n".join(lines) + "\n")
    if args.output:
        out.close()
    print(f"{len(funcs)} functions: " +
          ", ".join(f"{k} {v}" for k, v in sorted(tags.items())), file=sys.stderr)


if __name__ == "__main__":
    main()
