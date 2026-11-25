#!/usr/bin/env python3
"""
Lays a recording of the MEGA65 (m65rs live record, over HDMI) over what the engine's model
plays for the same module (modplay_host.py): is it the same music?

  ./hwcompare.py RECORDING.wav MODEL.wav [--seconds S]

The recording may begin before the song does, and it carries a constant offset: the song's
start is its first sound once that offset is taken off, moved by up to a second to where the
loudness of the two rises and falls together (a song that fades in has no first sound that
could be told from the capture card's noise). From there the two are compared a
second at a time by what is in them, not sample by sample: the player on the machine looks at
its clock a little earlier or later than the one on the model, so a note can begin a fraction
of a millisecond apart, which leaves the music the same and the waveforms not. Per second:

  spectrum   how alike the two spectra are (1: the same notes at the same strengths), taken
             over short windows and averaged
  level      the recording's loudness against the model's, in dB, less the difference there
             is over the whole (the MEGA65's mixer and the capture card have their own level)
  balance    how much of the sound is on the right, in both

A capture card can drop or repeat samples; that shows as seconds that drift apart.
"""
import argparse
import sys
import wave

import numpy as np

WIN, HOP = 4096, 1200


def load(path):
    w = wave.open(path)
    n = w.getnframes()
    width = w.getsampwidth()
    raw = w.readframes(n)
    x = np.frombuffer(raw, dtype="<i2" if width == 2 else "<f4").reshape(-1, w.getnchannels()).astype(np.float64)
    if width == 2:
        x /= 32768.0
    return x, w.getframerate()


def without_offset(x, rate):
    """x less its mean over the surrounding fifth of a second"""
    k = rate // 5
    c = np.cumsum(np.vstack([np.zeros((1, x.shape[1])), x]), axis=0)
    mean = (c[k:] - c[:-k]) / k
    out = x.copy()
    out[k // 2:k // 2 + len(mean)] -= mean
    out[:k // 2] -= mean[0]
    out[k // 2 + len(mean):] -= mean[-1]
    return out


def first_sound(x, level):
    loud = np.nonzero(np.abs(x).max(axis=1) >= level)[0]
    return int(loud[0]) if len(loud) else 0


def loudness(x, hop):
    m = x.sum(axis=1)
    n = len(m) // hop
    return np.sqrt((m[:n * hop].reshape(n, hop) ** 2).mean(axis=1))


def settle(rec, mod, rate, lead, seconds=20):
    """A song that fades in has no first sound to go by. rec begins `lead` samples before
    where the song was taken to begin: how many samples later (or earlier) it does begin, by
    how the loudness of the two rises and falls over the first seconds, to 2 ms."""
    hop = rate // 500
    er, em = loudness(rec, hop), loudness(mod[:seconds * rate], hop)
    em = em - em.mean()
    best, at = -2.0, lead // hop
    for l in range(0, min(2 * (lead // hop), len(er) - len(em)) + 1):
        a = er[l:l + len(em)]
        a = a - a.mean()
        c = float(np.dot(a, em) / (np.linalg.norm(a) * np.linalg.norm(em) + 1e-12))
        if c > best:
            best, at = c, l
    return at * hop - (lead // hop) * hop


def spectra(x):
    """Magnitude spectra of x (mono) in windows of WIN every HOP"""
    w = np.hanning(WIN)
    n = (len(x) - WIN) // HOP
    return np.array([np.abs(np.fft.rfft(x[i * HOP:i * HOP + WIN] * w)) for i in range(max(n, 0))])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("recording")
    ap.add_argument("model")
    ap.add_argument("--seconds", type=float, default=30.0)
    a = ap.parse_args()
    rec, rr = load(a.recording)
    mod, mr = load(a.model)
    if rr != mr:
        sys.exit(f"the recording is at {rr} Hz, the model at {mr}")
    # off both: a module can hold a steady level itself (a sample that ends away from zero),
    # which the model has and a recording, past the capture card, may not
    rec = without_offset(rec, rr)
    mod = without_offset(mod, rr)
    begin = first_sound(rec, 0.02 * np.abs(rec).max())
    mod = mod[first_sound(mod, 0.02 * np.abs(mod).max()):]
    begin = max(0, begin + settle(rec[max(0, begin - rr):], mod, rr, min(begin, rr)))
    rec = rec[begin:]
    n = int(min(len(rec), len(mod), a.seconds * rr))
    rec, mod = rec[:n], mod[:n]
    print(f"the song begins {begin / rr:.2f} s into the recording; comparing {n / rr:.0f} s")

    sr, sm = spectra(rec.sum(axis=1)), spectra(mod.sum(axis=1))
    per = rr // HOP                 # windows a second
    gain = 20 * np.log10(np.sqrt((rec ** 2).mean()) / np.sqrt((mod ** 2).mean()))
    sims, levels = [], []
    for sec in range(n // rr - 1):
        best = []
        for i in range(sec * per, (sec + 1) * per):
            if i >= len(sr):
                break
            # the model's window that fits this one best, within a tenth of a second
            c = [float(np.dot(sr[i], sm[j]) / (np.linalg.norm(sr[i]) * np.linalg.norm(sm[j]) + 1e-12))
                 for j in range(max(0, i - 4), min(len(sm), i + 5))]
            best.append(max(c))
        r_, m_ = rec[sec * rr:(sec + 1) * rr], mod[sec * rr:(sec + 1) * rr]
        lr, lm = np.sqrt((r_ ** 2).mean(axis=0)), np.sqrt((m_ ** 2).mean(axis=0))
        db = 20 * np.log10(np.sqrt((r_ ** 2).mean()) / max(np.sqrt((m_ ** 2).mean()), 1e-9)) - gain
        sims.append(float(np.mean(best)))
        levels.append(db)
        print(f"  {sec:3d} s: spectrum {sims[-1]:.3f}, level {db:+5.1f} dB, "
              f"on the right {lr[1] / (lr.sum() + 1e-12):.2f} (model {lm[1] / (lm.sum() + 1e-12):.2f})")
    print(f"spectrum: median {np.median(sims):.3f}, lowest {min(sims):.3f}; level within "
          f"{max(abs(x) for x in levels):.1f} dB of the model's; the recording is {gain:+.1f} dB against the model")
    return 0


if __name__ == "__main__":
    sys.exit(main())
