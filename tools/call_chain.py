#!/usr/bin/env python3
"""Walk callers upward from a never-executed function to the executed code that gates it.

Input: decoded Master image and build/hot.txt from ob12_master. A byte scan finds CALL,
CALLP and 24-bit pointer references, so data bytes can give false hits; treat the output
as leads to read, not as proof.
"""
import re
import sys

BASE = 0xF00000
image = open(sys.argv[1], "rb").read()
hot = {}
for line in open(sys.argv[2]):
    address, count = line.split()
    hot[int(address, 16)] = int(count)


def function_start(offset):
    """Scan back to the byte after the previous return; LINK or PUSHW usually starts there."""
    for at in range(offset - 1, max(offset - 0x600, 0), -1):
        if image[at] in (0x66, 0x67, 0x6B) and image[at + 1] in (0x08, 0x4F):
            return at + 1
    return offset


def references(target):
    found = []
    low = target & 0xFFFF
    call = bytes((0x64, low & 0xFF, low >> 8))
    far = bytes((0x65, low & 0xFF, low >> 8, (target >> 16) & 0xFF))
    pointer = bytes((low & 0xFF, low >> 8, (target >> 16) & 0xFF))
    bank_start = (target - BASE) & 0xFF0000
    for match in re.finditer(re.escape(call), image[bank_start:bank_start + 0x10000]):
        found.append(("CALL", bank_start + match.start()))
    for match in re.finditer(re.escape(far), image):
        found.append(("CALLP", match.start()))
    for match in re.finditer(re.escape(pointer), image):
        if not any(kind == "CALLP" and at + 1 == match.start() for kind, at in found):
            found.append(("PTR", match.start()))
    return found


seen = set()


def walk(target, depth):
    if target in seen or depth > int(sys.argv[4]):
        return
    seen.add(target)
    for kind, offset in references(target):
        site = BASE + offset
        start = BASE + function_start(offset)
        ran = hot.get(site, 0)
        entered = hot.get(start, 0)
        print(f"{'  ' * depth}{kind} at {site:06X} in {start:06X} site x{ran} entry x{entered}")
        if not ran and not entered:
            walk(start, depth + 1)


walk(int(sys.argv[3], 16), 0)
