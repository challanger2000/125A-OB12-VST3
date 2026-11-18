# Spectral centroid and level of MOUT left over time, from ports.raw.
# usage: centroid.py <ports.raw> <from s> <to s> [step s]
import sys
import numpy as np

x = np.memmap(sys.argv[1], dtype='<i4', mode='r').reshape(-1, 6)
start, end = float(sys.argv[2]), float(sys.argv[3])
step = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
t = start
while t < end and int((t + 0.2) * 96000) < len(x):
    y = np.array(x[int(t * 96000):int((t + 0.2) * 96000):2, 2], float)
    y -= y.mean()
    f = np.abs(np.fft.rfft(y * np.hanning(len(y)))) ** 2
    fr = np.fft.rfftfreq(len(y), 1 / 48000)
    print(f'{t:6.2f} s rms {y.std():9.0f} centroid {(f * fr).sum() / max(f.sum(), 1):7.0f} Hz')
    t += step
