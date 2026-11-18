#!/usr/bin/env python3
"""Diagnostic only: check whether factory images contain stable named preset records.

No firmware bytes are printed or published. The bytes remain transient in CI.
"""
import pathlib
import sys

def looks_like_name(data, pos, stop):
    if pos >= stop: return ""
    text = bytearray()
    while pos < stop and len(text) < 24 and 32 <= data[pos] <= 126:
        text.append(data[pos]); pos += 1
    name = text.decode("ascii").strip()
    if not 4 <= len(name) <= 20: return ""
    return name if sum(c.isalpha() for c in name) >= 3 else ""

def candidates(blob):
    if len(blob) % 256: return []
    stride = len(blob)//256
    if stride < 16 or stride > 4096: return []
    choices = []
    for offset in range(stride-4):
        names=[looks_like_name(blob, i*stride+offset, (i+1)*stride) for i in range(256)]
        good=[x for x in names if x]
        unique=len(set(good))
        if len(good)>=70 and unique>=35:
            choices.append((len(good)*2+unique,offset,names))
    return sorted(choices,key=lambda x:-x[0])

def main():
    for filename in sys.argv[1:]:
        p=pathlib.Path(filename)
        if not p.exists():
            print(f"{p.name}: ABSENT")
            continue
        b=p.read_bytes()
        hits=candidates(b)
        print(f"{p.name}: bytes={len(b)} stride={len(b)//256 if len(b)%256==0 else 'nonuniform'} candidates={len(hits)}")
        for score,offset,names in hits[:3]:
            print(f"  candidate score={score} offset={offset}, A001..A008={names[:8]}")
        if not hits: print("  No credible 256-record name layout. Browser must label slot numbers.")
    return 0
if __name__ == "__main__": raise SystemExit(main())
