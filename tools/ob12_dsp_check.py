#!/usr/bin/env python3
"""Statically test carved TMS57070 programs against a list of known opcodes.

Reports, per program, how many words use a known primary/secondary opcode and how
many jumps land inside the program. The same test runs on a slice of Slave CPU code
as a control, so the percentages can be read against non-microcode bytes.

The opcode sets are facts about the chip, passed in as JSON (--opcodes). This file
contains no code from any emulator.
"""
import argparse
import collections
import json
import os
import struct
import sys


def classify(word, primary, secondary):
    """Return (known_primary, known_secondary_or_None, class_name)."""
    top = word >> 24
    if top >= 0xC0:
        return top in primary, None, "primary"
    if top >= 0x80:
        paired = (((word & 0x003FC000) + 0x00400000) >> 16) & 0xFF
        return (top & 0x7F) in primary and paired in primary, None, "class2"
    return top in primary, ((word >> 16) & 0x3F) in secondary, "class1"


def analyse(words, primary, secondary):
    stats = collections.Counter()
    unknown = collections.Counter()
    for word in words:
        ok_primary, ok_secondary, name = classify(word, primary, secondary)
        stats[name] += 1
        stats["primary_ok"] += ok_primary
        if not ok_primary:
            unknown[f"P{word >> 24:02X}"] += 1
        if ok_secondary is not None:
            stats["secondary_total"] += 1
            stats["secondary_ok"] += ok_secondary
            if not ok_secondary:
                unknown[f"S{(word >> 16) & 0x3F:02X}"] += 1
        if word >> 28 == 0xF:
            direct = ((word >> 20) & 0x7C) not in (0x08, 0x0C)
            if direct:
                stats["jumps"] += 1
                stats["jumps_in_range"] += (word & 0xFFFF) < len(words)
    return stats, unknown


def report(name, words, primary, secondary):
    stats, unknown = analyse(words, primary, secondary)
    total = len(words)
    sec = stats["secondary_total"] or 1
    jumps = stats["jumps"]
    print(f"{name}: words {total} primary-known {100 * stats['primary_ok'] / total:.1f}% "
          f"secondary-known {100 * stats['secondary_ok'] / sec:.1f}% "
          f"jumps {stats['jumps_in_range']}/{jumps} in range "
          f"classes c1={stats['class1']} c2={stats['class2']} p={stats['primary']}")
    if unknown:
        print("   unknown:", unknown.most_common(10))


def load_words(path):
    with open(path, "rb") as handle:
        data = handle.read()
    return list(struct.unpack(f">{len(data) // 4}I", data[:len(data) // 4 * 4]))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("dsp_dir", help="directory written by ob12_carve.py")
    parser.add_argument("slave_image", help="decoded Slave .bin, for the control")
    parser.add_argument("--opcodes", required=True, help="JSON: {primary: [...], secondary: [...]}")
    args = parser.parse_args()
    with open(args.opcodes) as handle:
        sets = json.load(handle)
    primary, secondary = set(sets["primary"]), set(sets["secondary"])
    for prog in ("prog0", "prog1", "prog4", "prog5", "prog9"):
        report(prog, load_words(os.path.join(args.dsp_dir, prog + ".pmem.bin")), primary, secondary)
    with open(args.slave_image, "rb") as handle:
        image = handle.read()
    control = list(struct.unpack_from("<512I", image, 0x8000))
    report("CONTROL slave code @0x8000", control, primary, secondary)
    return 0


if __name__ == "__main__":
    sys.exit(main())
