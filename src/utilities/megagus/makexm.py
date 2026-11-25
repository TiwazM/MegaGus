#!/usr/bin/env python3
"""
Makes the XM test modules for modplay (work/test.xm, work/test_amiga.xm, work/big.xm).

test.xm goes through FastTracker 2's commands and its instruments' envelopes, a channel at a
time, so that modcompare.py can lay modplay's playing of each over libopenmpt's; test_amiga.xm
is the same song with Amiga periods instead of linear ones. big.xm has more samples than the
sample RAM holds, a 16-bit one among them, so the player has to make them smaller.

  ./makexm.py            writes all three
"""
import math
import os

import xmfile

HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "work")

C4 = 49          # a note: 1 is C-0
OFF = 97         # the key goes up
G, H, K, L, P, R, T, X = 0x10, 0x11, 0x14, 0x15, 0x19, 0x1B, 0x1D, 0x21


def sine(n, amp=100, bits=8):
    top = amp if bits == 8 else amp * 256
    return [int(round(top * math.sin(2 * math.pi * i / n))) for i in range(n)]


def instruments():
    out = []

    def add(name, samples, **kw):
        ins = xmfile.Instrument(name, samples)
        for k, v in kw.items():
            setattr(ins, k, v)
        out.append(ins)
        return ins

    loop = lambda **kw: xmfile.Sample(sine(32), 8, (0, 32), **kw)      # noqa: E731
    add("1 plain", [loop()])
    add("2 envelope, sustain, fade", [loop()], vol_on=True, vol_env=[(0, 64), (10, 40), (30, 20), (60, 0)],
        vol_sustain=1, fadeout=600)
    add("3 envelope loop, panning", [loop()], vol_on=True, vol_env=[(0, 20), (6, 64), (12, 30), (40, 0)],
        vol_loop=(0, 2), pan_on=True, pan_env=[(0, 32), (12, 60), (24, 4), (36, 32)], pan_loop=(0, 3), fadeout=300)
    add("4 its own vibrato", [loop()], vib_type=0, vib_sweep=0, vib_depth=8, vib_rate=20)
    add("5 vibrato growing", [loop()], vib_type=0, vib_sweep=40, vib_depth=12, vib_rate=16,
        vol_on=True, vol_env=[(0, 64), (100, 64)], vol_sustain=0, fadeout=200)
    add("6 tuned, left, softer", [loop(relnote=12, finetune=40, pan=48, volume=40)])
    two = add("7 two samples", [loop(), loop(relnote=-12, pan=200, volume=50)])
    two.notemap = [0] * 48 + [1] * 48
    add("8 16-bit, back and forth", [xmfile.Sample(sine(64, 100, 16)[:48], 16, (8, 40), pingpong=True)])
    add("9 no loop", [xmfile.Sample(sine(32) * 150, 8)], vol_on=True, vol_env=[(0, 64), (20, 0)])
    add("10 square vibrato, envelope sustain at loop end", [loop()], vib_type=1, vib_sweep=0, vib_depth=4, vib_rate=24,
        vol_on=True, vol_env=[(0, 30), (8, 64), (16, 30), (50, 0)], vol_loop=(0, 2), vol_sustain=2, fadeout=0)
    add("11 ramp vibratos", [loop(), loop()], vib_type=2, vib_sweep=0, vib_depth=6, vib_rate=30)
    out.append(xmfile.Instrument("12 nothing"))
    add("13 ramp down", [loop()], vib_type=3, vib_sweep=10, vib_depth=6, vib_rate=30)
    return out


def phrases():
    """What each of the four channels plays: lists of [note, instrument, volume column, command, argument]"""
    e = [0, 0, 0, 0, 0]
    a, b, c, d = [], [], [], []

    def n(note=0, ins=0, vol=0, fx=0, arg=0):
        return [note, ins, vol, fx, arg]

    # channel 1: pitch
    a += [n(C4, 1), e, n(fx=0, arg=0x47), n(fx=0, arg=0x47), n(fx=0, arg=0x3C), e]            # arpeggio
    a += [n(C4, 1), n(fx=1, arg=4), n(fx=1), n(fx=2, arg=6), n(fx=2), n(fx=1, arg=0x20), e]   # slides, remembered
    a += [n(C4 + 12, 1), n(C4, 0, 0, 3, 8), n(fx=3), n(fx=3), n(fx=3), n(fx=3, arg=0x40), n(fx=3), e]  # slide to a note
    a += [n(C4, 1), n(C4 + 7, 0, 0xF4), n(vol=0xF0), n(vol=0xF0), e]                         # the same from the volume column
    a += [n(C4, 1), n(fx=4, arg=0x46), n(fx=4), n(fx=4, arg=0x80), n(fx=4, arg=0x0C), e, e]   # vibrato
    a += [n(fx=0xE, arg=0x41), n(C4, 1, 0, 4, 0x48), n(fx=4), e, n(fx=0xE, arg=0x42), n(C4, 1, 0, 4, 0x68), n(fx=4),
          n(fx=0xE, arg=0x40), e]                                                            # its other waveforms
    a += [n(C4, 1, 0xA6), n(vol=0xB4), n(vol=0xB0), n(vol=0xB8), e]                          # vibrato from the column
    a += [n(C4, 1), n(C4 + 5, 0, 0, 5, 0x02), n(fx=5, arg=0x20), n(fx=5), e]                  # slide to a note, volume slide
    a += [n(C4, 1, 0, 4, 0x46), n(fx=6, arg=0x03), n(fx=6, arg=0x30), n(fx=6), e]             # vibrato, volume slide
    a += [n(C4, 1), n(fx=0xE, arg=0x13), n(fx=0xE, arg=0x10), n(fx=0xE, arg=0x25), n(fx=0xE, arg=0x20), e]  # fine slides
    a += [n(C4, 1), n(fx=X, arg=0x18), n(fx=X, arg=0x10), n(fx=X, arg=0x2C), n(fx=X, arg=0x20), e]          # the finest
    a += [n(C4, 1, 0, 0xE, 0x50), e, n(C4, 1, 0, 0xE, 0x5F), e, n(C4, 6), e, n(C4 + 30, 6), e]  # finetunes, a sample's tuning
    a += [n(C4 - 14, 7), e, n(C4 + 14, 7), e, n(C4, 8), e, n(C4 + 12, 8), e]                  # two samples, the 16-bit one
    a += [n(C4, 4), e, e, e, n(C4, 5), e, e, e, e, e, n(OFF), e, e, n(C4, 11), e, e, n(C4, 13), e, e, n(C4, 10), e, e, e, n(OFF), e, e]

    # channel 2: volume
    b += [n(C4, 1, 0, 0xC, 0x20), n(fx=0xC, arg=0x40), n(fx=0xC, arg=0x50), n(vol=0x10), n(vol=0x30), n(vol=0x50)]
    b += [n(C4, 1), n(fx=0xA, arg=0x04), n(fx=0xA), n(fx=0xA, arg=0x30), n(fx=0xA), n(fx=0xA, arg=0x24), e]
    b += [n(C4, 1, 0x30), n(fx=0xE, arg=0xA4), n(fx=0xE, arg=0xA0), n(fx=0xE, arg=0xB6), n(fx=0xE, arg=0xB0), e]
    b += [n(C4, 1, 0x40), n(vol=0x62), n(vol=0x62), n(vol=0x73), n(vol=0x84), n(vol=0x95), e]
    b += [n(C4, 1), n(fx=7, arg=0x4A), n(fx=7), n(fx=7, arg=0x80), e, n(fx=0xE, arg=0x71), n(C4, 1, 0, 7, 0x4C),
          n(fx=7), n(fx=0xE, arg=0x72), n(C4, 1, 0, 7, 0x4C), n(fx=7), n(fx=0xE, arg=0x70), e]
    b += [n(C4, 1, 0, 0xE, 0xC3), e, n(C4, 1, 0, 0xE, 0xC0), e, n(C4, 1), n(fx=0xE, arg=0xC2), e]
    b += [n(C4, 1), n(fx=T, arg=0x12), n(fx=T), n(fx=T), n(fx=T, arg=0x31), n(fx=T), e, e]
    b += [n(C4, 2), e, e, e, n(OFF), e, e, e, n(C4, 2), e, n(fx=K, arg=3), e, e, n(C4, 2), n(0, 2), e, n(OFF), n(0, 2), e, e]
    b += [n(C4, 3), e, e, e, e, e, n(OFF), e, e, e, n(C4, 3, 0, L, 10), e, n(fx=L, arg=30), e, e]
    b += [n(C4, 9), e, e, e, e, n(C4, 9, 0, 9, 0x08), e, n(C4, 9, 0, 9, 0), e, n(C4, 9, 0, 9, 0x40), e]
    b += [n(C4, 10), e, e, e, e, e, n(OFF), e, e, e, e, n(C4, 1), n(fx=K, arg=0), e, n(C4, 12), e, n(C4, 1), n(0, 12), e]

    # channel 3: places, and notes again
    c += [n(C4, 1, 0, 8, 0x00), n(fx=8, arg=0x40), n(fx=8, arg=0xC0), n(fx=8, arg=0xFF), n(vol=0xC2), n(vol=0xCC), e]
    c += [n(C4, 1, 0, 8, 0x80), n(fx=P, arg=0x08), n(fx=P), n(fx=P, arg=0x80), n(fx=P), n(vol=0xD4), n(vol=0xE6), e]
    c += [n(C4, 6), n(0, 6, 0xC8), n(C4, 1), n(C4, 0, 0xCF), n(0, 1), e]
    c += [n(C4, 1, 0, 0xE, 0x92), n(fx=0xE, arg=0x93), n(fx=0xE, arg=0x91), n(fx=0xE, arg=0x90), n(C4, 1, 0, 0xE, 0x90), e]
    c += [n(C4, 1, 0, R, 0x02), n(fx=R), n(fx=R, arg=0x13), n(fx=R, arg=0x60), n(fx=R, arg=0xE0), n(fx=R, arg=0x74),
          n(vol=0x20, fx=R, arg=0x92), n(vol=0x30, fx=R, arg=0x02), e]
    c += [n(C4, 1, 0, 0xE, 0xD3), e, n(fx=0xE, arg=0xD2), e, n(C4 + 4, 2, 0x30, 0xE, 0xD1), n(0, 0, 0xC4, 0xE, 0xD4), e, e]
    c += [n(C4, 1, 0, 1, 0x10), n(fx=R, arg=0x03), n(fx=R), e, n(C4, 1), n(C4 + 12, 0, 0, 3, 0x10), n(fx=0xE, arg=0x93), e]
    c += [n(C4, 1), n(C4 + 3), n(0, 1, 0x20), n(C4, 0, 0x20), n(OFF), n(C4, 2), n(OFF, 2), e, e, n(0, 2), e, e]
    c += [n(C4 - 24, 1), e, n(C4 + 40, 1), e, n(C4 + 47, 1, 0, 1, 0x40), e, n(C4 - 48, 1, 0, 2, 0x40), e, e]

    # channel 4: the song's own commands
    d += [n(C4, 1), n(fx=G, arg=0x20), n(fx=G, arg=0x40), n(fx=H, arg=0x04), n(fx=H), n(fx=H, arg=0x20), n(fx=H),
          n(fx=G, arg=0x50), e]
    d += [n(C4, 1, 0, 0xF, 3), e, n(fx=0xF, arg=8), e, n(fx=0xF, arg=6), n(fx=0xF, arg=150), e, e, n(fx=0xF, arg=125), e]
    d += [n(C4, 1, 0, 0xE, 0x60), n(C4 + 2), n(C4 + 4, 0, 0, 0xE, 0x62), n(C4 + 5), e]
    d += [n(C4, 1, 0, 0xE, 0xE2), n(C4 + 2, 1, 0, 0xA, 0x04), n(fx=0xE, arg=0xE1), e]
    d += [n(C4, 1, 0, 0, 0x37), n(fx=0xF, arg=4), n(fx=0, arg=0x37), n(fx=0xF, arg=17), n(fx=0, arg=0x37),
          n(fx=0xF, arg=18), n(fx=0, arg=0x37), n(fx=0xF, arg=6), e]
    return a, b, c, d


def make_test(linear=True):
    x = xmfile.XM("modplay's test", 4, 6, 125, linear)
    x.instruments = instruments()
    cols = phrases()
    rows = max(len(col) for col in cols)
    empty = [0, 0, 0, 0, 0]
    song = [[list(col[r]) if r < len(col) else list(empty) for col in cols] for r in range(rows)]
    for at, ln in ((0, 64), (64, 37), (101, rows - 101)):           # patterns of three lengths
        assert ln > 0
        x.patterns.append(song[at:at + ln])
    # a pattern that is left by a break to a row of the next, and one jumped to
    p = x.pattern(16)
    p[0][0] = [C4, 1, 0, 0, 0]
    p[3][0] = [C4 + 2, 1, 0, 0xD, 0x05]
    p = x.pattern(16)
    p[0][0] = [C4 + 4, 1, 0, 0, 0]
    p[5][0] = [C4 + 5, 1, 0, 0, 0]
    p[9][0] = [C4 + 7, 1, 0, 0xB, 6]
    p = x.pattern(8)
    p[0][0] = [C4 + 9, 1, 0, 0, 0]
    p = x.pattern(8)
    p[0][0] = [C4 + 12, 2, 0, 0, 0]
    p[4][0] = [OFF, 0, 0, 0, 0]
    x.orders = [0, 1, 2, 3, 4, 5, 6]
    return x


def make_big():
    """More than the 512 KB: a 16-bit sample of 300 KB (plays A-4 at C-4), an 8-bit one of 400 KB,
    a small loop. The first is kept as 8-bit, the second at half its rate."""
    x = xmfile.XM("too big", 2, 6, 125)
    cycle = 19                              # 8363 / 19 = 440.2 Hz at C-4
    long16 = sine(cycle, 100, 16) * (150000 // cycle)
    long8 = sine(cycle) * (400000 // cycle)
    x.instruments = [xmfile.Instrument("16-bit", [xmfile.Sample(long16, 16, pan=0)]),          # on the left
                     xmfile.Instrument("8-bit", [xmfile.Sample(long8, 8, pan=255)]),            # on the right
                     xmfile.Instrument("loop", [xmfile.Sample(sine(64), 8, (0, 64), pan=0)])]
    p = x.pattern(64)
    p[0][0] = [C4, 1, 0, 0, 0]
    p[15][0] = [OFF, 0, 0, 0, 0]
    p[16][1] = [C4, 2, 0, 0, 0]
    p[31][1] = [OFF, 0, 0, 0, 0]
    p[32][0] = [C4, 3, 0, 0, 0]
    p[47][0] = [OFF, 0, 0, 0, 0]
    p[48][1] = [C4, 2, 0, 9, 0x80]          # the long one again, from 32768 frames in
    x.orders = [0]
    return x


def main():
    os.makedirs(WORK, exist_ok=True)
    for name, x in (("test.xm", make_test()), ("test_amiga.xm", make_test(False)), ("big.xm", make_big())):
        data = xmfile.save(x)
        with open(os.path.join(WORK, name), "wb") as f:
            f.write(data)
        print(f"{name}: {len(data)} bytes, {x.channels} channels, {len(x.patterns)} patterns, "
              f"{len(x.instruments)} instruments")


if __name__ == "__main__":
    main()
