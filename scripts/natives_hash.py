"""Print natives-list entries (address, size, code hash) for DOL functions.

usage: natives_hash.py <main.dol> <address>:<size>:<symbol> ...

The hash is the FNV-1a 64 of the function's code bytes, which dolrecomp
--natives checks before replacing the function with a host implementation.
"""
import struct
import sys


def text_sections(dol):
    offsets = struct.unpack(">7I", dol[0x00:0x1C])
    addresses = struct.unpack(">7I", dol[0x48:0x64])
    sizes = struct.unpack(">7I", dol[0x90:0xAC])
    return [(a, o, s) for o, a, s in zip(offsets, addresses, sizes) if s]


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    dol = open(sys.argv[1], "rb").read()
    sections = text_sections(dol)
    for spec in sys.argv[2:]:
        address, size, symbol = spec.split(":")
        address, size = int(address, 16), int(size, 16)
        for base, offset, length in sections:
            if base <= address and address + size <= base + length:
                code = dol[offset + address - base:offset + address - base + size]
                print(f"{address:08X} {size:04X} {fnv1a64(code):016X} {symbol}")
                break
        else:
            sys.exit(f"{address:08X} is not in a DOL text section")


if __name__ == "__main__":
    main()
