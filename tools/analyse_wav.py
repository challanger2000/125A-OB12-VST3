#!/usr/bin/env python3
"""Per-second level and spectrum of a stereo 48 kHz WAV from ob12_system."""
import sys
import wave

import numpy as np

w = wave.open(sys.argv[1])
n = w.getnframes()
d = np.frombuffer(w.readframes(n), dtype="<i2").astype(float).reshape(-1, 2)
print(f"{n / 48000:.1f} s, peak {abs(d).max():.0f}")
print("rms per second L", [round(d[i:i + 48000, 0].std(), 1) for i in range(0, n, 48000)])


def spectrum(name, a, b):
    seg = d[int(a * 48000):int(b * 48000), 0]
    seg = seg - seg.mean()
    if seg.std() < 0.5:
        print(name, "silent")
        return
    amp = np.abs(np.fft.rfft(seg * np.hanning(len(seg))))
    f = np.fft.rfftfreq(len(seg), 1 / 48000)
    flat = np.exp(np.mean(np.log(amp[10:] + 1e-9))) / np.mean(amp[10:])
    peaks = [i for i in range(2, len(amp) - 1) if amp[i] > amp[i - 1] and amp[i] > amp[i + 1] and amp[i] > 0.1 * amp.max()]
    top = sorted(sorted(peaks, key=lambda i: -amp[i])[:10])
    print(name, f"rms {seg.std():.1f} flatness {flat:.3f}", [(round(float(f[i]), 1), round(float(amp[i] / amp.max()), 2)) for i in top])


for a, b in zip(sys.argv[2::2], sys.argv[3::2]):
    spectrum(f"{a}-{b} s", float(a), float(b))
