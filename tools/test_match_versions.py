import csv
import struct
import tempfile
import unittest
from pathlib import Path

import match_versions


class MatchVersionsTest(unittest.TestCase):
    def test_normalise_masks_jump_targets_and_immediates(self):
        left = struct.pack("<III", 0x0C040000, 0x3C010012, 0x03E00008)
        right = struct.pack("<III", 0x0C080000, 0x3C010034, 0x03E00008)
        self.assertEqual(match_versions.normalise(left), match_versions.normalise(right))
        self.assertNotEqual(match_versions.normalise(left), match_versions.normalise(right[:-4] + b"\0" * 4))

    def test_only_unique_file_backed_functions_match(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            elf = bytearray(0x110)
            elf[:7] = b"\x7fELF\x01\x01\x01"
            struct.pack_into("<I", elf, 28, 52)
            struct.pack_into("<HH", elf, 42, 32, 1)
            struct.pack_into("<8I", elf, 52, 1, 0x100, 0x1000, 0, 16, 32, 0, 0)
            elf[0x100:] = b"AAAABBBBAAAACCCC"
            (root / "game.elf").write_bytes(elf)
            with (root / "functions.csv").open("w", newline="") as f:
                writer = csv.writer(f)
                writer.writerow(("Name", "Start", "End"))
                writer.writerows((("dup1", "0x1000", "0x1004"), ("unique", "0x1004", "0x1008"),
                                  ("dup2", "0x1008", "0x100c"), ("bss", "0x1010", "0x1014")))
            index, skipped = match_versions.index_functions(root / "game.elf", root / "functions.csv")
            self.assertEqual(skipped, 1)
            self.assertEqual(match_versions.match(index, index), [((0x1004, 0x1008, "unique"),) * 2])


if __name__ == "__main__":
    unittest.main()
