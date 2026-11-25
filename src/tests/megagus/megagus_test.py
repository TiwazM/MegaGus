#!/usr/bin/env python3
"""
MegaGus: a bit-exact model of src/vhdl/megagus.vhdl, and the test that holds the VHDL to it.

  ./megagus_test.py          build a script of register and sample RAM accesses, run it through
                             the VHDL in nvc (src/vhdl/test_megagus.vhdl), replay it through
                             the model with every access at the clock edge the VHDL took it,
                             and compare every output sample and every byte read

The model is also the reference for an emulator's MegaGus: MegaGus.frame() is one output
sample, MegaGus.write()/read() are the bus.
"""

import os
import random
import subprocess
import sys

NVOICES = 32

# register offsets within a voice
CTRL, PAN, VOL, FREQ, POS, START, END = 0, 1, 2, 4, 7, 10, 13
# CTRL bits
RUN, LOOP, BIDI, BIT16, BACK, UNSIGNED, ENDED = 1, 2, 4, 8, 16, 32, 128
# globals
G_ID, G_VERSION, G_RAMSIZE, G_CONTROL, G_MASTER, G_RAMP, G_VOICES = (
    0x200, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207)
G_ACTIVE, G_ENDED, G_RATE, G_FRAMES = 0x208, 0x20C, 0x210, 0x214


def sat16(x):
    return max(-32768, min(32767, x))


def s8(b):
    return b - 256 if b & 0x80 else b


def s16(w):
    return w - 65536 if w & 0x8000 else w


class MegaGus:
    def __init__(self, ram_bits=19, rate=48000):
        self.ram_bits = ram_bits
        self.rate = rate
        self.ram = bytearray(1 << ram_bits)
        self.par = [bytearray(16) for _ in range(NVOICES)]
        self.pos = [0] * NVOICES            # byte address << 16 | fraction
        self.vol = [0] * NVOICES
        self.hold = [0] * NVOICES           # the last value each voice put out while it ran
        self.pos_snap = 0
        self.frames_snap = 0
        self.reset()

    def reset(self):
        self.running = [0] * NVOICES
        self.dirv = [0] * NVOICES
        self.live = [0] * NVOICES
        self.endflag = [0] * NVOICES
        self.run_req = [0] * NVOICES
        self.dir_req = [0] * NVOICES
        self.ctrl_load = [0] * NVOICES
        self.pos_load = [0] * NVOICES
        self.enable = 1
        self.master = 0x20
        self.ramp = 0x40
        self.frames = 0                     # played since the reset, or since $214 was written
        self.out = (0, 0)

    # ---------------------------------------------------------------------------------------
    # The bus
    # ---------------------------------------------------------------------------------------
    def _active(self, v):
        return self.run_req[v] if self.ctrl_load[v] else self.running[v]

    def _curdir(self, v):
        return self.dir_req[v] if self.ctrl_load[v] else self.dirv[v]

    def _bitmap(self, bits, n):
        return sum(bits(v) << (v - n * 8) for v in range(n * 8, n * 8 + 8))

    def read(self, ram, addr):
        if ram:
            return self.ram[addr] if addr < len(self.ram) else 0
        addr &= 0x3FF
        if addr < 0x200:
            v, r = addr >> 4, addr & 15
            p = self.par[v]
            cpos = (p[7] | p[8] << 8 | p[9] << 16) if self.pos_load[v] else self.pos[v] >> 16
            if r == 0:
                return (p[0] & 0x6E) | self._active(v) | self._curdir(v) << 4 | self.endflag[v] << 7
            if r == 7:
                self.pos_snap = cpos >> 8
                return cpos & 0xFF
            if r == 8:
                return self.pos_snap & 0xFF
            if r == 9:
                return self.pos_snap >> 8
            return p[r]
        if addr >= 0x220:
            return 0
        g = addr & 0x1F
        if g == 0:
            return 0x4D
        if g == 1:
            return 0x47
        if g == 2:
            return 3                        # 3: a stopped voice fades its last value
        if g == 3:
            return (1 << self.ram_bits) // 65536
        if g == 4:
            return self.enable
        if g == 5:
            return self.master
        if g == 6:
            return self.ramp
        if g == 7:
            return NVOICES
        if 8 <= g <= 11:
            return self._bitmap(self._active, g - 8)
        if 12 <= g <= 15:
            return self._bitmap(lambda v: self.endflag[v], g - 12)
        if g == 16:
            return self.rate & 0xFF
        if g == 17:
            return self.rate >> 8
        if g == 20:                         # the low byte holds the other three
            self.frames_snap = self.frames >> 8
            return self.frames & 0xFF
        if 21 <= g <= 23:
            return (self.frames_snap >> (8 * (g - 21))) & 0xFF
        return 0

    def write(self, ram, addr, data):
        if ram:
            if addr < len(self.ram):
                self.ram[addr] = data
            return
        addr &= 0x3FF
        if addr < 0x200:
            v, r = addr >> 4, addr & 15
            self.par[v][r] = data
            if r == 0:
                self.run_req[v] = data & 1
                self.dir_req[v] = (data >> 4) & 1
                self.ctrl_load[v] = 1
                self.endflag[v] = 0
            if r == 9:
                self.pos_load[v] = 1
            return
        if addr >= 0x220:
            return
        g = addr & 0x1F
        if g == 4:
            self.enable = data & 1
            if data & 0x80:             # as the reset line
                self.reset()
        elif g == 5:
            self.master = data
        elif g == 6:
            self.ramp = data
        elif g == 20:
            self.frames = 0
        elif 12 <= g <= 15:
            for i in range(8):
                if data >> i & 1:
                    self.endflag[(g - 12) * 8 + i] = 0

    # ---------------------------------------------------------------------------------------
    # One frame: one output sample
    # ---------------------------------------------------------------------------------------
    def _byte(self, a):
        return self.ram[a & (len(self.ram) - 1)]

    def frame(self):
        for _ in self.frame_steps():
            pass
        return self.out

    def frame_steps(self):
        """One frame, as the VHDL spreads it over the clock: yields the clock edge (counted
        from the frame's tick) of each point where the engine looks at or changes anything the
        bus can also reach, before it does. An access at an earlier edge has to be applied
        first; at the same edge a read comes first and a write after."""
        yield 0
        if not self.enable:
            self.out = (0, 0)
            return
        self.frames = (self.frames + 1) & 0xFFFFFFFF
        accl = accr = 0
        edge = 1
        for v in range(NVOICES):
            yield edge
            p = self.par[v]
            ctrl, pan = p[0], p[1]
            volume = p[2] | p[3] << 8
            freq = p[4] | p[5] << 8 | p[6] << 16
            start = p[10] | p[11] << 8 | p[12] << 16
            end = p[13] | p[14] << 8 | p[15] << 16
            loop, bidi = ctrl & LOOP, ctrl & BIDI
            sh = 1 if ctrl & BIT16 else 0
            flip = 0x80 if ctrl & UNSIGNED else 0

            if self.ctrl_load[v]:
                run, back = self.run_req[v], self.dir_req[v]
            else:
                run, back = self.running[v], self.dirv[v]
            if self.pos_load[v]:
                posi, frac = p[7] | p[8] << 8 | p[9] << 16, 0
            else:
                posi, frac = self.pos[v] >> 16, self.pos[v] & 0xFFFF
            cv = self.vol[v] if self.live[v] else 0
            self.ctrl_load[v] = 0
            self.pos_load[v] = 0

            if not run and not self.live[v]:
                # silent: one clock
                self.pos[v] = posi << 16 | frac
                self.vol[v] = 0
                self.running[v] = 0
                self.dirv[v] = back
                edge += 1
                continue

            sp, st, en = posi >> sh, start >> sh, end >> sh
            i1 = sp + 1
            if i1 >= en:
                i1 = st if (loop and not bidi) else sp

            # the volume's ramp
            yield edge + 1
            tgt = volume if run else 0
            step = self.ramp << 4
            if self.ramp == 0:
                ncv = tgt
            elif tgt - cv > step:
                ncv = cv + step
            elif tgt - cv < -step:
                ncv = cv - step
            else:
                ncv = tgt

            # the two samples. Once the voice has stopped, what fades is the last value it put
            # out while it ran: nothing the CPU writes to it or to its sample then (another
            # kind of sample in CTRL, the next note's START and END) can reach the output
            # before RUN is set again
            if run:
                if sh:
                    s0 = s16(self._byte(sp * 2) | (self._byte(sp * 2 + 1) ^ flip) << 8)
                    s1 = s16(self._byte(i1 * 2) | (self._byte(i1 * 2 + 1) ^ flip) << 8)
                else:
                    s0 = s8(self._byte(sp) ^ flip) << 8
                    s1 = s8(self._byte(i1) ^ flip) << 8
                interp = s0 + (((s1 - s0) * frac) >> 16)
                self.hold[v] = interp
            else:
                interp = self.hold[v]
            vv = (interp * cv) >> 16
            accl += (vv * (255 - pan)) >> 8
            accr += (vv * pan) >> 8

            # the position
            P, ST, EN = sp << 16 | frac, st << 16, en << 16
            n, run_n, back_n, wrapped, ended = P, run, back, False, False
            if run:
                if not back:
                    n = P + freq
                    if n >= EN:
                        if loop and not bidi:
                            n, wrapped = n - (EN - ST), True
                        elif loop:
                            n, back_n, wrapped = 2 * EN - 1 - n, 1, True
                        else:
                            n, run_n, ended = EN - 65536, 0, True
                else:
                    n = P - freq
                    if n < ST:
                        if loop and bidi:
                            n, back_n, wrapped = 2 * ST - n, 0, True
                        elif loop:
                            n, wrapped = n + (EN - ST), True
                        else:
                            n, run_n, ended = ST, 0, True
                if wrapped and (n < ST or n >= EN):
                    n = ST

            # the voice's state for the next frame
            yield edge + 16
            if sh:
                self.pos[v] = ((n >> 16) & 0x7FFFFF) << 17 | (n & 0xFFFF)
            else:
                self.pos[v] = n & 0xFFFFFFFFFF
            self.vol[v] = ncv
            self.running[v] = run_n
            self.dirv[v] = back_n
            self.live[v] = 1 if (ncv or run_n) else 0
            if ended and not self.ctrl_load[v]:
                self.endflag[v] = 1
            edge += 17

        yield edge
        self.out = (sat16((accl * self.master) >> 6), sat16((accr * self.master) >> 6))


# ---------------------------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------------------------
class Script:
    """A list of bus accesses and waits for the testbench."""

    def __init__(self):
        self.lines = []
        self.ops = []           # the accesses, in order: (kind, ram, addr, data)

    def w(self, addr, data, ram=0):
        self.lines.append(f"W {ram} {addr} {data}")
        self.ops.append(("W", ram, addr, data))

    def r(self, addr, ram=0):
        self.lines.append(f"R {ram} {addr}")
        self.ops.append(("R", ram, addr, 0))

    def frames(self, n):
        if n:
            self.lines.append(f"F {n}")

    def clocks(self, n):
        if n:
            self.lines.append(f"J {n}")

    def reset(self):
        self.lines.append("X")
        self.ops.append(("X", 0, 0, 0))

    # voice registers
    def reg(self, v, r, value, size=1):
        for i in range(size):
            self.w(v * 16 + r + i, (value >> (8 * i)) & 0xFF)

    def voice(self, v, ctrl, pos, start, end, freq, vol, pan):
        self.reg(v, CTRL, 0)
        self.reg(v, PAN, pan)
        self.reg(v, VOL, vol, 2)
        self.reg(v, FREQ, freq, 3)
        self.reg(v, START, start, 3)
        self.reg(v, END, end, 3)
        self.reg(v, POS, pos, 3)
        self.reg(v, CTRL, ctrl)

    def count(self):
        for a in range(G_FRAMES, G_FRAMES + 4):
            self.r(a)

    def status(self, v=None):
        for a in range(G_ACTIVE, G_ACTIVE + 8):
            self.r(a)
        if v is not None:
            self.r(v * 16 + CTRL)
            for i in range(3):
                self.r(v * 16 + POS + i)


def replay(model, ops, log):
    """The model's output for the testbench's log: every access applied at the clock edge
    the VHDL took it. Returns (frames compared, reads compared, the differences)."""
    ticks = [int(l.split()[1]) for l in log if l[0] == "T"]
    outs = [tuple(map(int, l.split()[1:])) for l in log if l[0] == "S"]
    acc = [tuple(map(int, l.split()[1:])) for l in log if l[0] == "A"]
    assert len(acc) == len(ops), f"{len(acc)} accesses in the log, {len(ops)} in the script"
    assert len(ticks) - len(outs) in (0, 1)
    bad = []
    nxt = 0

    def apply_before(edge, engine_acts):
        # accesses before this edge; at it, reads (the engine's action comes after them)
        nonlocal nxt
        while nxt < len(ops):
            kind, ram, addr, data = ops[nxt]
            at, got = acc[nxt]
            if at > edge or (at == edge and engine_acts and kind != "R"):
                break
            if kind == "R":
                want = model.read(ram, addr)
                if want != got:
                    bad.append(f"access {nxt} (edge {at}): read {ram}/{addr:#x}: "
                               f"VHDL {got:#04x}, model {want:#04x}")
            elif kind == "W":
                model.write(ram, addr, data)
            else:
                model.reset()
            nxt += 1

    for k, got in enumerate(outs):
        for step_edge in model.frame_steps():
            apply_before(ticks[k] + step_edge, True)
        if model.out != got:
            bad.append(f"frame {k}: VHDL {got}, model {model.out}")
        # the rest of the frame: nothing more from the engine
        last = ticks[k + 1] - 1 if k + 1 < len(ticks) else 1 << 62
        apply_before(last, False)
    return len(outs), sum(1 for o in ops[:nxt] if o[0] == "R"), bad


def build(ram_bits, seed):
    rnd = random.Random(seed)
    s = Script()
    size = 1 << ram_bits
    f = lambda x: int(x * 65536)        # samples a frame as FREQ

    # the globals as they come up, then a sample RAM full of noise (every bit pattern)
    for a in range(0x200, 0x218):
        s.r(a)
    for a in range(size):
        s.w(a, rnd.randrange(256), ram=1)
    for a in (0, 1, 0x7FF, size - 1, size, size + 5):
        s.r(a, ram=1)
    s.w(size + 5, 0x55, ram=1)          # beyond the RAM: nothing there
    s.r(5, ram=1)
    s.frames(3)

    # one voice, 8 bits, once through: its end, the ended flag, the fade of its last sample
    s.voice(0, RUN, 0x010, 0x010, 0x050, f(0.37), 0xFFFF, 0x40)
    s.frames(60)
    s.status(0)
    s.frames(140)
    s.status(0)
    s.w(G_ENDED, 0x01)
    s.status(0)
    s.frames(40)

    # the loops: forward (entered from before its start), ping-pong in 16 bits, backward in
    # unsigned 16 bits, and a loop of two samples stepped over by more than its length
    s.voice(1, RUN | LOOP, 0x100, 0x120, 0x163, f(1.7), 0xC000, 0x00)
    s.voice(5, RUN | LOOP | BIDI | BIT16, 0x400, 0x410, 0x4D0, f(0.83), 0xA000, 0xFF)
    s.voice(31, RUN | LOOP | BACK | BIT16 | UNSIGNED, 0x6F0, 0x600, 0x700, f(2.31), 0x8000, 0x80)
    s.voice(7, RUN | LOOP | BIDI | UNSIGNED, 0x800, 0x800, 0x802, f(3.3), 0x6000, 0x30)
    s.frames(300)
    s.status(5)
    s.status(31)

    # changes while they play: volume, pan, pitch, a stop that fades, a start again
    s.reg(1, VOL, 0x2000, 2)
    s.reg(5, PAN, 0x10)
    s.reg(31, FREQ, f(0.11), 3)
    s.frames(100)
    s.reg(1, CTRL, LOOP)
    s.frames(10)
    s.status(1)
    s.frames(90)
    s.reg(1, CTRL, RUN | LOOP)          # on from where it stopped
    s.reg(5, POS, 0x480, 3)             # a jump inside the loop
    s.frames(100)

    # a stopped voice fades the last value it put out, whatever is written to it meanwhile:
    # CTRL without its 16-BIT and UNSIGNED, another position, other ends, other samples
    s.w(G_RAMP, 0x04)                   # a slow fade, to see it
    s.voice(9, RUN | LOOP | BIT16 | UNSIGNED, 0x300, 0x300, 0x3C0, f(0.7), 0xFFFF, 0x60)
    s.frames(120)
    s.reg(9, CTRL, 0)
    s.frames(7)
    s.reg(9, POS, 0x1234, 3)
    s.frames(7)
    s.reg(9, START, 0x1000, 3)
    s.reg(9, END, 0x1100, 3)
    s.frames(7)
    for a in range(0x300, 0x3C0):
        s.w(a, a & 0xFF, ram=1)
    s.reg(9, CTRL, BIT16)
    s.frames(7)
    s.reg(9, CTRL, RUN | LOOP)          # and on, as 8 bits, from the new position
    s.frames(60)
    s.reg(9, CTRL, LOOP | UNSIGNED)
    s.frames(300)                       # all the way down
    s.status(9)
    s.w(G_RAMP, 0x40)

    # the ramp: instant, slowest, fastest
    for ramp in (0x00, 0x01, 0xFF):
        s.w(G_RAMP, ramp)
        s.reg(7, VOL, 0xFFFF, 2)
        s.frames(40)
        s.reg(7, VOL, 0x0100, 2)
        s.frames(40)
    s.w(G_RAMP, 0x40)

    # the master volume, and stopping the engine: voices hold, output silent
    for master in (0x00, 0x01, 0x40, 0xFF, 0x20):
        s.w(G_MASTER, master)
        s.frames(30)
    s.w(G_CONTROL, 0)
    s.count()                           # the frame counter stands still with the engine
    s.frames(20)
    s.status(5)
    s.count()
    s.w(G_CONTROL, 1)
    s.frames(50)
    s.count()

    # the frame counter: its upper bytes are held from the read of the lowest; a write
    # starts it from 0
    s.r(G_FRAMES)
    s.frames(300)
    for a in (G_FRAMES + 1, G_FRAMES + 2, G_FRAMES + 3):
        s.r(a)
    s.count()
    s.w(G_FRAMES, 0xA5)
    s.count()
    for n in (1, 2, 255, 1, 300):
        s.frames(n)
        s.count()

    # backward once through, in 8 bits; a voice whose END is below its START; 16 bits on
    # odd addresses
    s.voice(2, RUN | BACK, 0x9FF, 0x9C0, 0xA00, f(0.61), 0xFFFF, 0x80)
    s.voice(3, RUN | LOOP, 0xA10, 0xA40, 0xA20, f(0.5), 0xFFFF, 0x80)
    s.voice(4, RUN | BIT16, 0xB01, 0xB00, 0xB41, f(0.25), 0xFFFF, 0x80)
    s.frames(200)
    s.status(2)
    s.status(3)
    s.status(4)

    # every voice at once, loud: the sum clips
    s.w(G_MASTER, 0xFF)
    for v in range(NVOICES):
        mode = RUN | LOOP | (BIDI if v & 1 else 0) | (BIT16 if v & 2 else 0)
        base = (v * 0x70) % (size - 0x100) & ~1
        s.voice(v, mode, base, base, base + 0x60, f(0.2 + v * 0.07), 0xFFFF, (v * 37) & 0xFF)
    s.frames(400)
    s.status(17)

    # reset: silence at once, the registers stay
    s.reset()
    s.frames(5)
    s.status(17)
    s.r(17 * 16 + VOL)
    s.reg(17, CTRL, RUN | LOOP)
    s.frames(50)
    s.reg(17, CTRL, 0)
    s.frames(100)

    # the same from software: bit 7 of the control register
    for v in (3, 9, 30):
        s.voice(v, RUN | LOOP, 0x200, 0x200, 0x280, f(0.9), 0xFFFF, 0x80)
    s.w(G_MASTER, 0x55)
    s.frames(30)
    s.w(G_CONTROL, 0x80)
    s.frames(3)
    s.status(9)
    for a in (G_CONTROL, G_MASTER, G_RAMP):
        s.r(a)
    s.reg(9, CTRL, RUN | LOOP)
    s.frames(30)

    # and whatever comes: random accesses to random voices at random clocks, in the middle
    # of the engine's work on the same voice too; mostly sensible values, some not. Quiet
    # enough that the sum rarely clips (a clipped sample would hide a difference).
    s.w(G_MASTER, 0x06)
    for _ in range(9000):
        for _ in range(rnd.choice((0, 0, 1, 1, 2, 3, 6))):
            s.clocks(rnd.choice((0, 0, 1, 2, 3, 5, 17, 40, 150)))
            v = rnd.randrange(NVOICES)
            what = rnd.randrange(14)
            wild = rnd.random() < 0.05
            addr = lambda: rnd.randrange(1 << 24) if wild else rnd.randrange(size)
            if what == 0:
                a, b = sorted((addr(), addr()))
                pos = rnd.randrange(a, b + 1) if not wild else addr()
                s.voice(v, rnd.randrange(64), pos, a, b,
                        rnd.randrange(1 << (24 if wild else 18)),
                        rnd.randrange(65536), rnd.randrange(256))
            elif what == 1:
                s.reg(v, CTRL, rnd.randrange(256))
            elif what == 2:
                s.reg(v, VOL, rnd.randrange(65536), 2)
            elif what == 3:
                s.reg(v, PAN, rnd.randrange(256))
            elif what == 4:
                s.reg(v, FREQ, rnd.randrange(1 << (24 if wild else 18)), 3)
            elif what == 5:
                s.reg(v, POS, addr(), 3)
            elif what == 6:
                s.reg(v, rnd.choice((START, END)), addr(), 3)
            elif what == 7:
                s.w(v * 16 + rnd.randrange(16), rnd.randrange(256))     # any one byte
            elif what == 8:
                s.w(G_RAMP, rnd.randrange(256))
            elif what == 9:
                s.w(G_MASTER, rnd.randrange(12))
            elif what == 10:
                s.w(G_ENDED + rnd.randrange(4), rnd.randrange(256))
            elif what == 11:
                s.status(v)
            elif what == 12:
                s.r(v * 16 + rnd.randrange(16))
            else:
                s.r(0x200 + rnd.randrange(0x14))
        if rnd.random() < 0.6:
            s.clocks(rnd.randrange(900))
        else:
            s.frames(rnd.choice((1, 1, 2, 3, 7, 25)))
    return s


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    vhdl = os.path.join(here, "..", "..", "vhdl")
    work = os.path.join(here, "work")
    os.makedirs(work, exist_ok=True)

    ram_bits = 12
    s = build(ram_bits, seed=65)
    with open(os.path.join(work, "megagus_stim.txt"), "w") as fh:
        fh.write("\n".join(s.lines) + "\n")

    nvc = ["nvc", "--std=1993", "--work=work"]
    for cmd in (nvc + ["-a", os.path.join(vhdl, "megagus.vhdl"), os.path.join(vhdl, "test_megagus.vhdl")],
                nvc + ["-e", "test_megagus"],
                nvc + ["-r", "test_megagus"]):
        subprocess.run(cmd, cwd=work, check=True)

    with open(os.path.join(work, "megagus_out.txt")) as fh:
        log = fh.read().split("\n")[:-1]
    outs = [l for l in log if l[0] == "S"]
    loud = sum(1 for l in outs if l != "S 0 0")
    clipped = sum(1 for l in outs if "32767" in l or "-32768" in l)
    frames, reads, bad = replay(MegaGus(ram_bits), s.ops, log)
    print(f"{len(s.ops)} accesses, {frames} frames ({loud} not silent, {clipped} clipped), "
          f"{reads} reads")
    if bad:
        print(f"FAIL: the VHDL and the model differ in {len(bad)} places")
        for b in bad[:12]:
            print("  " + b)
        sys.exit(1)
    print("PASS: the VHDL and the model agree on every frame and every read")

    # The engine where the core has it: in slow_devices.vhdl, reached as the CPU reaches it
    # (src/vhdl/test_megagus_bus.vhdl checks itself)
    nvc = ["nvc", "--std=2008", "--work=bus"]
    core = ["cputypes.vhdl", "debugtools.vhdl", "fake_opl2.vhdl", "expansion_port_controller.vhdl",
            "megagus.vhdl", "slow_devices.vhdl", "test_megagus_bus.vhdl"]
    quiet = dict(cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for cmd in (nvc + ["-a"] + [os.path.join(vhdl, c) for c in core],
                nvc + ["-e", "test_megagus_bus"],
                nvc + ["-r", "--exit-severity=failure", "test_megagus_bus"]):
        r = subprocess.run(cmd, **quiet)
        said = [l for l in r.stdout.split("\n") if "MegaGus" in l or "Error" in l or "Failure" in l
                or "Fatal" in l]
        if r.returncode:
            print("FAIL: the slow device bus test\n  " + "\n  ".join(said[-12:] or r.stdout.split("\n")[-12:]))
            sys.exit(1)
    verdict = [l for l in said if "MegaGus on the slow device bus: PASS" in l]
    if not verdict:
        print("FAIL: the slow device bus test didn't finish")
        sys.exit(1)
    print("PASS: on the slow device bus " + verdict[-1].split(": PASS ")[-1])


if __name__ == "__main__":
    main()
