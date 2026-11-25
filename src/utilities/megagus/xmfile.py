#!/usr/bin/env python3
"""
FastTracker 2 modules (XM) read into Python objects and written out again: for makexm.py,
which makes the player's test modules, and modcompare.py, which takes modules apart to compare
the player with libopenmpt a channel at a time.

  load(bytes) -> XM          save(XM) -> bytes

A pattern is a list of rows, a row a list of cells, one a channel: [note, instrument, volume
column, command, argument], 0 where there is nothing. A note is 1 (C-0) to 96 (B-7), 97 is the
key going up. A sample's data is a list of signed numbers, 8-bit or 16-bit as its 'bits' say.
"""
import struct

SIGNATURE = b"Extended Module: "


class Sample:
    def __init__(self, data=(), bits=8, loop=None, pingpong=False, volume=64, finetune=0, pan=128,
                 relnote=0, name=""):
        self.data = list(data)
        self.bits = bits
        self.loop = loop            # (first frame, frames) or None
        self.pingpong = pingpong
        self.volume, self.finetune, self.pan, self.relnote, self.name = volume, finetune, pan, relnote, name


class Instrument:
    def __init__(self, name="", samples=None):
        self.name = name
        self.samples = samples or []
        self.notemap = [0] * 96
        self.vol_env, self.pan_env = [], []         # [(tick, value 0..64)]
        self.vol_sustain = self.vol_loop = None     # a point's number; (first point, last point)
        self.pan_sustain = self.pan_loop = None
        self.vol_on = self.pan_on = False
        self.vib_type = self.vib_sweep = self.vib_depth = self.vib_rate = 0
        self.fadeout = 0
        self.tail = b""             # what the file's header had after the fields above


class XM:
    def __init__(self, title="", channels=4, speed=6, bpm=125, linear=True):
        self.title, self.tracker = title, "FastTracker v2.00   "
        self.channels, self.speed, self.bpm, self.linear = channels, speed, bpm, linear
        self.restart = 0
        self.orders = []
        self.patterns = []
        self.instruments = []

    def pattern(self, rows=64):
        self.patterns.append([[[0, 0, 0, 0, 0] for _ in range(self.channels)] for _ in range(rows)])
        return self.patterns[-1]


def _env(points):
    out = b"".join(struct.pack("<HH", x, y) for x, y in points[:12])
    return out + bytes(48 - len(out))


def _env_type(on, sustain, loop):
    return (1 if on else 0) | (2 if sustain is not None else 0) | (4 if loop else 0)


def pack_pattern(rows):
    out = bytearray()
    for row in rows:
        for note, ins, vol, fx, arg in row:
            if note and ins and vol and fx and arg:
                out += bytes([note, ins, vol, fx, arg])
                continue
            what = 0x80 | (1 if note else 0) | (2 if ins else 0) | (4 if vol else 0) | (8 if fx else 0) | (16 if arg else 0)
            out.append(what)
            out += bytes(v for v in (note, ins, vol, fx, arg) if v)
    return bytes(out)


def unpack_pattern(data, rows, channels):
    out, q = [], 0
    for _ in range(rows):
        row = []
        for _ in range(channels):
            cell = [0, 0, 0, 0, 0]
            if q < len(data):
                b = data[q]
                q += 1
                if b & 0x80:
                    for k in range(5):
                        if b & (1 << k):
                            cell[k] = data[q]
                            q += 1
                else:
                    cell = [b] + list(data[q:q + 4])
                    q += 4
            row.append(cell)
        out.append(row)
    return out


def _deltas(s):
    mask = 0xFFFF if s.bits == 16 else 0xFF
    out, last = [], 0
    for v in s.data:
        out.append((v - last) & mask)
        last = v
    return struct.pack("<%d%s" % (len(out), "H" if s.bits == 16 else "B"), *out)


def save(xm):
    name = xm.title.encode("latin1")[:20].ljust(20)
    out = bytearray(SIGNATURE + name + b"\x1a" + xm.tracker.encode("latin1")[:20].ljust(20))
    orders = bytes(xm.orders) + bytes(256 - len(xm.orders))
    out += struct.pack("<HIHHHHHHHH", 0x0104, 20 + 256, len(xm.orders), xm.restart, xm.channels,
                       len(xm.patterns), len(xm.instruments), 1 if xm.linear else 0, xm.speed, xm.bpm)
    out += orders
    for rows in xm.patterns:
        packed = pack_pattern(rows)
        if all(not any(c) for row in rows for c in row):
            packed = b""                                    # an empty pattern has no data at all
        out += struct.pack("<IBHH", 9, 0, len(rows), len(packed)) + packed
    for ins in xm.instruments:
        iname = ins.name.encode("latin1")[:22].ljust(22)
        if not ins.samples:
            # as FastTracker writes an instrument with no samples: 33 bytes. libopenmpt takes a
            # file whose headers are not as FastTracker's for another tracker's, and then plays
            # it by other rules
            out += struct.pack("<I", 33) + iname + bytes([0]) + struct.pack("<HI", 0, 40)
            continue
        head = bytearray(struct.pack("<I", 0) + iname + bytes([0]) + struct.pack("<H", len(ins.samples)))
        head += struct.pack("<I", 40) + bytes(ins.notemap) + _env(ins.vol_env) + _env(ins.pan_env)
        vl, pl = ins.vol_loop or (0, 0), ins.pan_loop or (0, 0)
        head += bytes([len(ins.vol_env), len(ins.pan_env), ins.vol_sustain or 0, vl[0], vl[1],
                       ins.pan_sustain or 0, pl[0], pl[1],
                       _env_type(ins.vol_on, ins.vol_sustain, ins.vol_loop),
                       _env_type(ins.pan_on, ins.pan_sustain, ins.pan_loop),
                       ins.vib_type, ins.vib_sweep, ins.vib_depth, ins.vib_rate])
        head += struct.pack("<H", ins.fadeout) + (ins.tail or bytes(22))
        head[0:4] = struct.pack("<I", len(head))
        out += head
        datas = [_deltas(s) for s in ins.samples]
        for s, d in zip(ins.samples, datas):
            unit = 2 if s.bits == 16 else 1
            ls, ll = s.loop if s.loop else (0, 0)
            kind = (0 if not s.loop else 2 if s.pingpong else 1) | (16 if s.bits == 16 else 0)
            out += struct.pack("<IIIBbBBb", len(d), ls * unit, ll * unit, s.volume, s.finetune, kind, s.pan,
                               s.relnote) + bytes([0]) + s.name.encode("latin1")[:22].ljust(22)
        for d in datas:
            out += d
    return bytes(out)


def load(data):
    if data[:17] != SIGNATURE:
        raise ValueError("not an XM")
    xm = XM(data[17:37].decode("latin1").rstrip("\0 "))
    xm.tracker = data[38:58].decode("latin1")
    hsize, = struct.unpack_from("<I", data, 60)
    songlen, xm.restart, xm.channels, npat, nins, flags, xm.speed, xm.bpm = struct.unpack_from("<8H", data, 64)
    xm.linear = bool(flags & 1)
    xm.orders = list(data[80:80 + songlen])
    o = 60 + hsize
    for _ in range(npat):
        hl, _, rows, size = struct.unpack_from("<IBHH", data, o)
        xm.patterns.append(unpack_pattern(data[o + hl:o + hl + size], rows, xm.channels))
        o += hl + size
    for _ in range(nins):
        if o + 29 > len(data):
            xm.instruments.append(Instrument())
            continue
        isz, = struct.unpack_from("<I", data, o)
        ins = Instrument(data[o + 4:o + 26].decode("latin1").rstrip("\0 "))
        ns, = struct.unpack_from("<H", data, o + 27)
        xm.instruments.append(ins)
        if ns == 0:
            o += isz
            continue
        shs, = struct.unpack_from("<I", data, o + 29)
        ins.notemap = list(data[o + 33:o + 129])
        pts = struct.unpack_from("<48H", data, o + 129)
        nv, np_, vs, vls, vle, ps, pls, ple, vt, pt, ins.vib_type, ins.vib_sweep, ins.vib_depth, ins.vib_rate = data[o + 225:o + 239]
        ins.vol_env = [(pts[2 * i], pts[2 * i + 1]) for i in range(min(nv, 12))]
        ins.pan_env = [(pts[24 + 2 * i], pts[25 + 2 * i]) for i in range(min(np_, 12))]
        ins.vol_on, ins.pan_on = bool(vt & 1), bool(pt & 1)
        ins.vol_sustain = vs if vt & 2 else None
        ins.pan_sustain = ps if pt & 2 else None
        ins.vol_loop = (vls, vle) if vt & 4 else None
        ins.pan_loop = (pls, ple) if pt & 4 else None
        ins.fadeout, = struct.unpack_from("<H", data, o + 239)
        ins.tail = data[o + 241:o + isz]
        q = o + isz
        heads = []
        for _ in range(ns):
            heads.append(struct.unpack_from("<IIIBbBBb", data, q) + (data[q + 18:q + 40].decode("latin1").rstrip("\0 "),))
            q += shs
        for ln, ls, ll, vol, ft, kind, pan, rel, sname in heads:
            wide = bool(kind & 16)
            raw = data[q:q + ln]
            q += ln
            n = len(raw) // (2 if wide else 1)
            vals, acc = [], 0
            for d in struct.unpack_from("<%d%s" % (n, "H" if wide else "B"), raw):
                acc = (acc + d) & (0xFFFF if wide else 0xFF)
                vals.append(acc - (65536 if wide and acc >= 32768 else 256 if not wide and acc >= 128 else 0))
            unit = 2 if wide else 1
            s = Sample(vals, 16 if wide else 8, (ls // unit, ll // unit) if kind & 3 and ll else None,
                       (kind & 3) >= 2, vol, ft, pan, rel, sname)
            ins.samples.append(s)
        o = q
    return xm


if __name__ == "__main__":
    import sys
    for path in sys.argv[1:]:
        d = open(path, "rb").read()
        x = load(d)
        again = load(save(x))
        same = (again.patterns == x.patterns and
                [[s.data for s in i.samples] for i in again.instruments] == [[s.data for s in i.samples] for i in x.instruments])
        print(f"{path}: '{x.title}' {x.channels} channels, {len(x.patterns)} patterns, {len(x.instruments)} instruments, "
              f"{sum(len(i.samples) for i in x.instruments)} samples; written and read again: {'the same' if same else 'DIFFERENT'}")
