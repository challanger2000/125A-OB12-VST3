# Mean spectral centroid of MOUT left per window, for a sound with a slow LFO.
# usage: centroid_mean.py <ports.raw> <from s> <to s> ...pairs
import sys
import numpy as np
x = np.memmap(sys.argv[1], dtype='<i4', mode='r').reshape(-1, 6)
a = sys.argv[2:]
for s, e in zip(a[0::2], a[1::2]):
    s, e = float(s), float(e)
    c, r = [], []
    t = s
    while t + 0.2 < e:
        y = np.array(x[int(t * 96000):int((t + 0.2) * 96000):2, 2], float)
        y -= y.mean()
        f = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
        fr = np.fft.rfftfreq(len(y), 1 / 48000)
        c.append((f * fr).sum() / max(f.sum(), 1))
        r.append(y.std())
        t += 0.2
    print(f'{s:5.1f}-{e:5.1f} s centroid mean {np.mean(c):6.0f} min {np.min(c):6.0f} max {np.max(c):6.0f} rms {np.mean(r):8.0f}')
