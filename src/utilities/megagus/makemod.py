#!/usr/bin/env python3
"""
Makes the modules modplay is tried with, in work/:

  test.mod, test.json   one command after another, each with what the player must then have
                        told the engine (modplay_host.py checks it)
  test6.mod, test6.json the same for what differs in a module with six channels
  test_vblank.mod, .json and for VBlank timing
  test_switch.mod, test_switch_on.json, test_switch_off.json
                        the timing changed over in the middle, to VBlank and from it
  demo.mod              half a minute of music on four channels, to listen to

Everything in them is made here: no samples or tunes from anywhere else.
"""

import json
import math
import os
import random
import struct

NOTES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]
PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453,
           428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226,
           214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113]


def period(name):
    """'C-2' -> 428"""
    return PERIODS[NOTES.index(name[:2]) + 12 * (int(name[2]) - 1)]


def s8(values):
    return bytes((max(-128, min(127, int(round(v)))) & 0xFF) for v in values)


class Mod:
    def __init__(self, title, channels=4):
        self.title = title
        self.channels = channels
        self.samples = []       # (name, data, finetune, volume, loop start, loop length)
        self.patterns = []      # each: 64 rows of `channels` cells (period, sample, fx, arg)
        self.orders = []

    def sample(self, name, data, volume=64, finetune=0, loop=None):
        data = bytes(data)
        if len(data) & 1:
            data += b"\0"
        ls, ll = loop if loop else (0, 2)
        self.samples.append((name, data, finetune, volume, ls, ll))
        return len(self.samples)

    def pattern(self):
        self.patterns.append([[(0, 0, 0, 0)] * self.channels for _ in range(64)])
        return len(self.patterns) - 1

    def put(self, pat, row, ch, note=None, smp=0, fx=0, arg=0):
        per = period(note) if isinstance(note, str) else (note or 0)
        self.patterns[pat][row] = list(self.patterns[pat][row])
        self.patterns[pat][row][ch] = (per, smp, fx, arg)

    def write(self, path):
        out = self.title.encode().ljust(20, b"\0")[:20]
        for i in range(31):
            if i < len(self.samples):
                name, data, ft, vol, ls, ll = self.samples[i]
                out += name.encode().ljust(22, b"\0")[:22]
                out += struct.pack(">HBBHH", len(data) // 2, ft & 15, vol, ls // 2, ll // 2)
            else:
                out += b"\0" * 22 + struct.pack(">HBBHH", 0, 0, 0, 0, 1)
        out += bytes([len(self.orders), 127]) + bytes(self.orders).ljust(128, b"\0")
        out += {4: b"M.K.", 6: b"6CHN", 8: b"8CHN"}.get(self.channels) or b"%02dCH" % self.channels
        for pat in self.patterns:
            for row in pat:
                for per, smp, fx, arg in row:
                    out += bytes([(smp & 0xF0) | (per >> 8), per & 0xFF, ((smp & 15) << 4) | fx, arg])
        for name, data, ft, vol, ls, ll in self.samples:
            out += data
        with open(path, "wb") as f:
            f.write(out)
        return out


def sample_offsets(mod):
    """Where each sample (1..) lies in the file"""
    at = 1084 + len(mod.patterns) * 64 * 4 * mod.channels
    where = {}
    for i, (name, data, ft, vol, ls, ll) in enumerate(mod.samples, 1):
        where[i] = at
        at += len(data)
    return where


# -----------------------------------------------------------------------------------------------
# The test module
# -----------------------------------------------------------------------------------------------
def freq(per):
    return 4842694 // per


def vol16(v):
    return v * 1023


def make_test():
    m = Mod("modplay test")
    sine = s8(100 * math.sin(2 * math.pi * i / 32) for i in range(32))
    SINE = m.sample("sine, looped", sine, loop=(0, 32))
    HIT = m.sample("a hit, once", s8(110 * math.sin(i / 3) * (1 - i / 2000) for i in range(2000)))
    ATT = m.sample("attack then loop", s8(90 * math.sin(i / 5) for i in range(1000)) + sine, loop=(1000, 32))
    FT3 = m.sample("sine, finetune 3", sine, finetune=3, loop=(0, 32))
    LONG = m.sample("long", s8(60 * math.sin(i / 7) for i in range(4096)), volume=48)
    FTM = m.sample("sine, finetune -2", sine, finetune=14, loop=(0, 32))

    expect = []                 # what modplay_host.py checks
    state = {"pat": m.pattern(), "row": 0}
    m.orders.append(state["pat"])

    def rows(n):
        """The next n rows: (order, pattern, first row). A new pattern if they don't fit."""
        if state["row"] + n + 1 > 63:
            m.put(state["pat"], state["row"], 3, fx=0xD, arg=0)     # break to the next pattern
            state["pat"] = m.pattern()
            state["row"] = 0
            m.orders.append(state["pat"])
        r = state["row"]
        state["row"] += n + 1       # a row of rest after every test
        m.put(state["pat"], r + n, 0, fx=0xC, arg=0)
        return len(m.orders) - 1, state["pat"], r

    def at(order, row, tick, what, value, name, voice=0):
        expect.append({"name": name, "order": order, "row": row, "tick": tick, "voice": voice,
                       "what": what, "value": value})

    P = period("C-2")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE)
    for t in (0, 3, 5):
        at(o, r, t, "freq", freq(P), "a note: its pitch")
        at(o, r, t, "vol", vol16(64), "a note: the sample's volume")
    at(o, r, 2, "running", 1, "a note: the voice runs")
    at(o, r, 2, "hz", 4842694 // P / 65536 * 48000 / 32, "a note: what comes out")
    for ch, pan in enumerate((64, 192, 192, 64)):
        m.put(p, r, ch, "C-2", SINE, 0xC if ch else 0, 0)
        at(o, r, 1, "pan", pan, "the channels left and right", voice=ch)
    m.put(p, r, 0, "C-2", SINE)

    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", FT3)
    at(o, r, 1, "freq", freq(419), "finetune 3: C-2 is period 419")
    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", FTM)
    at(o, r, 1, "freq", freq(round(428 * 2 ** (2 / 96))), "finetune -2")
    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", SINE, 0xE, 0x53)
    at(o, r, 1, "freq", freq(419), "E5x: finetune for this note")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0xC, 0x20)
    at(o, r, 1, "vol", vol16(32), "Cxx: volume")
    m.put(p, r + 1, 0, None, SINE)
    at(o, r + 1, 1, "vol", vol16(64), "a sample number alone: its volume again")
    at(o, r + 1, 1, "starts", 0, "a sample number alone: no new start")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0xA, 0x04)
    for t in range(6):
        at(o, r, t, "vol", vol16(64 - 4 * t), "A0y: volume slides down")
    m.put(p, r + 1, 0, fx=0xA, arg=0x30)
    for t in range(6):
        at(o, r + 1, t, "vol", vol16(min(64, 44 + 3 * t)), "Ax0: volume slides up")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0x1, 0x04)
    for t in range(6):
        at(o, r, t, "freq", freq(P - 4 * t), "1xx: pitch slides up")
    m.put(p, r + 1, 0, fx=0x2, arg=0x08)
    for t in range(6):
        at(o, r + 1, t, "freq", freq(P - 20 + 8 * t), "2xx: pitch slides down")

    o, p, r = rows(1)
    m.put(p, r, 0, "A-3", SINE, 0x1, 0x10)
    at(o, r, 5, "freq", freq(113), "1xx stops at period 113")

    o, p, r = rows(3)
    m.put(p, r, 0, "C-2", SINE)
    m.put(p, r + 1, 0, "E-2", 0, 0x3, 0x10)
    for t in range(6):
        at(o, r + 1, t, "freq", freq(P - 16 * t), "3xx: slides toward the note")
    at(o, r + 1, 3, "starts", 0, "3xx: the note does not start again")
    m.put(p, r + 2, 0, fx=0x3, arg=0)
    for t in (1, 2, 5):
        at(o, r + 2, t, "freq", freq(339), "300: goes on, and stops at the note")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0x4, 0x48)
    for t, d in enumerate((0, 0, 6, 11, 14, 15)):
        at(o, r, t, "freq", freq(P + d), "4xy: vibrato")
    m.put(p, r + 1, 0, fx=0x4, arg=0)
    for t, d in ((0, 15), (1, 14), (2, 11), (5, -6)):
        at(o, r + 1, t, "freq", freq(P + d), "400: vibrato goes on")

    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", SINE, 0x0, 0x47)
    for t, per in enumerate((428, 340, 286, 428, 340, 286)):
        at(o, r, t, "freq", freq(per), "0xy: arpeggio")

    o, p, r = rows(3)
    m.put(p, r, 0, "C-2", SINE, 0xC, 0x20)
    m.put(p, r + 1, 0, fx=0x7, arg=0x48)
    for t, d in enumerate((0, 0, 12, 22, 29, 31)):
        at(o, r + 1, t, "vol", vol16(32 + d), "7xy: tremolo")
    m.put(p, r + 2, 0)
    at(o, r + 2, 1, "vol", vol16(32), "tremolo ends: the volume as it was")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0xC, 0x10)
    m.put(p, r + 1, 0, "E-2", 0, 0x5, 0x20)
    at(o, r + 1, 5, "vol", vol16(26), "5xy: volume slides")
    at(o, r + 1, 5, "freq", freq(P - 80), "5xy: and the tone slides at the speed 3xx left")
    o, p, r = rows(3)
    m.put(p, r, 0, "C-2", SINE, 0xC, 0x10)
    m.put(p, r + 1, 0, "E-2", 0, 0x3, 0x08)
    m.put(p, r + 2, 0, fx=0x5, arg=0x02)
    at(o, r + 2, 5, "freq", freq(P - 80), "5xy: the tone slide goes on")
    at(o, r + 2, 5, "vol", vol16(6), "5xy: and the volume slides")
    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0x4, 0x48)
    m.put(p, r + 1, 0, fx=0x6, arg=0x04)
    at(o, r + 1, 2, "freq", freq(P + 11), "6xy: the vibrato goes on")
    at(o, r + 1, 5, "vol", vol16(44), "6xy: and the volume slides")

    off = None                  # the samples' places are known once all patterns exist
    o9, p9, r9 = rows(2)
    m.put(p9, r9, 0, "C-2", LONG, 0x9, 0x04)
    m.put(p9, r9 + 1, 0, "C-2", LONG, 0x9, 0x00)
    oh, ph, rh = rows(5)
    m.put(ph, rh, 0, "C-3", HIT)
    oa, pa, ra = rows(4)
    m.put(pa, ra, 0, "C-3", ATT)

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0xE, 0x13)
    at(o, r, 0, "freq", freq(P - 3), "E1x: fine slide up")
    at(o, r, 5, "freq", freq(P - 3), "E1x: once only")
    m.put(p, r + 1, 0, fx=0xE, arg=0x25)
    at(o, r + 1, 3, "freq", freq(P + 2), "E2x: fine slide down")
    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0xE, 0xB9)
    at(o, r, 4, "vol", vol16(55), "EBx: fine volume down")
    m.put(p, r + 1, 0, fx=0xE, arg=0xA3)
    at(o, r + 1, 4, "vol", vol16(58), "EAx: fine volume up")

    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", SINE, 0xE, 0xC3)
    at(o, r, 2, "vol", vol16(64), "ECx: sounds until tick x")
    at(o, r, 3, "vol", 0, "ECx: cut at tick x")
    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", SINE, 0xE, 0xD2)
    at(o, r, 1, "starts", 0, "EDx: not yet")
    at(o, r, 5, "starts", 1, "EDx: started later in the row")
    at(o, r, 5, "start tick", 2, "EDx: at tick x")
    o, p, r = rows(1)
    m.put(p, r, 0, "C-2", SINE, 0xE, 0x92)
    at(o, r, 5, "starts", 3, "E9x: started again every x ticks")

    o, p, r = rows(2)
    m.put(p, r, 0, "C-2", SINE, 0x8, 0x20)
    at(o, r, 1, "pan", 64, "8xx: not a ProTracker command, left alone in its modules")
    m.put(p, r + 1, 0, fx=0xE, arg=0x8F)
    at(o, r + 1, 1, "pan", 64, "E8x: the same")

    # how the song moves
    o, p, r = rows(4)
    m.put(p, r, 0, "C-2", SINE)
    m.put(p, r, 1, fx=0xF, arg=3)
    at(o, r, 0, "row frames", 3 * 960, "F03: three ticks a row")
    m.put(p, r + 1, 1, fx=0xF, arg=150)
    at(o, r + 1, 0, "row frames", 3 * 800, "F96: 150 beats a minute")
    m.put(p, r + 2, 1, fx=0xE, arg=0xE2)
    at(o, r + 2, 0, "row frames", 9 * 800, "EEx: the row three times as long")
    m.put(p, r + 3, 1, fx=0xF, arg=6)
    m.put(p, r + 3, 2, fx=0xF, arg=125)
    at(o, r + 3, 0, "row frames", 6 * 960, "speed and tempo back")

    o, p, r = rows(3)
    m.put(p, r, 0, "C-2", SINE)
    m.put(p, r, 1, fx=0xE, arg=0x60)
    m.put(p, r + 2, 1, fx=0xE, arg=0x62)
    expect.append({"name": "E6x: three rows played three times", "what": "sequence", "order": o,
                   "value": [r, r + 1, r + 2] * 3 + [r + 3]})

    # a break to row 10 of the next pattern, then a jump over one pattern
    m.put(state["pat"], state["row"], 0, fx=0xD, arg=0x10)
    last_row = state["row"]
    skipped = m.pattern()
    final = m.pattern()
    target = m.pattern()
    m.orders += [target, skipped, final]
    o_target = len(m.orders) - 3
    m.put(target, 10, 0, "C-2", SINE)
    m.put(target, 11, 0, fx=0xB, arg=o_target + 2)
    m.put(skipped, 0, 0, "C-3", SINE)
    m.put(final, 0, 0, "E-2", SINE)
    m.put(final, 1, 0, fx=0xC, arg=0)
    m.put(final, 3, 0, fx=0xB, arg=0)       # back to the start: the song's end
    expect.append({"name": "Dxx: on to row 10 of the next pattern", "what": "follows",
                   "value": [[o_target - 1, last_row], [o_target, 10]]})
    expect.append({"name": "Bxx: jumps to a position", "what": "follows",
                   "value": [[o_target, 11], [o_target + 2, 0]]})
    expect.append({"name": "the song ends where it jumps back", "what": "last", "value": [o_target + 2, 3]})

    off = sample_offsets(m)
    at(o9, r9, 1, "start pos", off[LONG] + 0x400, "9xx: starts further in")
    at(o9, r9, 1, "vol", vol16(48), "9xx: with the sample's own volume")
    at(o9, r9 + 1, 1, "start pos", off[LONG] + 0x400, "900: the same offset again")
    at(oh, rh, 1, "running", 1, "a sample without a loop plays")
    at(oh, rh, 1, "end", off[HIT] + 2000, "its end is the sample's")
    at(oh, rh + 4, 1, "running", 0, "and stops when it is over")
    at(oa, ra, 1, "start pos", off[ATT], "a loop after an attack: starts at the beginning")
    at(oa, ra, 1, "start", off[ATT] + 1000, "its loop begins after the attack")
    at(oa, ra, 1, "end", off[ATT] + 1032, "and ends with the sample")
    at(oa, ra + 3, 1, "running", 1, "it goes on in the loop")
    at(oa, ra + 3, 1, "in loop", [off[ATT] + 1000, off[ATT] + 1032], "its position is in the loop")
    return m, expect


def make_test6():
    """What is different in a module that is not ProTracker's: six channels here"""
    m = Mod("modplay test, 6 channels", channels=6)
    SINE = m.sample("sine, looped", s8(100 * math.sin(2 * math.pi * i / 32) for i in range(32)), loop=(0, 32))
    p = m.pattern()
    m.orders.append(p)
    expect = []

    def at(row, tick, what, value, name, voice=0):
        expect.append({"name": name, "order": 0, "row": row, "tick": tick, "voice": voice,
                       "what": what, "value": value})

    for ch, pan in enumerate((64, 192, 192, 64, 64, 192)):
        m.put(p, 0, ch, "C-2", SINE, 0xC, 0x10)
        at(0, 1, "pan", pan, "six channels left and right", voice=ch)
        at(0, 1, "freq", freq(428), "all six play", voice=ch)
    at(0, 1, "master", 21, "the master volume leaves room for six")
    m.put(p, 2, 0, "C-2", SINE, 0x8, 0x20)
    at(2, 1, "pan", 0x20, "8xx: pan")
    m.put(p, 3, 0, fx=0xE, arg=0x8F)
    at(3, 1, "pan", 255, "E8x: pan")
    m.put(p, 5, 5, "B-3", SINE, 0x1, 0x10)
    at(5, 5, "freq", freq(113 - 80), "1xx goes on past period 113", voice=5)
    m.put(p, 6, 5, fx=0xC, arg=0)
    m.put(p, 7, 0, fx=0xB, arg=0)
    expect.append({"name": "the song ends where it jumps back", "what": "last", "value": [0, 7]})
    return m, expect


def make_test_vblank():
    """Played with VBlank timing, every F command is a speed"""
    m = Mod("modplay test, vblank")
    SINE = m.sample("sine, looped", s8(100 * math.sin(2 * math.pi * i / 32) for i in range(32)), loop=(0, 32))
    p = m.pattern()
    m.orders.append(p)
    expect = []
    m.put(p, 0, 0, "C-2", SINE)
    m.put(p, 1, 0, fx=0xF, arg=0x40)
    m.put(p, 2, 0, fx=0xF, arg=6)
    m.put(p, 3, 0, fx=0xC, arg=0)
    m.put(p, 4, 0, fx=0xB, arg=0)
    for row, ticks, name in ((0, 6, "six ticks a row to begin with"), (1, 64, "F40: 64 ticks a row, not a tempo"),
                             (2, 6, "F06: six again"), (3, 6, "and the tempo was never changed")):
        expect.append({"name": name, "order": 0, "row": row, "tick": 0, "voice": 0, "what": "row frames",
                       "value": ticks * 960})
    return m, expect


# -----------------------------------------------------------------------------------------------
# Something to listen to
# -----------------------------------------------------------------------------------------------

def make_test_switch():
    """The timing changed over before the third row, as the V key does: it is from then on
    what it would have been from the start. Returns the module and what to expect both ways."""
    m = Mod("modplay test, v key")
    SINE = m.sample("sine, looped", s8(100 * math.sin(2 * math.pi * i / 32) for i in range(32)), loop=(0, 32))
    p = m.pattern()
    m.orders.append(p)
    m.put(p, 0, 0, "C-2", SINE)
    m.put(p, 0, 1, fx=0xF, arg=0x40)   # a tempo of 64, or 64 ticks a row
    m.put(p, 3, 0, fx=0xF, arg=6)
    m.put(p, 4, 0, fx=0xC, arg=0)
    m.put(p, 5, 0, fx=0xB, arg=0)
    CIA, VBL = 6 * 1875, 64 * 960      # a row at tempo 64 and speed 6; at 50 Hz and speed 64

    def rows(frames, names):
        return [{"name": n, "order": 0, "row": r, "tick": 0, "voice": 0, "what": "row frames", "value": f}
                for r, (f, n) in enumerate(zip(frames, names))]

    on = rows([CIA, CIA, VBL, 6 * 960, 6 * 960],
              ["F40 as a tempo: 64", "and the next row", "V pressed: F40 was a speed, 64 ticks at 50 Hz",
               "F06 in VBlank timing", "and the next row"])
    off = rows([VBL, VBL, CIA, CIA, CIA],
               ["F40 as a speed: 64 ticks", "and the next row", "V pressed: F40 was a tempo, speed is 6 again",
                "F06 changes nothing then", "and the next row"])
    return m, on, off


def make_demo():
    rnd = random.Random(65)
    m = Mod("megagus says hello")
    n = 64
    bass = m.sample("bass", s8(90 * (2 * i / n - 1) * 0.6 + 50 * math.sin(2 * math.pi * i / n) for i in range(n)),
                    volume=56, loop=(0, n))
    lead = m.sample("lead", s8(70 * math.sin(2 * math.pi * i / 32) + 25 * math.sin(4 * math.pi * i / 32)
                               for i in range(32)), volume=40, loop=(0, 32))
    pad = m.sample("pad", s8(60 * math.sin(2 * math.pi * i / 64) + 30 * math.sin(6 * math.pi * i / 64)
                             for i in range(64)), volume=30, loop=(0, 64))
    kick = m.sample("kick", s8(120 * math.sin(40 * (1 - math.exp(-i / 300))) * math.exp(-i / 700)
                               for i in range(2400)), volume=64)
    snare = m.sample("snare", s8((rnd.uniform(-1, 1) * 90 + 30 * math.sin(i / 6)) * math.exp(-i / 500)
                                 for i in range(2000)), volume=52)
    hat = m.sample("hat", s8(rnd.uniform(-1, 1) * 70 * math.exp(-i / 120) for i in range(600)), volume=30)

    chords = [("A-1", "A-2", "C-3", "E-3", 0x37), ("F-1", "F-2", "A-2", "C-3", 0x47),
              ("C-1", "C-2", "E-2", "G-2", 0x47), ("G-1", "G-2", "B-2", "D-3", 0x47)]
    tune = ["E-3", None, "A-3", None, "G-3", "E-3", None, "D-3", "C-3", None, "E-3", None, "D-3", "C-3", "A-2", None,
            "C-3", None, "F-3", None, "E-3", "C-3", None, "A-2", "C-3", None, "D-3", None, "E-3", None, None, None,
            "G-3", None, "E-3", None, "C-3", "E-3", None, "G-3", "A-3", None, "G-3", None, "E-3", None, "D-3", None,
            "D-3", None, "G-3", None, "B-3", "A-3", None, "G-3", "D-3", None, "E-3", None, "D-3", None, None, None]

    def drums(pat, fill=False):
        for row in range(0, 64, 2):
            beat = row % 16
            if beat in (0, 6, 10):
                m.put(pat, row, 3, "C-2", kick)
            elif beat in (4, 12):
                m.put(pat, row, 3, "C-2", snare)
            else:
                m.put(pat, row, 3, "C-3", hat)
        if fill:
            for row in range(56, 64):
                m.put(pat, row, 3, "C-2", snare, 0xC, 20 + 5 * (row - 56))

    def backing(pat, lead_on):
        for bar, (low, root, third, fifth, arp) in enumerate(chords):
            base = bar * 16
            for row in range(0, 16, 2):
                m.put(pat, base + row, 0, low if row % 8 < 4 else root, bass)
            m.put(pat, base, 1, root, pad, 0x0, arp)
            for row in range(1, 16):
                m.put(pat, base + row, 1, fx=0x0, arg=arp)
        if lead_on:
            for row, note in enumerate(tune):
                if note:
                    m.put(pat, row, 2, note, lead)
                elif row and tune[row - 1]:
                    m.put(pat, row, 2, fx=0x4, arg=0x46)
                else:
                    m.put(pat, row, 2, fx=0xA, arg=0x03)

    intro = m.pattern()
    backing(intro, False)
    m.put(intro, 0, 2, fx=0xF, arg=6)
    for row in range(32, 64, 4):
        m.put(intro, row, 3, "C-3", hat)
    main = m.pattern()
    backing(main, True)
    drums(main)
    turn = m.pattern()
    backing(turn, True)
    drums(turn, fill=True)
    end = m.pattern()
    m.put(end, 0, 0, "A-1", bass, 0xA, 0x02)
    m.put(end, 0, 1, "A-2", pad, 0x0, 0x37)
    m.put(end, 0, 2, "A-3", lead, 0x4, 0x46)
    m.put(end, 0, 3, "C-2", kick)
    for row in range(1, 16):
        m.put(end, row, 0, fx=0xA, arg=0x02)
        m.put(end, row, 1, fx=0xA, arg=0x01)
        m.put(end, row, 2, fx=0x6, arg=0x01)
    m.put(end, 16, 0, fx=0xB, arg=0)        # and round again
    m.orders = [intro, main, turn, main, turn, end]
    return m


def main():
    work = os.path.join(os.path.dirname(os.path.abspath(__file__)), "work")
    os.makedirs(work, exist_ok=True)
    m, expect = make_test()
    data = m.write(os.path.join(work, "test.mod"))
    with open(os.path.join(work, "test.json"), "w") as f:
        json.dump(expect, f, indent=1)
    print(f"test.mod: {len(data)} bytes, {len(m.patterns)} patterns, {len(expect)} things to check")
    m, expect = make_test6()
    data = m.write(os.path.join(work, "test6.mod"))
    with open(os.path.join(work, "test6.json"), "w") as f:
        json.dump(expect, f, indent=1)
    print(f"test6.mod: {len(data)} bytes, {len(expect)} things to check")
    m, expect = make_test_vblank()
    data = m.write(os.path.join(work, "test_vblank.mod"))
    with open(os.path.join(work, "test_vblank.json"), "w") as f:
        json.dump(expect, f, indent=1)
    print(f"test_vblank.mod: {len(data)} bytes, {len(expect)} things to check")
    m, on, off = make_test_switch()
    data = m.write(os.path.join(work, "test_switch.mod"))
    for name, expect in (("on", on), ("off", off)):
        with open(os.path.join(work, f"test_switch_{name}.json"), "w") as f:
            json.dump(expect, f, indent=1)
    print(f"test_switch.mod: {len(data)} bytes, {len(on)} + {len(off)} things to check")
    d = make_demo()
    data = d.write(os.path.join(work, "demo.mod"))
    print(f"demo.mod: {len(data)} bytes, {len(d.patterns)} patterns")


if __name__ == "__main__":
    main()
