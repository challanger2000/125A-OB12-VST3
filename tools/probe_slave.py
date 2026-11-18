#!/usr/bin/env python3
"""Builds a Slave test stream: the captured Master stream plus candidate control messages.

usage: probe_slave.py make <in.bin> <out.bin> <cc|sysex>
       probe_slave.py read <dsp_stream_4.bin> <hex addresses...>
"""
import sys


def updates(s):
    i = 0
    out = []
    while i < len(s):
        c = s[i]
        if c == 0x0C and i + 2061 <= len(s):
            i += 2061
        elif c == 0x05 and i + 1537 <= len(s):
            i += 1537
        elif c == 0x03:
            i += 1
            while i + 5 <= len(s) and s[i] <= 1:
                out.append(((s[i] << 8 | s[i + 1]) & 0x1FF, s[i + 2] << 16 | s[i + 3] << 8 | s[i + 4]))
                i += 5
        else:
            i += 1
    return out


if sys.argv[1] == "make":
    data = bytearray(open(sys.argv[2], "rb").read())
    if sys.argv[4].startswith("p"):
        for param in sys.argv[4][1:].split(","):
            data += bytes((0xF0, 0x7F, 0x30, 0x04, int(param, 16), 0x04, 0x7F, 0xF7))
    elif sys.argv[4] == "cc":
        for cc in range(0, 120):
            if cc in (0, 32):
                continue
            data += bytes((0xB0, cc, 0x7F))
    else:
        for param in range(0, 32):
            data += bytes((0xF0, 0x7F, 0x30, 0x04, param, 0x04, 0x7F, 0xF7))
    open(sys.argv[3], "wb").write(data)
else:
    ups = updates(open(sys.argv[2], "rb").read())
    wanted = [int(a, 16) for a in sys.argv[3:]]
    print("updates", len(ups), "distinct", len({a for a, _ in ups}))
    for a in wanted:
        hits = [v for x, v in ups if x == a]
        print(f"C{a:03X}: {len(hits)} writes, last {hits[-1]:06X}" if hits else f"C{a:03X}: none")
    print("addresses below 0x20:", sorted({f"{a:03X}" for a, _ in ups if a < 0x20}))
    print("addresses C0-D0:", sorted({f"{a:03X}" for a, _ in ups if 0xC0 <= a <= 0xD0}))
