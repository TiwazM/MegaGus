#!/usr/bin/env python3
"""
Runs modplay.c on this computer against the engine's model (src/tests/megagus/megagus_test.py),
instead of a MEGA65: the player's far memory accesses are answered from the model, time passes
as the accesses and the song's clock ask for it, and what the engine then plays is written to
a WAV file.

  ./modplay_host.py FILE.mod [-o OUT.wav] [--sim] [--vblank] [--expect FILE.json] [--trace FILE]

  --sim      run the cc65 build in cc65's simulator (sim65) instead of the host's own
             compiler's: 16-bit ints, as on the MEGA65
  --vblank   VBlank timing: every F command is a speed, none a tempo
  --expect   check what the player told the engine against makemod.py's test.json
  --trace    write every register write (frame, voice, register, value) to a file: two runs
             that played the same have the same trace

  ./modplay_host.py --selftest      makes the test module, plays it both ways, checks both
"""

import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tests", "megagus"))
from megagus_test import MegaGus  # noqa: E402

RATE = 48000
ACCESS = 10e-6          # a far peek or poke with the DMA, from C
PER_BYTE = 0.3e-6       # a DMA copy, a byte
IDLE = 0.25e-3          # the player found no tick due and looked again this much later
RAM, REG = 0xE000000, 0xEFFF000


def build(sim):
    """The player for this computer: built when modplay.c is newer than what is there"""
    work = os.path.join(HERE, "work")
    os.makedirs(work, exist_ok=True)
    src = os.path.join(HERE, "modplay.c")
    exe = os.path.join(work, "modplay_sim" if sim else "modplay_host")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        if sim:
            subprocess.run(["cl65", "-t", "sim6502", "-O", "-o", exe, src], check=True)
        else:
            subprocess.run(["cc", "-O1", "-Wall", "-o", exe, src], check=True)
    return [shutil.which("sim65") or "sim65", exe] if sim else [exe]


class Run:
    """One playing of a module: the sound, the rows it went through, every register write"""

    def __init__(self, mod, sim=False, limit=600.0, vblank=False, switch=0):
        self.m = MegaGus(19)
        self.left, self.right = [], []
        self.rows = []          # (frame, order, pattern, row, speed, bpm)
        self.writes = []        # (frame, register address in the block, value)
        self.said = []
        now = 0.0
        m = self.m

        def run_until(t):
            while len(self.left) < int(t * RATE):
                l, r = m.frame()
                self.left.append(l)
                self.right.append(r)

        def write(a, v):
            if a < REG:
                m.write(1, a - RAM, v)
            else:
                m.write(0, a - REG, v)
                self.writes.append((len(self.left), a - REG, v))

        p = subprocess.Popen(build(sim) + (["-v"] if vblank else []) + ([f"-v{switch}"] if switch else []) + [mod], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, text=True)
        for line in p.stdout:
            op, _, rest = line.rstrip("\n").partition(" ")
            if op == "I":
                now += IDLE
                run_until(now)
            elif op == "R":
                now += ACCESS
                run_until(now)
                a = int(rest, 16)
                v = m.read(1, a - RAM) if a < REG else m.read(0, a - REG)
                p.stdin.write(f"{v:x}\n")
                p.stdin.flush()
            elif op == "W":
                now += ACCESS
                run_until(now)
                a, v = (int(x, 16) for x in rest.split())
                write(a, v)
            elif op == "C":
                vals = [int(x, 16) for x in rest.split()]
                now += ACCESS + PER_BYTE * (len(vals) - 1)
                run_until(now)
                for i, v in enumerate(vals[1:]):
                    write(vals[0] + i, v)
            elif op == "D":
                a, n = rest.split()
                a, n = int(a, 16), int(n)
                now += ACCESS + PER_BYTE * n
                run_until(now)
                p.stdin.write(" ".join(f"{m.read(1, a - RAM + i):x}" for i in range(n)) + "\n")
                p.stdin.flush()
            elif op == "M":
                self.rows.append((len(self.left),) + tuple(int(x) for x in rest.split()))
            elif op == "P":
                self.said.append(rest)
            else:
                sys.exit(f"what is '{line.strip()}'?")
            if now > limit:
                p.kill()
                self.said.append(f"stopped after {limit:.0f} s")
                break
        self.code = p.wait()
        run_until(now + 0.1)

    def wav(self, path):
        with wave.open(path, "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(b"".join(struct.pack("<hh", l, r) for l, r in zip(self.left, self.right)))

    def trace(self, path):
        with open(path, "w") as f:
            for (frame, order, pattern, row, speed, bpm) in self.rows:
                f.write(f"row {order} {pattern} {row} {speed} {bpm}\n")
            # frames differ by a poll or two between builds: what was written, in order, does not
            for frame, a, v in self.writes:
                f.write(f"{a:03x} {v:02x}\n")

    # what a register held at a frame
    def reg(self, voice, offset, size, frame):
        v = [0] * size
        for f, a, x in self.writes:
            if f > frame:
                break
            if a >> 4 == voice and a < 0x200 and offset <= (a & 15) < offset + size:
                v[(a & 15) - offset] = x
        return sum(b << (8 * i) for i, b in enumerate(v))

    def check(self, expect):
        problems = []
        rows = self.rows

        def find(order, row, nth=0):
            hits = [i for i, r in enumerate(rows) if r[1] == order and r[3] == row]
            return hits[nth] if len(hits) > nth else None

        for e in expect:
            what, want, name = e["what"], e["value"], e["name"]
            if what == "sequence":
                got = [r[3] for r in rows if r[1] == e["order"] and r[3] >= want[0]][:len(want)]
                if got != want:
                    problems.append(f"{name}: rows {got}, not {want}")
                continue
            if what == "follows":
                i = find(*want[0])
                got = list(rows[i + 1][1:4:2]) if i is not None and i + 1 < len(rows) else None
                if got != want[1]:
                    problems.append(f"{name}: after {want[0]} came {got}, not {want[1]}")
                continue
            if what == "last":
                got = [rows[-1][1], rows[-1][3]]
                if got != want:
                    problems.append(f"{name}: the last row was {got}, not {want}")
                continue
            i = find(e["order"], e["row"])
            if i is None:
                problems.append(f"{name}: position {e['order']} row {e['row']} was never played")
                continue
            begin = rows[i][0]
            until = rows[i + 1][0] if i + 1 < len(rows) else len(self.left)
            tick = RATE * 5 // (2 * rows[i][5])        # frames a tick, at the tempo the row began with
            when = begin + e["tick"] * tick + tick // 2
            v = e["voice"]
            if what == "freq":
                got = self.reg(v, 4, 3, when)
            elif what == "vol":
                got = self.reg(v, 2, 2, when)
            elif what == "pan":
                got = self.reg(v, 1, 1, when)
            elif what == "master":
                got = [x for f, a, x in self.writes if f <= when and a == 0x205][-1]
            elif what == "start":
                got = self.reg(v, 10, 3, when)
            elif what == "end":
                got = self.reg(v, 13, 3, when)
            elif what == "start pos":
                got = self.reg(v, 7, 3, when)
            elif what == "starts":
                got = sum(1 for f, a, x in self.writes if begin <= f <= when and a == v * 16 + 9)
            elif what == "start tick":
                first = [f for f, a, x in self.writes if begin <= f <= when and a == v * 16 + 9]
                got = (first[0] - begin) // tick if first else None
            elif what == "row frames":
                got = until - begin
                if abs(got - want) > 40:
                    problems.append(f"{name}: the row took {got} frames, not {want}")
                continue
            elif what == "hz":
                x = self.left[begin + tick:begin + 5 * tick]
                ups = [i for i in range(len(x) - 1) if x[i] < 0 <= x[i + 1]]    # whole cycles
                got = (len(ups) - 1) * RATE / (ups[-1] - ups[0]) if len(ups) > 1 else 0
                if abs(got - want) > 1.5 or max(abs(s) for s in x) < 1000:
                    problems.append(f"{name}: {got:.1f} Hz, not {want:.1f}")
                continue
            elif what in ("running", "in loop"):
                # the model as it was then is gone: replay is not needed, the voice's own
                # record is its register writes, so ask a second model
                got = self.voice_then(v, when, what)
                if what == "in loop":
                    if not (want[0] <= got < want[1]):
                        problems.append(f"{name}: at {got}, not in {want}")
                    continue
            else:
                problems.append(f"{name}: what is '{what}'?")
                continue
            if got != want:
                problems.append(f"{name} (position {e['order']} row {e['row']} tick {e['tick']}): "
                                f"{what} is {got}, not {want}")
        return problems

    def voice_then(self, voice, frame, what):
        """Whether a voice ran, or where it was, at a frame: from a model given the same writes"""
        if not hasattr(self, "_then"):
            self._then = {}
            m = MegaGus(19)
            m.ram[:] = self.m.ram
            asked = sorted(self._asks)
            k = 0
            n = 0
            for f in asked:
                while n < f:
                    while k < len(self.writes) and self.writes[k][0] <= n:
                        m.write(0, self.writes[k][1], self.writes[k][2])
                        k += 1
                    m.frame()
                    n += 1
                self._then[f] = (list(m.running), [p >> 16 for p in m.pos])
        running, pos = self._then[frame]
        return running[voice] if what == "running" else pos[voice]

    def prepare(self, expect):
        """The frames at which check() will ask voice_then()"""
        self._asks = set()
        for e in expect:
            if e["what"] in ("running", "in loop"):
                hits = [r for r in self.rows if r[1] == e["order"] and r[3] == e["row"]]
                if hits:
                    tick = RATE * 5 // (2 * hits[0][5])
                    self._asks.add(hits[0][0] + e["tick"] * tick + tick // 2)


def selftest():
    subprocess.run([sys.executable, os.path.join(HERE, "makemod.py")], check=True)
    work = os.path.join(HERE, "work")
    failed = False
    # the checks, the module if it has another name, VBlank timing, the row the timing changes at
    for test, mod, vblank, switch in (("test", None, False, 0), ("test6", None, False, 0),
                                      ("test_vblank", None, True, 0),
                                      ("test_switch_on", "test_switch", False, 3),
                                      ("test_switch_off", "test_switch", True, 3)):
        with open(os.path.join(work, test + ".json")) as f:
            expect = json.load(f)
        traces = []
        for sim in (False, True):
            name = "cc65's simulator" if sim else "the host's compiler"
            r = Run(os.path.join(work, (mod or test) + ".mod"), sim, vblank=vblank, switch=switch)
            r.prepare(expect)
            problems = r.check(expect)
            trace = os.path.join(work, test + ("_sim.trace" if sim else "_host.trace"))
            r.trace(trace)
            traces.append(open(trace).read())
            if not sim:
                r.wav(os.path.join(work, test + ".wav"))
            peak = max(max(map(abs, r.left)), max(map(abs, r.right)))
            print(f"{test}, {name}: {len(r.rows)} rows, {len(r.left) / RATE:.1f} s, peak {peak}, "
                  f"{len(expect) - len(problems)} of {len(expect)} checks pass")
            for p in problems:
                print("  " + p)
            failed |= bool(problems) or r.code != 0 or peak >= 32767
        if traces[0] != traces[1]:
            print("  the two builds did not tell the engine the same")
            failed = True
        else:
            print("  both builds told the engine exactly the same")
    print("FAIL" if failed else "PASS")
    sys.exit(1 if failed else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mod", nargs="?")
    ap.add_argument("-o", "--out")
    ap.add_argument("--sim", action="store_true")
    ap.add_argument("--vblank", action="store_true")
    ap.add_argument("--expect")
    ap.add_argument("--trace")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--limit", type=float, default=600.0, help="stop after this many seconds of song")
    a = ap.parse_args()
    if a.selftest:
        selftest()
    if not a.mod:
        ap.error("a module, or --selftest")
    r = Run(a.mod, a.sim, a.limit, a.vblank)
    for s in r.said:
        print(s)
    out = a.out or os.path.join(HERE, "work", os.path.splitext(os.path.basename(a.mod))[0] + ".wav")
    r.wav(out)
    peak = max(max(map(abs, r.left), default=0), max(map(abs, r.right), default=0))
    print(f"{len(r.rows)} rows, {len(r.left) / RATE:.1f} s in {os.path.relpath(out)}, peak {peak}")
    if a.trace:
        r.trace(a.trace)
    if a.expect:
        with open(a.expect) as f:
            expect = json.load(f)
        r.prepare(expect)
        problems = r.check(expect)
        print(f"{len(expect) - len(problems)} of {len(expect)} checks pass")
        for p in problems:
            print("  " + p)
        sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
