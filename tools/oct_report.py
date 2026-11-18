# Per mark: pitch of MOUT left, phase words written at the note, Master bytes. Arg: run dir.
import sys
import numpy as np
d = sys.argv[1]
x = np.memmap('build/%s/build/ports.raw' % d, dtype='<i4', mode='r').reshape(-1, 6)
marks = [l.strip().split('|') for l in open('build/oct_marks.txt')]
W, M = [], []
for line in open('build/%s/host.txt' % d, errors='replace'):
    p = line.split()
    if len(p) > 4 and p[1] == 'W':
        W.append((float(p[0]), int(p[2]), p[3], p[4]))
    elif len(p) > 2 and p[1] == 'M':
        M.append((float(p[0]), p[2]))
for name, t in marks:
    t = float(t)
    v = np.array(x[int((t + .15) * 96000):int((t + .45) * 96000):2, 2], float)
    v -= v.mean()
    if v.std() < 1:
        print(name, 'silent')
        continue
    n = len(v)
    f = np.fft.rfft(v, 2 * n)
    c = np.fft.irfft(f * np.conj(f))[:4000]
    c /= c[0]
    k = 24 + int(np.argmax(c[24:4000]))
    words = [(a, val) for tt, chip, a, val in W if t <= tt < t + .02 and a in ('C085', 'C09A', 'C0AF', 'C087', 'C09C', 'C0B1')]
    ms = ' '.join(b for tt, b in M if t - .4 <= tt < t)
    print('%-14s %.1f Hz corr %.2f' % (name, 48000 / k, c[k]), words[:3], '| master', ms[:30])
