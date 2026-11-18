# Pitch words written at each note of a run, and MOUT pitch after it. usage: note_words.py <run dir> t0 step count
import sys
import numpy as np
d, t0, step, count = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), int(sys.argv[4])
W, M = [], []
for line in open('build/%s/host.txt' % d, errors='replace'):
    p = line.split()
    if len(p) > 4 and p[1] == 'W':
        W.append((float(p[0]), int(p[2]), p[3], p[4]))
    elif len(p) > 2 and p[1] == 'M':
        M.append((float(p[0]), p[2]))
x = np.memmap('build/%s/build/ports.raw' % d, dtype='<i4', mode='r').reshape(-1, 6)
names = {'C046', 'C05B', 'C070', 'C085', 'C09A', 'C0AF', 'C048', 'C087'}
for i in range(count):
    a = t0 + i * step
    words = [(c, n, v) for t, c, n, v in W if a <= t < a + .03 and n in names]
    v = np.array(x[int((a + step * .4) * 96000):int((a + step * .95) * 96000):2, 2], float)
    v -= v.mean()
    f = np.abs(np.fft.rfft(v * np.hanning(len(v)))) if v.std() > 1 else np.zeros(9)
    fr = np.fft.rfftfreq(len(v), 1 / 48000)
    pk = [round(float(fr[j])) for j in range(3, len(f) - 1) if f[j] > f[j - 1] and f[j] > f[j + 1] and f[j] > .25 * f.max()][:5]
    ms = ' '.join(b for t, b in M if a - .02 <= t < a + .05)
    print('%.2f master %s | rms %.0f lines %s | %s' % (a, ms[:24], v.std(), pk, words[:8]))
