# One sample flips: a value near one rail, then near the other, on MOUT and the voice bus of a run dir.
# usage: wrap_scan.py <run dir> [from seconds]
import sys
import numpy as np
d = sys.argv[1]
t0 = float(sys.argv[2]) if len(sys.argv) > 2 else 12
x = np.memmap(d + '/build/ports.raw', dtype='<i4', mode='r').reshape(-1, 6)
b = np.memmap(d + '/build/voice_bus.raw', dtype='<i4', mode='r').reshape(-1, 2)
def scan(name, v):
    v = np.array(v, dtype=np.int64)
    top = np.abs(v).max()
    if top == 0:
        print(name, 'silent'); return
    flips = np.nonzero((np.abs(np.diff(v)) > 1.6 * top))[0]
    print(name, 'peak', top, 'at top %.2f%%' % (100 * (np.abs(v) > .995 * top).mean()), 'flips', len(flips), 'first', [round(t0 + i / 48000, 4) for i in flips[:4]])
    return flips
a = int(t0 * 96000)
for name, c in (('MOUT L', 2), ('MOUT R', 3)):
    scan(name, x[a + 1::2, c])
for name, c in (('bus L', 0), ('bus R', 1)):
    scan(name, b[a + 1::2, c])
