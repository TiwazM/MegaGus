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
# the queue: the frame its writes are for (3 bytes), the register the next one is for (2),
# the write itself, the queue's state (bit 7 written: forget it all), and its room
G_QTIME, G_QREG, G_QDATA, G_QSTAT, G_QFREE = 0x218, 0x21B, 0x21D, 0x21E, 0x21F
QEMPTY, QFULL, QLOST, QFLUSH = 1, 2, 0x80, 0x80
QWAIT = 280     # clocks a frame's voices wait for the queued writes that are due: two a write


def sat16(x):
    return max(-32768, min(32767, x))


def s8(b):
    return b - 256 if b & 0x80 else b


def s16(w):
    return w - 65536 if w & 0x8000 else w


class MegaGus:
    def __init__(self, ram_bits=19, rate=48000, queue_bits=12):
        self.ram_bits = ram_bits
        self.rate = rate
        self.queue_size = 1 << queue_bits
        self.ram = bytearray(1 << ram_bits)
        self.par = [bytearray(16) for _ in range(NVOICES)]
        self.pos = [0] * NVOICES            # byte address << 16 | fraction
        self.vol = [0] * NVOICES
        self.hold = [0] * NVOICES           # the last value each voice put out while it ran
        self.pos_snap = 0
        self.frames_snap = 0
        self.resets = 0
        self.reset()

    def reset(self):
        self.resets += 1
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
        self.queue = []                     # [frame, register, value]: writes still to be made
        self.q_time = 0
        self.q_reg = 0
        self.q_over = 0

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
            return 5                        # 3: a stopped voice fades its last value; 4: the
                                            # queue; 5: CTRL and POS read back at once
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
        if 24 <= g <= 26:
            return (self.q_time >> (8 * (g - 24))) & 0xFF
        if g == 27:
            return self.q_reg & 0xFF
        if g == 28:
            return self.q_reg >> 8
        if g == 30:
            return ((QEMPTY if not self.queue else 0) | (QFULL if len(self.queue) == self.queue_size else 0)
                    | (QLOST if self.q_over else 0))
        if g == 31:
            return min(255, (self.queue_size - len(self.queue)) >> 4)
        return 0

    # ---------------------------------------------------------------------------------------
    # The queue
    # ---------------------------------------------------------------------------------------
    def queue_due(self):
        """Whether the write at the head of the queue is to be made now: the frame count is
        at its time, or past it by less than half of what 24 bits count"""
        if not self.queue:
            return False
        ahead = (self.queue[0][0] - self.frames) & 0xFFFFFF
        return ahead == 0 or ahead >= 0x800000

    def queue_pop(self):
        """Make the write at the head of the queue"""
        entry = self.queue.pop(0)
        self.write(0, entry[1], entry[2], queued=True)
        return entry

    def write(self, ram, addr, data, queued=False):
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
        if queued and g >= 24:              # the queue's own registers are the CPU's only
            return
        if 24 <= g <= 26:
            shift = 8 * (g - 24)
            self.q_time = (self.q_time & ~(0xFF << shift)) | data << shift
        elif g == 27:
            self.q_reg = (self.q_reg & 0x300) | data
        elif g == 28:
            self.q_reg = (self.q_reg & 0xFF) | (data & 3) << 8
        elif g == 29:
            if len(self.queue) == self.queue_size:
                self.q_over = 1
            else:
                self.queue.append([self.q_time, self.q_reg, data])
            self.q_reg = (self.q_reg + 1) & 0x3FF
        elif g == 30:
            if data & QFLUSH:
                self.queue = []
                self.q_over = 0
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

    def frame_steps(self, go=1, drain=True):
        """One frame, as the VHDL spreads it over the clock: yields the clock edge (counted
        from the frame's tick) of each point where the engine looks at or changes anything the
        bus can also reach, before it does. An access at an earlier edge has to be applied
        first; at the same edge a read comes first and a write after.

        The queued writes that are due are made as the frame begins, before any voice is
        looked at. (The VHDL makes them two clocks apart and lets the voices go, at edge
        `go`, when they are in: after 280 clocks at the latest, so with more than 140 due at
        once the rest are made while the voices are already being worked through. The test
        takes `go` and the edge of each queued write from the VHDL's log, and passes
        drain=False.)"""
        yield 0
        enabled, resets = self.enable, self.resets
        if enabled:
            self.frames = (self.frames + 1) & 0xFFFFFFFF
        else:
            self.out = (0, 0)               # stopped: the voices hold, the output is silent
        if drain:
            while self.queue_due():
                self.queue_pop()
        # A frame the engine was stopped for has no voices; nor has one that a queued write
        # reset the engine in (go is None then: the VHDL never let its voices go)
        if not enabled or go is None or self.resets != resets:
            return
        accl = accr = 0
        edge = go
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
            # what the CPU wrote is the voice's state from here on: a read while the voice is
            # worked on (the 16 clocks that follow) shows it
            self.pos[v] = posi << 16 | frac
            self.vol[v] = cv
            self.running[v] = run
            self.dirv[v] = back

            if not run and not self.live[v]:
                # silent: one clock
                edge += 1
                continue
            self.live[v] = 1 if (cv or run) else 0

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
        self.expect = {}        # for some reads: (mask, the value those bits must have)

    def w(self, addr, data, ram=0):
        self.lines.append(f"W {ram} {addr} {data}")
        self.ops.append(("W", ram, addr, data))

    def r(self, addr, ram=0):
        self.lines.append(f"R {ram} {addr}")
        self.ops.append(("R", ram, addr, 0))

    def rx(self, addr, mask, value):
        """A read whose answer is known: not only must the VHDL and the model agree on it"""
        self.expect[len(self.ops)] = (mask, value)
        self.r(addr)

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

    # the queue: the frame the writes that follow are for, the register the next one is
    # for, and writes to that register and the ones after it
    def qtime(self, frame):
        for i in range(3):
            self.w(G_QTIME + i, (frame >> (8 * i)) & 0xFF)

    def qreg(self, addr):
        self.w(G_QREG, addr & 0xFF)
        self.w(G_QREG + 1, addr >> 8)

    def qpush(self, *values):
        for value in values:
            self.w(G_QDATA, value)

    def qvoice(self, frame, v, ctrl, pos, start, end, freq, vol, pan):
        """A note as a player starts one, all of it at one frame: the voice stopped, its
        registers, and CTRL last"""
        le = lambda x, n: [(x >> (8 * i)) & 0xFF for i in range(n)]
        self.qtime(frame)
        self.qreg(v * 16)
        self.qpush(ctrl & ~RUN, pan, *le(vol, 2), *le(freq, 3), *le(pos, 3), *le(start, 3), *le(end, 3))
        self.qreg(v * 16)
        self.qpush(ctrl)

    def from_zero(self):
        """Count the frames from here: the next one is frame 1"""
        self.frames(1)
        self.w(G_FRAMES, 0)

    def status(self, v=None):
        for a in range(G_ACTIVE, G_ACTIVE + 8):
            self.r(a)
        if v is not None:
            self.r(v * 16 + CTRL)
            for i in range(3):
                self.r(v * 16 + POS + i)


def replay(model, ops, log, expect=None):
    """The model's output for the testbench's log: every access, and every queued write the
    VHDL made, applied at the clock edge the VHDL took it. Returns (frames compared, reads
    compared, queued writes made, the differences)."""
    ticks = [int(l.split()[1]) for l in log if l[0] == "T"]
    goes = [int(l.split()[1]) for l in log if l[0] == "G"]
    outs = [tuple(map(int, l.split()[1:])) for l in log if l[0] == "S"]
    acc = [tuple(map(int, l.split()[1:])) for l in log if l[0] == "A"]
    assert len(acc) == len(ops), f"{len(acc)} accesses in the log, {len(ops)} in the script"
    assert len(ticks) - len(outs) in (0, 1)
    # what happened on the bus, in the order of its clock edges: the script's accesses and
    # the queue's writes (never two at one edge: there is one bus)
    events = sorted([(at, i) for i, (at, _) in enumerate(acc)]
                    + [(int(l.split()[1]), -1) for l in log if l[0] == "Q"])
    assert all(a[0] != b[0] for a, b in zip(events, events[1:])), "two accesses at one edge"
    assert [i for _, i in events if i >= 0] == list(range(len(ops)))
    bad = []
    nxt = 0
    queued = [0, 0]             # made, and of them made after the frame's voices were let go
    frame_of = {"tick": 0, "go": None, "before": 0, "made": 0}

    def apply_before(edge, engine_acts):
        # accesses before this edge; at it, reads (the engine's action comes after them)
        nonlocal nxt
        while nxt < len(events):
            at, i = events[nxt]
            kind, ram, addr, data = ops[i] if i >= 0 else ("Q", 0, 0, 0)
            if at > edge or (at == edge and engine_acts and kind != "R"):
                break
            if kind == "R":
                want = model.read(ram, addr)
                if want != acc[i][1]:
                    bad.append(f"access {i} (edge {at}): read {ram}/{addr:#x}: "
                               f"VHDL {acc[i][1]:#04x}, model {want:#04x}")
                if expect and i in expect and (want & expect[i][0]) != expect[i][1]:
                    bad.append(f"access {i} (edge {at}): read {ram}/{addr:#x} is {want:#04x}: "
                               f"its bits {expect[i][0]:#04x} should be {expect[i][1]:#04x}")
            elif kind == "W":
                size = len(model.queue)
                model.write(ram, addr, data)
                if len(model.queue) > size:
                    model.queue[-1].append(at)      # when it was queued
            elif kind == "Q":
                if not model.queue:
                    bad.append(f"edge {at}: the VHDL made a queued write, the model's queue is empty")
                else:
                    if not model.queue_due():
                        bad.append(f"edge {at}: the VHDL made a queued write for frame "
                                   f"{model.queue[0][0]} at frame {model.frames}")
                    entry = model.queue_pop()
                    queued[0] += 1
                    frame_of["made"] += 1
                    # In before the voices, if it was there and due when the frame began?
                    go = frame_of["go"]
                    if go is not None and at > go and entry[3] < frame_of["tick"]:
                        queued[1] += 1
                        if frame_of["before"] < 100:
                            bad.append(f"edge {at}: a queued write made after the frame's voices "
                                       f"were let go, with only {frame_of['before']} before them")
                    elif go is None or at <= go:
                        frame_of["before"] += 1
            else:
                model.reset()
            nxt += 1

    g = 0
    for k, got in enumerate(outs):
        end = ticks[k + 1] if k + 1 < len(ticks) else 1 << 62
        while g < len(goes) and goes[g] < ticks[k]:
            g += 1
        go = goes[g] if g < len(goes) and goes[g] < end else None
        frame_of.update(tick=ticks[k], go=go, before=0, made=0)
        if go is not None and go - ticks[k] > QWAIT + 1:
            bad.append(f"frame {k}: the voices waited {go - ticks[k]} clocks")
        for step_edge in model.frame_steps(None if go is None else go - ticks[k] + 1, drain=False):
            apply_before(ticks[k] + step_edge, True)
        # the rest of the frame: nothing more from the engine
        apply_before(end - 1, False)
        if model.out != got:
            bad.append(f"frame {k}: VHDL {got}, model {model.out}")
        # nothing that was due and queued in good time is left over, unless the engine made
        # queued writes all through the frame (two clocks each: some 400 fit in one)
        if model.queue_due() and model.queue[0][3] < end - 8 and frame_of["made"] < 300:
            bad.append(f"frame {k}: a queued write for frame {model.queue[0][0]} not made by the "
                       f"end of frame {model.frames}")
    return len(outs), sum(1 for _, i in events[:nxt] if i >= 0 and ops[i][0] == "R"), queued, bad


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
    queue_tests(s, size, seed)
    readback_tests(s, seed)
    return s


def readback_tests(s, seed):
    """What is written to CTRL and POS reads back at once, at whatever clock of the frame the
    read comes: also in the clocks in which the engine, having taken the write, is working
    on that voice (VERSION 5; before it, a read there showed the voice as it had been)."""
    f = lambda x: int(x * 65536)
    rnd = random.Random(seed + 2)
    s.frames(3)             # (a reset at a frame's beginning, as everywhere in this script)
    s.reset()
    s.frames(2)
    v = 6
    s.voice(v, 0, 0x100, 0x100, 0x180, f(0.5), 0x4000, 0x80)
    for n in range(2500):
        gap = lambda: s.clocks(rnd.choice((0, 0, 0, 1, 2, 3, 5, 9, 14, 30)))
        s.clocks(rnd.randrange(60))
        s.reg(v, CTRL, RUN | LOOP)                  # started: running, at once
        gap()
        s.rx(v * 16 + CTRL, RUN, RUN)
        s.rx(G_ACTIVE, 1 << v, 1 << v)
        s.clocks(rnd.randrange(900))
        s.reg(v, CTRL, LOOP | BACK)                 # stopped, and backward
        gap()
        s.rx(v * 16 + CTRL, RUN | BACK, BACK)
        s.rx(G_ACTIVE, 1 << v, 0)
        s.clocks(rnd.randrange(60))
        pos = 0x100 + (n * 7) % 0x80                # stopped, it stays where it is put
        s.reg(v, POS, pos, 3)
        gap()
        s.rx(v * 16 + POS, 0xFF, pos & 0xFF)
        s.rx(v * 16 + POS + 1, 0xFF, pos >> 8)
        s.clocks(rnd.randrange(900))


def queue_tests(s, size, seed):
    """The queue: writes handed over with the frame each is for"""
    f = lambda x: int(x * 65536)
    s.reset()
    s.frames(2)
    for a in range(0x218, 0x224):
        s.r(a)

    # two notes, a change of volume and a stop, each for its frame; the queue's own registers
    # read back; then frame by frame as they come
    s.from_zero()
    s.qvoice(5, 2, RUN | LOOP, 0x100, 0x120, 0x163, f(1.7), 0xC000, 0x40)
    s.qvoice(9, 3, RUN, 0x010, 0x010, 0x050, f(0.37), 0xFFFF, 0xC0)
    s.qtime(12)
    s.qreg(3 * 16 + VOL)
    s.qpush(0x00, 0x40)
    s.qtime(30)
    s.qreg(2 * 16 + CTRL)
    s.qpush(LOOP)
    for a in (G_QSTAT, G_QFREE, G_QTIME, G_QTIME + 1, G_QTIME + 2, G_QREG, G_QREG + 1, G_QDATA):
        s.r(a)
    for _ in range(12):
        s.frames(1)
        s.status(2)
        s.status(3)
        s.r(G_QSTAT)
        s.count()
    s.frames(40)
    s.r(G_QSTAT)
    s.status(2)

    # for a frame gone by: made at once. And one more than half of 24 bits ahead counts as
    # gone by
    s.qtime(3)
    s.qreg(2 * 16 + VOL)
    s.qpush(0x34, 0x12)
    s.clocks(20)
    s.r(2 * 16 + VOL)
    s.r(2 * 16 + VOL + 1)
    s.r(G_QSTAT)
    s.qtime(0x900000)
    s.qreg(3 * 16 + PAN)
    s.qpush(0x11)
    s.clocks(20)
    s.r(3 * 16 + PAN)
    s.r(G_QSTAT)

    # a tick as full as a tracker's gets: eight notes on one frame, 136 writes, all in before
    # the frame's first voice. Then twelve: more than fit before the voices
    for voices in (8, 12):
        s.from_zero()
        for v in range(voices):
            a = 0x100 + 0x80 * v
            s.qvoice(5, v, RUN | LOOP | (BIT16 if v & 1 else 0), a, a, a + 0x60, f(0.3 + 0.21 * v),
                     0x3000 + 0x800 * v, 20 * v)
        s.r(G_QFREE)
        s.frames(4)
        s.r(G_QSTAT)
        s.frames(1)
        s.r(G_QSTAT)
        s.status(voices - 1)
        s.frames(12)

    # the engine stopped: the frames stand still and what is queued waits; what is for a
    # frame gone by is still made
    s.from_zero()
    s.qtime(5)
    s.qreg(G_MASTER)
    s.qpush(0x10)
    s.frames(2)
    s.w(G_CONTROL, 0)
    s.frames(6)
    s.r(G_MASTER)
    s.r(G_QSTAT)
    s.count()
    s.qtime(1)
    s.qreg(G_RAMP)
    s.qpush(0x20)
    s.clocks(30)
    s.r(G_RAMP)
    s.r(G_QSTAT)
    s.w(G_CONTROL, 1)
    s.frames(6)
    s.r(G_MASTER)
    s.r(G_QSTAT)

    # a queued write stops the engine at a frame; writes queued for the queue's own
    # registers are not made
    s.from_zero()
    s.qtime(3)
    s.qreg(G_QTIME)
    s.qpush(0x55)
    s.qreg(G_QSTAT)
    s.qpush(QFLUSH)
    s.qtime(4)
    s.qreg(G_CONTROL)
    s.qpush(0)
    s.qtime(5)
    s.qreg(G_MASTER)
    s.qpush(0x08)
    s.frames(8)
    for a in (G_CONTROL, G_QSTAT, G_MASTER, G_QTIME):
        s.r(a)
    s.count()
    s.w(G_CONTROL, 1)
    s.frames(4)
    s.r(G_MASTER)
    s.r(G_QSTAT)

    # forgetting what is queued, and queueing again after it
    s.from_zero()
    s.qtime(50)
    s.qreg(2 * 16 + PAN)
    s.qpush(1, 2, 3)
    s.r(G_QSTAT)
    s.w(G_QSTAT, QFLUSH)
    s.r(G_QSTAT)
    s.r(G_QFREE)
    s.frames(3)
    s.from_zero()
    s.qtime(2)
    s.qreg(4 * 16 + PAN)
    s.qpush(0x77)
    s.frames(4)
    s.r(4 * 16 + PAN)

    # full: 4096 writes for a frame far off, three more that are lost, the room as it goes
    s.from_zero()
    s.qtime(100000)
    for i in range(4099):
        if i % 16 == 0:
            s.qreg(6 * 16)
        s.qpush(i & 0xFF)
        if i % 500 == 499 or i > 4090:
            s.r(G_QFREE)
            s.r(G_QSTAT)
    s.frames(2)
    s.w(G_QSTAT, QFLUSH)
    s.r(G_QSTAT)
    s.r(G_QFREE)

    # full again, and all of it due on one frame: it takes the engine some frames
    s.from_zero()
    s.voice(0, RUN | LOOP, 0x100, 0x100, 0x180, f(0.9), 0x4000, 0x80)
    s.qtime(60)
    for i in range(4096):
        if i % 4 == 0:
            s.qreg((16 + i % 16) * 16 + PAN)
        s.qpush((i * 7) & 0xFF)
    s.r(G_QSTAT)
    s.count()
    for _ in range(8):
        s.frames(9)
        s.r(G_QSTAT)
        s.r(G_QFREE)
        s.count()
    s.frames(30)
    s.r(G_QSTAT)

    # a queued reset: the engine is as after its reset line, and what was queued behind it
    # is forgotten
    s.from_zero()
    s.voice(2, RUN | LOOP, 0x100, 0x120, 0x163, f(1.7), 0xC000, 0x40)
    s.qtime(4)
    s.qreg(G_CONTROL)
    s.qpush(0x81)
    s.qtime(6)
    s.qreg(G_MASTER)
    s.qpush(0x3F)
    s.frames(9)
    s.status(2)
    for a in (G_QSTAT, G_MASTER, G_CONTROL):
        s.r(a)
    s.count()

    # anything: notes and single writes queued for frames just gone and soon to come, with
    # the CPU's own writes and reads among them, at any clock
    rnd = random.Random(seed + 1)
    est = 0.0
    for n in range(1500):
        if n % 40 == 0:
            s.from_zero()
            est = 0.0
        for _ in range(rnd.choice((0, 1, 1, 2, 4))):
            s.clocks(rnd.choice((0, 0, 1, 2, 5, 40)))
            v = rnd.randrange(NVOICES)
            when = max(0, int(est) + rnd.choice((-2, 0, 1, 1, 2, 3, 8)))
            what = rnd.randrange(11)
            if what < 3:
                a, b = sorted((rnd.randrange(size), rnd.randrange(size)))
                s.qvoice(when, v, rnd.randrange(64), rnd.randrange(a, b + 1), a, b,
                         rnd.randrange(1 << 18), rnd.randrange(65536), rnd.randrange(256))
            elif what == 3:
                s.qtime(when)
                s.qreg(v * 16 + VOL)
                s.qpush(rnd.randrange(256), rnd.randrange(256))
            elif what == 4:
                s.qtime(when)
                s.qreg(v * 16 + rnd.randrange(16))
                s.qpush(rnd.randrange(256))
            elif what == 5:
                s.qtime(when)
                s.qreg(rnd.choice((G_MASTER, G_RAMP, G_ENDED, G_QTIME + 1)))
                s.qpush(rnd.randrange(16))
            elif what == 6:
                a, b = sorted((rnd.randrange(size), rnd.randrange(size)))
                s.voice(v, rnd.randrange(64), rnd.randrange(a, b + 1), a, b,
                        rnd.randrange(1 << 18), rnd.randrange(65536), rnd.randrange(256))
            elif what == 7:
                s.r(rnd.choice((G_QSTAT, G_QFREE, v * 16, 0x200 + rnd.randrange(0x20))))
            elif what == 8:
                s.status(v)
            elif what == 9:
                s.reg(v, CTRL, rnd.randrange(64))
            else:
                s.w(G_QSTAT, rnd.choice((0, 0, 0, QFLUSH)))
        if rnd.random() < 0.5:
            c = rnd.randrange(900)
            s.clocks(c)
            est += c / 843.75
        else:
            c = rnd.choice((1, 1, 2, 3, 7))
            s.frames(c)
            est += c


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
    frames, reads, queued, bad = replay(MegaGus(ram_bits), s.ops, log, s.expect)
    print(f"{len(s.ops)} accesses, {frames} frames ({loud} not silent, {clipped} clipped), "
          f"{reads} reads, {queued[0]} queued writes ({queued[1]} of them after their frame's "
          f"voices were let go)")
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
