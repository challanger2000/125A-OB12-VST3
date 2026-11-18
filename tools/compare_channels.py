#!/usr/bin/env python3
"""Compares the Slave's DASP0 writes for one note on different Slave channels.

Replays build/master_to_slave.bin plus one note through ob12_slave for each channel, then
lists the CMEM addresses and final values each note produced.
usage: compare_channels.py <channel> [<channel> ...]
"""
import subprocess
import sys

sys.argv, channels = sys.argv[:1], [int(c) for c in sys.argv[1:]]


def updates(s):
    i, out = 0, []
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


base = open("build/master_to_slave.bin", "rb").read()
results = {}
for channel in channels:
    open("build/note.bin", "wb").write(base + bytes([0xFE] * 6000) + bytes((0x90 | channel, 0x3C, 0x64)))
    subprocess.run(["F:/Claude/OB-12/build/ob12_slave.exe", "rom", "120000000", "build/note.bin"], capture_output=True)
    ups = []
    for chip in range(5):
        data = open(f"build/dsp_stream_{chip}.bin", "rb").read()
        u = updates(data)
        if len(u) > 3 and chip < 4:
            ups = u[3:]
            print(f"channel {channel}: DASP{chip}, {len(ups)} writes")
            break
    final = {}
    for a, v in ups:
        final[a] = v
    results[channel] = final

keys = sorted({a for f in results.values() for a in f})
print("addr  " + "  ".join(f"ch{c:<6}" for c in channels))
for a in keys:
    row = [f"{results[c].get(a, -1):06X}" if a in results[c] else "  --  " for c in channels]
    if len(set(row)) > 1:
        print(f"{a:03X}   " + "   ".join(row))
print("identical addresses:", sum(1 for a in keys if len({results[c].get(a) for c in channels}) == 1))
