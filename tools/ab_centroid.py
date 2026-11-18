# Same run with one change: compares MOUT left at equal times, so slow LFOs cancel out.
# usage: ab_centroid.py <a ports.raw> <b ports.raw> <from s> <to s>
import sys
import numpy as np
def cent(p, s, e):
    x = np.memmap(p, dtype='<i4', mode='r').reshape(-1, 6)
    out = []
    t = s
    while t + 0.2 < e:
        y = np.array(x[int(t * 96000):int((t + 0.2) * 96000):2, 2], float)
        y -= y.mean()
        f = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
        fr = np.fft.rfftfreq(len(y), 1 / 48000)
        out.append((f * fr).sum() / max(f.sum(), 1))
        t += 0.2
    return np.array(out)
s, e = float(sys.argv[3]), float(sys.argv[4])
a, b = cent(sys.argv[1], s, e), cent(sys.argv[2], s, e)
print(f'a mean {a.mean():6.0f}  b mean {b.mean():6.0f}  b/a median {np.median(b / a):5.2f}  b higher {np.mean(b > a) * 100:3.0f}% of the time')
