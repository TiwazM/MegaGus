#!/usr/bin/env python3
"""
Runs megagustest.c on this computer against the engine's model, before (or instead of) a
MEGA65: the same C source, built with the host's compiler, its far memory accesses and video
frames answered from megagus_test.py's MegaGus.

  ./megagustest_host.py      prints what the program would put on the MEGA65's screen, checks
                             its sounds, and writes them to work/megagustest.wav to listen to

Time passes as it would on the MEGA65: a video frame is 1/50 s (960 output frames), a
register access some microseconds.
"""

import os
import struct
import subprocess
import sys
import wave

from megagus_test import MegaGus

RATE = 48000
ACCESS = 10e-6          # a far peek or poke with the DMA, from C
PER_BYTE = 0.3e-6       # a DMA copy, a byte
VIDEO_FRAME = 1 / 50


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    work = os.path.join(here, "work")
    os.makedirs(work, exist_ok=True)
    exe = os.path.join(work, "megagustest_host")
    subprocess.run(["cc", "-O1", "-Wall", "-o", exe, os.path.join(here, "megagustest.c")], check=True)

    m = MegaGus(19)
    left, right = [], []
    marks = []                      # (output frame, the screen's text so far) at every line
    screen = ""
    now = 0.0

    def run_until(t):
        while len(left) < int(t * RATE):
            l, r = m.frame()
            left.append(l)
            right.append(r)

    p = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
    for line in p.stdout:
        op, _, rest = line.rstrip("\n").partition(" ")
        if op == "P":
            text = rest.replace("\\n", "\n")
            screen += text
            sys.stdout.write(text)
            sys.stdout.flush()
            marks.append((len(left), text))
        elif op == "F":
            now += VIDEO_FRAME
            run_until(now)
        elif op == "R":
            now += ACCESS
            run_until(now)
            a = int(rest, 16)
            v = m.read(1, a - 0xE000000) if a < 0xEFFF000 else m.read(0, a - 0xEFFF000)
            p.stdin.write(f"{v:x}\n")
            p.stdin.flush()
        elif op == "W":
            now += ACCESS
            run_until(now)
            a, v = (int(x, 16) for x in rest.split())
            if a < 0xEFFF000:
                m.write(1, a - 0xE000000, v)
            else:
                m.write(0, a - 0xEFFF000, v)
        elif op == "C":
            vals = [int(x, 16) for x in rest.split()]
            now += ACCESS + PER_BYTE * (len(vals) - 1)
            run_until(now)
            for i, v in enumerate(vals[1:]):
                m.write(1, vals[0] - 0xE000000 + i, v)
        elif op == "D":
            a, n = rest.split()
            a, n = int(a, 16), int(n)
            now += ACCESS + PER_BYTE * n
            run_until(now)
            p.stdin.write(" ".join(f"{m.read(1, a - 0xE000000 + i):x}" for i in range(n)) + "\n")
            p.stdin.flush()
        else:
            sys.exit(f"what is '{line.strip()}'?")
    code = p.wait()
    run_until(now + 0.25)

    with wave.open(os.path.join(work, "megagustest.wav"), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in zip(left, right)))

    # What it played: each numbered line starts a stretch of sound
    def stretch(number):
        starts = [f for f, text in marks if text.startswith(f"{number} ")]
        later = [f for f, text in marks if f > starts[0] and text[:1].isdigit()]
        return starts[0], (later[0] if later else len(left))

    def hz(x):
        ups = sum(1 for a, b in zip(x, x[1:]) if a < 0 <= b)
        return ups * RATE / len(x)

    def peak(x):
        return max(abs(v) for v in x)

    problems = []

    def expect(what, ok):
        if not ok:
            problems.append(what)

    first_sound = stretch(1)[0]
    expect("the tests are silent", peak(left[:first_sound]) == 0 and peak(right[:first_sound]) == 0)
    a, b = stretch(1)
    expect(f"1: 440 Hz on both sides ({hz(left[a:b]):.1f}, {hz(right[a:b]):.1f})",
           abs(hz(left[a:b]) - 440) < 3 and abs(hz(right[a:b]) - 440) < 3)
    expect("1: about the same level on both sides", abs(peak(left[a:b]) - peak(right[a:b])) < 300)
    a, b = stretch(2)
    expect("2: left only", peak(right[(a + b) // 2:b]) == 0 and peak(left[a:b]) > 10000)
    a, b = stretch(3)
    expect("3: right only", peak(left[(a + b) // 2:b]) == 0 and peak(right[a:b]) > 10000)
    a, b = stretch(4)
    q = (b - a) // 4
    expect("4: left to right", peak(left[a:a + q]) > peak(left[b - q:b]) and
           peak(right[a:a + q]) < peak(right[b - q:b]))
    a, b = stretch(5)
    mid = (a + b) // 2
    expect("5: fades out and in", peak(left[mid - 500:mid + 500]) < 1000 < peak(left[a:a + 2000]))
    a, b = stretch(6)
    expect(f"6: a scale going up ({hz(left[a:a + 9000]):.0f} to {hz(left[b - 21000:b - 13000]):.0f} Hz)",
           abs(hz(left[a + 1000:a + 9000]) - 261.6) < 8 and abs(hz(left[b - 21000:b - 13000]) - 523.3) < 8)
    a, b = stretch(7)
    expect(f"7: 440 Hz from the 16-bit sample ({hz(left[a:b]):.1f})", abs(hz(left[a:b - 12000]) - 440) < 3)
    a, b = stretch(8)
    expect("8: four hits, left right left right",
           all((peak(left[a + i * 24000:a + i * 24000 + 6000]) > peak(right[a + i * 24000:a + i * 24000 + 6000]))
               == (i % 2 == 0) for i in range(4)))
    a, b = stretch(9)
    expect(f"9: a chord that doesn't clip (peak {peak(left[a:b])}, {peak(right[a:b])})",
           3000 < peak(left[a:b]) < 32767 and 3000 < peak(right[a:b]) < 32767)
    expect("silent at the end", peak(left[-2000:]) == 0 and peak(right[-2000:]) == 0)
    expect("the program's own tests pass", "tests passed" in screen and code == 0)

    print()
    print(f"{len(left) / RATE:.1f} s of sound in {os.path.relpath(os.path.join(work, 'megagustest.wav'))}")
    if problems:
        print("FAIL:\n  " + "\n  ".join(problems))
        sys.exit(1)
    print("PASS: the program's tests pass on the model, and its sounds are what it says they are")


if __name__ == "__main__":
    main()
