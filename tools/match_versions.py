#!/usr/bin/env python3
"""Match unique, byte-identical functions between two PS2 ELF builds.

The output contains addresses and sizes only, never game code. An exact match
is evidence of identical bytes, not proof that referenced data is equivalent.
"""
import argparse
import csv
import hashlib
import struct
from collections import defaultdict
from pathlib import Path


def load_segments(path):
    data = Path(path).read_bytes()
    if data[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError(f"{path}: expected a little-endian ELF32 file")
    offset = struct.unpack_from("<I", data, 28)[0]
    entry_size, count = struct.unpack_from("<HH", data, 42)
    if entry_size < 32 or offset + entry_size * count > len(data):
        raise ValueError(f"{path}: invalid program header table")
    segments = []
    for i in range(count):
        kind, file_offset, address, _, file_size, _, _, _ = struct.unpack_from(
            "<8I", data, offset + i * entry_size)
        if kind == 1:
            if file_offset + file_size > len(data):
                raise ValueError(f"{path}: truncated load segment")
            segments.append((address, file_offset, file_size))
    return data, segments


def normalise(code):
    """Remove possible relocated addresses, retaining instruction opcodes/registers.

    Matches are candidates only: immediates can also represent ordinary constants.
    """
    if len(code) % 4:
        return code
    out = bytearray()
    for (word,) in struct.iter_unpack("<I", code):
        op = word >> 26
        if op in (2, 3):
            word &= 0xFC000000
        elif op in (9, 13, 15, 24, *range(32, 48), 49, 53, 55, 57, 61, 63):
            word &= 0xFFFF0000
        out.extend(struct.pack("<I", word))
    return bytes(out)


def index_functions(elf_path, csv_path, candidates=False):
    data, segments = load_segments(elf_path)
    index = defaultdict(list)
    skipped = 0
    with open(csv_path, newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            start, end = int(row["Start"], 0), int(row["End"], 0)
            size = end - start
            if size <= 0:
                skipped += 1
                continue
            for address, file_offset, file_size in segments:
                if address <= start and end <= address + file_size:
                    pos = file_offset + start - address
                    code = data[pos:pos + size]
                    digest = hashlib.sha256(normalise(code) if candidates else code).digest()
                    index[(size, digest)].append((start, end, row["Name"]))
                    break
            else:
                skipped += 1
    return index, skipped


def match(left, right):
    return sorted((entries[0], right[key][0]) for key, entries in left.items()
                  if len(entries) == 1 and len(right.get(key, ())) == 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("left_elf")
    parser.add_argument("left_csv")
    parser.add_argument("right_elf")
    parser.add_argument("right_csv")
    parser.add_argument("output")
    parser.add_argument("--candidates", action="store_true", help="mask possible relocated addresses; results require manual verification")
    args = parser.parse_args()
    left, left_skipped = index_functions(args.left_elf, args.left_csv, args.candidates)
    right, right_skipped = index_functions(args.right_elf, args.right_csv, args.candidates)
    matches = match(left, right)
    with open(args.output, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(("left_start", "left_end", "left_name", "right_start", "right_end", "right_name"))
        for a, b in matches:
            writer.writerow((f"0x{a[0]:08X}", f"0x{a[1]:08X}", a[2],
                             f"0x{b[0]:08X}", f"0x{b[1]:08X}", b[2]))
    kind = "candidate" if args.candidates else "exact"
    print(f"{len(matches)} unique {kind} matches; skipped {left_skipped} left, {right_skipped} right functions")


if __name__ == "__main__":
    main()
