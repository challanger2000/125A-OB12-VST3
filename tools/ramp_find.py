# Cells of build/cells.bin (256 D, 512 C per sample) that ramp and then stop: when, from what to what.
import sys
import numpy as np
path = sys.argv[1]
x = np.memmap(path, dtype='<i4', mode='r').reshape(-1, 768)
stride = 96  # a multiple of the 6 voice ticks, so a cell keeps its phase
y = np.array(x[::stride], dtype=np.int64)
n = len(y)
print('samples', len(x), 'rows', n, 'seconds', len(x) / 48000)
for c in range(768):
    v = y[:, c]
    if v.min() == v.max():
        continue
    d = np.diff(v)
    changes = np.nonzero(d)[0]
    last = changes[-1]
    first = changes[0]
    runs = len(changes)
    # only smooth movers: mostly one sign, many steps, then still
    sign = np.sign(d[changes])
    if runs < 8 or abs(sign.sum()) < runs * .9:
        continue
    t = lambda r: r * stride / 48000
    name = ('D%03X' % c) if c < 256 else ('C%03X' % (c - 256))
    print(name, 'moves %.4f to %.4f s' % (t(first), t(last + 1)), 'value %06X to %06X' % (v[first] & 0xffffff, v[last + 1] & 0xffffff), 'steps', runs, 'of', n)
