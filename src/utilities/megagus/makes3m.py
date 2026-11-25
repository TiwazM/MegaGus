#!/usr/bin/env python3
"""
Makes the Scream Tracker 3 modules modplay is tried with, in work/:

  test.s3m   one command after another on the first channel, over held notes on the others:
             modcompare.py plays it against libopenmpt, modplay_host.py's self-test plays it
             with both builds of the player
  test_it.s3m, test_300.s3m
             the same, as if saved by Impulse Tracker 2.14 and by Scream Tracker 3.00: a
             player treats those differently (tuning, what commands remember, a sample number
             with no note, volume slides on a row's first tick)
  big.s3m    a megabyte of samples (600 KB of 16-bit, 400 KB of 8-bit, a short loop): more
             than the sample RAM holds, so the player has to make the first 8-bit and keep
             the second at half its rate; each still has to sound at 440 Hz
  big_signed.s3m
             the same with signed samples (an S3M's are unsigned as a rule, a MOD's signed)

Everything in them is made here: no samples or tunes from anywhere else.
"""

import math
import os
import struct

NOTES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]


def note(name):
    """'C-4' -> $40; '^^^' cuts the note"""
    return 254 if name == "^^^" else (int(name[2]) << 4) | NOTES.index(name[:2])


def pad(b):
    return bytes(b) + bytes(-len(b) % 16)


class S3M:
    def __init__(self, title, channels=(0, 8, 1, 9), speed=6, tempo=125, version=0x1320):
        self.title = title
        self.channels = channels    # 0..7 left, 8..15 right
        self.speed, self.tempo, self.version = speed, tempo, version
        # Scream Tracker 3.20 itself writes 16, 24 or 32 here (the GUS's "ultra click" removal):
        # with 0 a player takes the file for another program's
        self.click = 16 if version >> 12 == 1 else 0
        self.signed = False         # the samples are given unsigned; written signed if this is set
        self.samples = []           # (name, data, bits, loop or None, rate at C-4, volume)
        self.patterns = []          # 64 rows of {channel: [note, sample, volume, command, argument]}
        self.orders = []

    def sample(self, name, data, loop=None, rate=8363, volume=64, bits=8):
        self.samples.append((name, data, bits, loop, rate, volume))
        return len(self.samples)

    def pattern(self):
        self.patterns.append([dict() for _ in range(64)])
        return len(self.patterns) - 1

    def put(self, pat, row, ch, n=None, smp=0, vol=None, fx=None, arg=0):
        """fx is the command's letter"""
        self.patterns[pat][row][ch] = [note(n) if n else None, smp, vol, ord(fx) - 64 if fx else None, arg]

    def write(self, path):
        head = bytearray(0x60)
        head[0:len(self.title)] = self.title.encode()
        head[0x1C], head[0x1D] = 0x1A, 16
        head[0x20:0x2C] = struct.pack("<HHHHHH", len(self.orders), len(self.samples), len(self.patterns),
                                      0, self.version, 1 if self.signed else 2)
        head[0x2C:0x30] = b"SCRM"
        head[0x30:0x36] = bytes([64, self.speed, self.tempo, 0xB0, self.click, 252])
        head[0x40:0x60] = bytes(list(self.channels) + [255] * (32 - len(self.channels)))
        packed = []
        for rows in self.patterns:
            out = bytearray()
            for row in rows:
                for ch, c in sorted(row.items()):
                    has_note = c[0] is not None or c[1]
                    w = ch | (32 if has_note else 0) | (64 if c[2] is not None else 0) | (128 if c[3] else 0)
                    out.append(w)
                    if has_note:
                        out += bytes([255 if c[0] is None else c[0], c[1]])
                    if c[2] is not None:
                        out.append(c[2])
                    if c[3]:
                        out += bytes([c[3], c[4]])
                out.append(0)
            packed.append(pad(struct.pack("<H", len(out) + 2) + out))
        at = 0x60 + len(self.orders) + 2 * len(self.samples) + 2 * len(self.patterns) + 32
        at += -at % 16
        ins_at = [at + 0x50 * i for i in range(len(self.samples))]
        at += 0x50 * len(self.samples)
        pat_at = []
        for p in packed:
            pat_at.append(at)
            at += len(p)
        data_at = []
        for s in self.samples:
            data_at.append(at)
            at += len(pad(s[1]))
        out = bytes(head) + bytes(self.orders)
        out += b"".join(struct.pack("<H", a // 16) for a in ins_at + pat_at) + bytes(32)
        out = pad(out)
        for (name, data, bits, loop, rate, volume), where in zip(self.samples, data_at):
            h = bytearray(0x50)
            frames = len(data) // (bits // 8)
            h[0] = 1
            h[0x0D] = (where // 16) >> 16
            h[0x0E:0x10] = struct.pack("<H", (where // 16) & 0xFFFF)
            h[0x10:0x1C] = struct.pack("<III", frames, loop[0] if loop else 0, loop[1] if loop else 0)
            h[0x1C] = volume
            h[0x1F] = (1 if loop else 0) | (4 if bits == 16 else 0)
            h[0x20:0x24] = struct.pack("<I", rate)
            h[0x30:0x30 + len(name)] = name.encode()
            h[0x4C:0x50] = b"SCRS"
            out += h
        def stored(data, bits):
            if not self.signed:
                return data
            if bits == 8:
                return bytes(b ^ 0x80 for b in data)
            return bytes(b ^ 0x80 if i & 1 else b for i, b in enumerate(data))

        out += b"".join(packed) + b"".join(pad(stored(s[1], s[2])) for s in self.samples)
        with open(path, "wb") as f:
            f.write(out)
        return out


def sine8(frames, cycles, amp=110):
    return bytes(int(128 + amp * math.sin(2 * math.pi * cycles * i / frames)) for i in range(frames))


def make_test(version=0x1320):
    m = S3M("megagus s3m test", version=version)
    tone = m.sample("tone", sine8(64, 1), loop=(0, 64))
    odd = m.sample("another rate, quieter", sine8(64, 1), loop=(0, 64), rate=11025, volume=48)
    shot = m.sample("no loop", sine8(4000, 60), rate=8363)

    def hold(p, rows=(0,)):
        # the other three channels: something steady to hear the first against
        for r in rows:
            m.put(p, r, 1, "C-4", odd)
            m.put(p, r, 2, "E-4", tone, vol=24)
            m.put(p, r, 3, "G-3", tone, vol=24)

    # volumes
    p = m.pattern()
    hold(p)
    m.put(p, 0, 0, "C-4", tone)
    m.put(p, 2, 0, fx="D", arg=0x04)
    m.put(p, 4, 0, fx="D", arg=0x40)
    m.put(p, 6, 0, fx="D", arg=0x0F)
    m.put(p, 8, 0, "C-4", tone, fx="D", arg=0xF2)
    m.put(p, 10, 0, fx="D", arg=0x2F)
    m.put(p, 11, 0, fx="D", arg=0x00)
    m.put(p, 12, 0, fx="D", arg=0x24)
    m.put(p, 14, 0, vol=20)
    m.put(p, 16, 0, "D-4", odd, vol=40)
    m.put(p, 18, 0, smp=tone)                   # a sample number alone: its volume
    m.put(p, 20, 0, "C-4", shot, fx="Q", arg=0x23)
    m.put(p, 21, 0, fx="Q", arg=0x00)
    m.put(p, 22, 0, "C-4", tone, fx="Q", arg=0x92)
    m.put(p, 23, 0, fx="Q", arg=0xE1)
    m.put(p, 24, 0, "C-4", tone, fx="I", arg=0x12)
    m.put(p, 25, 0, fx="I", arg=0x00)
    m.put(p, 26, 0, fx="I", arg=0x21)
    m.put(p, 28, 0, "C-4", tone, fx="R", arg=0x48)
    m.put(p, 29, 0, fx="R", arg=0x00)
    m.put(p, 30, 0, fx="R", arg=0x8F)
    m.put(p, 32, 0, "C-4", tone, fx="V", arg=0x20)
    m.put(p, 34, 0, fx="V", arg=0x40)
    m.put(p, 36, 0, "^^^")
    m.put(p, 38, 0, smp=tone)                   # after a cut: nothing
    m.put(p, 40, 0, "C-4", tone, fx="S", arg=0xC2)
    m.put(p, 42, 0, "E-4", tone, fx="S", arg=0xD3)
    m.put(p, 44, 0, "C-4", tone, fx="H", arg=0x46)
    m.put(p, 45, 0, fx="K", arg=0x02)
    m.put(p, 46, 0, fx="K", arg=0x00)
    m.put(p, 48, 0, "C-4", tone)
    m.put(p, 49, 0, "G-4", fx="G", arg=0x10)
    m.put(p, 50, 0, fx="L", arg=0x02)
    m.put(p, 51, 0, fx="L", arg=0x20)
    m.put(p, 52, 0, "C-4", tone, vol=64)
    m.put(p, 53, 0, smp=odd)                    # another sample's number with no note
    m.put(p, 54, 0, vol=0)
    m.put(p, 56, 0, "C-4", shot, fx="O", arg=0x04)
    m.put(p, 58, 0, "C-4", tone, fx="X", arg=0x20)
    m.put(p, 60, 0, fx="S", arg=0x8C)
    m.put(p, 62, 0, fx="X", arg=0x40)
    vols = p

    # pitches
    p = m.pattern()
    hold(p)
    m.put(p, 0, 0, "C-4", tone)
    m.put(p, 1, 0, fx="E", arg=0x04)
    m.put(p, 2, 0, fx="F", arg=0x04)
    m.put(p, 3, 0, fx="E", arg=0xF4)
    m.put(p, 4, 0, fx="F", arg=0xF4)
    m.put(p, 5, 0, fx="E", arg=0xE8)
    m.put(p, 6, 0, fx="F", arg=0xE8)
    m.put(p, 7, 0, fx="E", arg=0x00)
    m.put(p, 8, 0, "C-4", tone)
    m.put(p, 9, 0, "C-5", fx="G", arg=0x20)
    m.put(p, 10, 0, fx="G", arg=0x00)
    m.put(p, 11, 0, fx="G", arg=0x00)
    m.put(p, 12, 0, "C-4", tone, fx="H", arg=0x46)
    m.put(p, 13, 0, fx="H", arg=0x00)
    m.put(p, 14, 0, fx="H", arg=0x80)
    m.put(p, 15, 0, fx="H", arg=0x0F)
    m.put(p, 16, 0, "C-4", tone, fx="U", arg=0x4F)
    m.put(p, 17, 0, fx="U", arg=0x00)
    m.put(p, 18, 0, "C-4", tone, fx="J", arg=0x37)
    m.put(p, 19, 0, fx="J", arg=0x00)
    m.put(p, 20, 0, fx="J", arg=0xC0)
    m.put(p, 22, 0, "C-4", tone, fx="S", arg=0x22)
    m.put(p, 24, 0, "C-4", tone, fx="S", arg=0x2E)
    m.put(p, 26, 0, "C-4", tone, fx="S", arg=0x31)
    m.put(p, 27, 0, fx="H", arg=0x38)
    m.put(p, 28, 0, fx="S", arg=0x32)
    m.put(p, 29, 0, fx="H", arg=0x38)
    m.put(p, 30, 0, fx="S", arg=0x30)
    m.put(p, 32, 0, "C-2", odd)
    m.put(p, 34, 0, "B-6", odd)
    m.put(p, 36, 0, "C-4", tone, fx="E", arg=0x40)
    m.put(p, 37, 0, fx="E", arg=0x40)
    m.put(p, 38, 0, fx="F", arg=0x60)
    m.put(p, 39, 0, fx="F", arg=0x60)
    m.put(p, 40, 0, "C-4", tone)
    m.put(p, 41, 0, "E-4", odd, fx="G", arg=0x08)    # a slide to a note with another sample
    m.put(p, 42, 0, fx="G", arg=0x00)
    m.put(p, 44, 0, "C-4", tone, fx="S", arg=0xD2)
    m.put(p, 46, 0, "C-4", tone, fx="S", arg=0xD9)   # later than the row is long
    m.put(p, 48, 0, "C-4", tone)
    pitches = p

    # the way through the song
    p = m.pattern()
    hold(p)
    m.put(p, 0, 0, "C-4", tone, fx="A", arg=0x03)
    m.put(p, 4, 0, "D-4", tone, fx="T", arg=0x90)
    m.put(p, 8, 0, "E-4", tone, fx="A", arg=0x06)
    m.put(p, 9, 0, fx="T", arg=0x7D)
    m.put(p, 10, 0, "C-4", tone, fx="S", arg=0xB0)
    m.put(p, 12, 0, "D-4", tone, fx="S", arg=0xB2)
    m.put(p, 14, 0, "E-4", tone, fx="S", arg=0xE2)
    m.put(p, 16, 0, "F-4", tone, fx="T", arg=0x10)   # not a tempo
    m.put(p, 18, 0, "G-4", tone, fx="C", arg=0x20)
    flow = p
    p = m.pattern()
    hold(p, (20,))
    m.put(p, 0, 0, "C-5", tone)                      # passed over by the break
    m.put(p, 20, 0, "A-4", tone)
    m.put(p, 24, 0, "B-4", tone, fx="B", arg=0x05)
    after = p
    p = m.pattern()
    hold(p)
    m.put(p, 0, 0, "C-5", tone)
    m.put(p, 8, 0, "^^^", fx="C", arg=0x00)
    last = p
    m.orders = [vols, pitches, 254, flow, after, last, 255, vols]
    return m


def make_big(signed=False):
    m = S3M("megagus big test", channels=(0, 8, 1))
    m.signed = signed
    rate = 22050
    wide = m.sample("16-bit", b"".join(struct.pack("<H", int(32768 + 30000 * math.sin(2 * math.pi * 440 * i / rate)))
                                       for i in range(300000)), rate=rate, bits=16)
    long_ = m.sample("long", bytes(int(128 + 110 * math.sin(2 * math.pi * 440 * i / rate)) for i in range(400000)),
                     rate=rate)
    loop = m.sample("loop", sine8(64, 1), loop=(0, 64))
    p = m.pattern()
    m.put(p, 0, 0, "C-4", wide)
    m.put(p, 15, 0, "^^^")
    m.put(p, 16, 1, "C-4", long_)
    m.put(p, 31, 1, "^^^")
    m.put(p, 32, 2, "C-4", loop)
    m.put(p, 47, 2, "^^^")
    m.put(p, 48, 1, "C-4", long_, fx="O", arg=0x80)     # 32 768 frames in
    m.orders = [p, 255]
    return m


def main():
    work = os.path.join(os.path.dirname(os.path.abspath(__file__)), "work")
    os.makedirs(work, exist_ok=True)
    for name, m in (("test.s3m", make_test()), ("test_it.s3m", make_test(0x3214)),
                    ("test_300.s3m", make_test(0x1300)), ("big.s3m", make_big()),
                    ("big_signed.s3m", make_big(True))):
        data = m.write(os.path.join(work, name))
        print(f"{name}: {len(data)} bytes, {len(m.patterns)} patterns")


if __name__ == "__main__":
    main()
