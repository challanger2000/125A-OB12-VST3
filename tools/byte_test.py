# Program byte test: one line per mutated byte, only where AOUT moved or new DASP4 cells appeared.
import sys, collections
import numpy as np
for d in sys.argv[1:]:
    x = np.memmap('build/%s/build/ports.raw' % d, dtype='<i4', mode='r').reshape(-1, 6)
    print('==', d, 'aout max', np.abs(x[int(13 * 96000):, 4:6]).max(0))
    W = collections.defaultdict(collections.Counter)
    for line in open('build/%s/host.txt' % d, errors='replace'):
        p = line.split()
        if len(p) > 4 and p[1] == 'W' and float(p[0]) >= 13:
            W[int((float(p[0]) - 13) / .6)][p[2] + p[3]] += 1
    levels = []
    for k in range(53):
        a = 13 + k * .6 + .2
        y = np.array(x[int(a * 96000):int((a + .2) * 96000):2, 2:6], float).std(0).round().astype(int)
        levels.append(int(y[0]))
        odd = sorted(c for c in W[k] if c[0] == '4')
        if y[2:].any() or odd:
            print(k + 3, y, len(W[k]), odd[:8])
    print('left levels', levels)
