# Finds clicks and full scale jumps in a WAV or 32 bit float WAV. usage: glitch_scan.py file [channel]
import sys, wave
import numpy as np

def load(path):
    try:
        w = wave.open(path)
        r, c, b = w.getframerate(), w.getnchannels(), w.getsampwidth()
        raw = w.readframes(w.getnframes())
        if b == 2:
            x = np.frombuffer(raw, '<i2') / 32768
        elif b == 3:
            a = np.frombuffer(raw, 'u1').reshape(-1, 3).astype(np.int32)
            x = ((a[:, 0] | a[:, 1] << 8 | a[:, 2] << 16) << 8 >> 8) / 8388608
        else:
            x = np.frombuffer(raw, '<i4') / 2147483648
        return r, x.reshape(-1, c)
    except wave.Error:
        import struct
        d = open(path, 'rb').read()
        fmt = d.find(b'fmt ')
        tag, c, r = struct.unpack_from('<HHI', d, fmt + 8)
        bits = struct.unpack_from('<H', d, fmt + 22)[0]
        at = d.find(b'data')
        n = struct.unpack_from('<I', d, at + 4)[0]
        kind = '<f4' if bits == 32 else '<f8'
        return r, np.frombuffer(d, kind, n // (bits // 8), at + 8).reshape(-1, c).astype(float)

r, x = load(sys.argv[1])
print('rate', r, 'frames', len(x), 'seconds %.2f' % (len(x) / r), 'peak', np.abs(x).max(0).round(3))
ch = int(sys.argv[2]) if len(sys.argv) > 2 else 0
v = x[:, ch]
d = np.abs(np.diff(v))
level = np.percentile(d, 99.9)
print('step 99.9%% %.4f, max %.4f, steps over 0.5: %d, over 1.0: %d' % (level, d.max(), (d > .5).sum(), (d > 1).sum()))
print('samples at the top', (np.abs(v) > .98 * np.abs(v).max()).sum())
hits = np.nonzero(d > max(.5 * d.max(), 3 * level))[0]
groups = np.split(hits, np.nonzero(np.diff(hits) > r // 100)[0] + 1) if len(hits) else []
print('click groups', len(groups))
for g in groups[:14]:
    i = g[0]
    print('  %.4f s, %d steps, around:' % (i / r, len(g)), np.round(v[i - 3:i + 5], 3).tolist())
for t in np.arange(0, len(v) / r, max(1, len(v) / r / 14)):
    a = int(t * r)
    y = v[a:a + r // 2]
    if len(y) < 2048:
        continue
    f = np.abs(np.fft.rfft(y * np.hanning(len(y))))
    fr = np.fft.rfftfreq(len(y), 1 / r)
    print('%.1f s rms %.3f peak %.3f top %d Hz, over 8 kHz %.1f%%' % (t, y.std(), np.abs(y).max(), fr[f.argmax()], 100 * (f[fr > 8000] ** 2).sum() / (f ** 2).sum()))
