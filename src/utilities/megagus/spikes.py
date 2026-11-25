#!/usr/bin/env python3
"""spikes.py FILE.wav [SECONDS] : frames that stand out from both their neighbours, against the
sound's own loudness (rms): one-frame spikes, as an engine before version 3 made them when a
voice was stopped with CTRL = 0. Works on a recording over HDMI as on the model's output."""
import sys, wave
import numpy as np
w = wave.open(sys.argv[1]); n = w.getnframes(); wd = w.getsampwidth()
x = np.frombuffer(w.readframes(n), dtype='<i2' if wd == 2 else '<f4').reshape(-1, w.getnchannels()).astype(float)
m = x.mean(axis=1)
# from the first sound on
loud = np.nonzero(np.abs(m - np.median(m)) > 0.02 * np.abs(m - np.median(m)).max())[0]
m = m[loud[0]:]
if len(sys.argv) > 2:
    m = m[:int(float(sys.argv[2]) * w.getframerate())]
m = m - np.convolve(m, np.ones(9600) / 9600, mode='same')
d = m[1:-1] - (m[:-2] + m[2:]) / 2
rms = np.sqrt((m ** 2).mean())
out = []
for k in (1.0, 1.5, 2.0):
    idx = np.nonzero(np.abs(d) > k * rms)[0]
    out.append(len([a for i, a in enumerate(idx) if i == 0 or a - idx[i - 1] > 480]))
print(f"{sys.argv[1]}: {len(m)/w.getframerate():.0f} s; frames standing out by more than 1, 1.5, 2 times the rms: {out[0]}, {out[1]}, {out[2]}")
