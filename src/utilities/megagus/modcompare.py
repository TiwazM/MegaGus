#!/usr/bin/env python3
"""
Compares modplay's playing of a module with libopenmpt's (https://lib.openmpt.org/, here from
Homebrew: brew install libopenmpt), which is the reference for how a module should sound.

  ./modcompare.py FILE.mod [--seconds S] [--vblank] [--rows N]

Two things are compared:

  the timeline   when each row begins, by position and row, in both players
  the channels   every channel on its own: the module is written out once per channel with
                 the other channels' notes taken out (their speed, jump, break, loop and delay
                 commands stay, since those are the song's), both players play each of these,
                 and row by row the two recordings are laid over each other. A row agrees when
                 the waveforms correlate, at the best shift within a few milliseconds, and are
                 as loud as each other; where they do not, the row and its commands are listed.

modplay runs as in modplay_host.py: the same source, against the engine's model.
"""

import argparse
import ctypes
import ctypes.util
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
RATE = 48000
GUARD = 96
CENTS, DB = 5.0, 0.5      # what counts as the same pitch and level
SIGS = {b"M.K.": 4, b"M!K!": 4, b"FLT4": 4, b"4CHN": 4, b"6CHN": 6, b"8CHN": 8, b"OCTA": 8, b"CD81": 8}
NOTES = ["C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"]
PERIODS = [856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453]


def openmpt():
    lib = ctypes.CDLL(ctypes.util.find_library("openmpt") or "/opt/homebrew/lib/libopenmpt.dylib")
    lib.openmpt_module_create_from_memory2.restype = ctypes.c_void_p
    lib.openmpt_module_create_from_memory2.argtypes = [ctypes.c_char_p, ctypes.c_size_t] + [ctypes.c_void_p] * 7
    for f, res in (("get_current_row", ctypes.c_int32), ("get_current_order", ctypes.c_int32),
                   ("get_current_speed", ctypes.c_int32), ("get_current_tempo2", ctypes.c_double),
                   ("get_num_channels", ctypes.c_int32), ("destroy", None)):
        fn = getattr(lib, "openmpt_module_" + f)
        fn.restype, fn.argtypes = res, [ctypes.c_void_p]
    lib.openmpt_module_set_render_param.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int32]
    lib.openmpt_module_ctl_set_text.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p]
    lib.openmpt_module_ctl_set_boolean.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    lib.openmpt_module_read_float_stereo.restype = ctypes.c_size_t
    lib.openmpt_module_read_float_stereo.argtypes = [ctypes.c_void_p, ctypes.c_int32, ctypes.c_size_t,
                                                     ctypes.c_void_p, ctypes.c_void_p]
    return lib


def reference(data, seconds, vblank=False, chunk=8):
    """libopenmpt's playing: the sound (frames x 2) and the rows [(frame, order, row, speed, tempo)]"""
    lib = openmpt()
    mod = lib.openmpt_module_create_from_memory2(data, len(data), None, None, None, None, None, None, None)
    if not mod:
        sys.exit("libopenmpt can't load it")
    lib.openmpt_module_set_render_param(mod, 3, 2)      # linear interpolation, as the engine's
    lib.openmpt_module_set_render_param(mod, 2, 100)    # stereo separation as the file has it
    if is_s3m(data):
        lib.openmpt_module_set_render_param(mod, 4, 0)  # no gliding to a new volume: its level is the tick's
    lib.openmpt_module_ctl_set_boolean(mod, b"render.resampler.emulate_amiga", 0)
    if vblank:
        lib.openmpt_module_ctl_set_text(mod, b"vblank", b"1")
    l = (ctypes.c_float * chunk)()
    r = (ctypes.c_float * chunk)()
    out, rows, last = [], [], None
    for at in range(0, int(seconds * RATE), chunk):
        here = (lib.openmpt_module_get_current_order(mod), lib.openmpt_module_get_current_row(mod))
        if here != last:
            rows.append((at,) + here + (lib.openmpt_module_get_current_speed(mod),
                                        lib.openmpt_module_get_current_tempo2(mod)))
            last = here
        n = lib.openmpt_module_read_float_stereo(mod, RATE, chunk, l, r)
        if n == 0:
            break
        out.append(np.stack([np.frombuffer(l, dtype=np.float32, count=n),
                             np.frombuffer(r, dtype=np.float32, count=n)], axis=1).copy())
    lib.openmpt_module_destroy(mod)
    return (np.concatenate(out) if out else np.zeros((0, 2), np.float32)), rows


def ours(path, seconds, vblank=False):
    """modplay's playing on the model: the sound and the rows [(frame, order, row, speed, tempo)]"""
    import modplay_host
    r = modplay_host.Run(path, False, limit=seconds, vblank=vblank)
    snd = np.stack([np.array(r.left, dtype=np.float32), np.array(r.right, dtype=np.float32)], axis=1) / 32768.0
    return snd, [(f, o, row, sp, bpm) for f, o, pat, row, sp, bpm in r.rows]


# -----------------------------------------------------------------------------------------------
# S3M: the same three things for Scream Tracker modules
# -----------------------------------------------------------------------------------------------
def is_s3m(data):
    return data[0x2C:0x30] == b"SCRM"


def s3m_head(data):
    ordnum, insnum, patnum = (int.from_bytes(data[o:o + 2], "little") for o in (0x20, 0x22, 0x24))
    ins = [int.from_bytes(data[0x60 + ordnum + 2 * i:][:2], "little") * 16 for i in range(insnum)]
    pat = [int.from_bytes(data[0x60 + ordnum + 2 * insnum + 2 * i:][:2], "little") * 16 for i in range(patnum)]
    return ordnum, ins, pat


def s3m_rows(data, at):
    """A packed pattern as 64 rows of {channel: [note, ins, vol, cmd, info]} (None: not there)"""
    rows = []
    q = at + 2
    for _ in range(64):
        row = {}
        while q < len(data):
            w = data[q]
            q += 1
            if w == 0:
                break
            c = row.setdefault(w & 31, [None] * 5)
            if w & 32:
                c[0], c[1] = data[q], data[q + 1]
                q += 2
            if w & 64:
                c[2] = data[q]
                q += 1
            if w & 128:
                c[3], c[4] = data[q], data[q + 1]
                q += 2
        rows.append(row)
    return rows, q - at


def s3m_pack(rows):
    out = bytearray()
    for row in rows:
        for ch, c in sorted(row.items()):
            w = ch | (32 if c[0] is not None else 0) | (64 if c[2] is not None else 0) | (128 if c[3] is not None else 0)
            if w == ch:
                continue
            out.append(w)
            if w & 32:
                out += bytes([c[0], c[1]])
            if w & 64:
                out.append(c[2])
            if w & 128:
                out += bytes([c[3], c[4]])
        out.append(0)
    return out


def s3m_channels(data):
    """The channels that play samples and have something in a pattern: as modplay gives voices"""
    _, _, pat = s3m_head(data)
    used = set()
    for at in pat:
        if at:
            for row in s3m_rows(data, at)[0]:
                used.update(ch for ch, c in row.items()
                            if c[0] is not None or c[2] is not None or (c[3] is not None and c[3]))
    return [c for c in range(32) if data[0x40 + c] < 16 and c in used]


def s3m_solo(data, ch):
    """Only this channel's notes; of the others, what steers the song: speed, tempo, jumps,
    breaks, loops, delays and the song's volume"""
    d = bytearray(data)
    _, _, pat = s3m_head(data)
    for at in pat:
        if not at:
            continue
        rows, size = s3m_rows(data, at)
        for row in rows:
            for c in list(row):
                if c == ch:
                    continue
                cmd, info = row[c][3], row[c][4]
                keep = cmd in (1, 2, 3, 20, 22) or (cmd == 19 and info >> 4 in (0xB, 0xE))
                if keep:
                    row[c] = [None, None, None, cmd, info]
                else:
                    del row[c]
        packed = s3m_pack(rows)
        assert len(packed) + 2 <= size
        d[at + 2:at + 2 + len(packed)] = packed
        d[at:at + 2] = (len(packed) + 2).to_bytes(2, "little")
    return bytes(d)


def s3m_toned(data):
    """Every sample one cycle of a sine, looped, at the end of the file"""
    import math
    d = bytearray(data)
    _, ins, _ = s3m_head(data)
    bias = 0 if data[0x2A] == 1 else 128
    wave_ = bytes((int(round(127 * math.sin(2 * math.pi * i / 16))) + bias) & 255 for i in range(16))
    d += bytes(-len(d) % 16)
    seg = len(d) // 16
    d += wave_
    for o in ins:
        if o and d[o] == 1 and int.from_bytes(d[o + 0x10:o + 0x14], "little"):
            d[o + 0x0D] = seg >> 16
            d[o + 0x0E:o + 0x10] = (seg & 0xFFFF).to_bytes(2, "little")
            d[o + 0x10:o + 0x14] = (16).to_bytes(4, "little")
            d[o + 0x14:o + 0x18] = (0).to_bytes(4, "little")
            d[o + 0x18:o + 0x1C] = (16).to_bytes(4, "little")
            d[o + 0x1E] = 0
            d[o + 0x1F] = 1
    return bytes(d)


_S3M_ROWS = {}


def s3m_cell(data, order, row, ch):
    key = (id(data), order)
    if key not in _S3M_ROWS:
        ordnum, _, pat = s3m_head(data)
        p = data[0x60 + order]
        _S3M_ROWS[key] = s3m_rows(data, pat[p])[0] if p < len(pat) and pat[p] else [{}] * 64
    c = _S3M_ROWS[key][row].get(ch)
    if not c:
        return "... .. .. ..."
    note = "..."
    if c[0] is not None and c[0] != 255:
        note = "^^^" if c[0] == 254 else NOTES[c[0] & 15] + str(c[0] >> 4)
    ins = f"{c[1]:02d}" if c[1] else ".."
    vol = f"{c[2]:02d}" if c[2] is not None else ".."
    fx = f"{chr(64 + c[3])}{c[4]:02X}" if c[3] else "..."
    return f"{note} {ins} {vol} {fx}"


def solo(data, ch, channels):
    """The module with only this channel's notes: the others keep what steers the song"""
    if is_s3m(data):
        return s3m_solo(data, ch)
    d = bytearray(data)
    npat = max(d[952:1080]) + 1
    for i in range(npat * 64 * channels):
        if i % channels == ch:
            continue
        o = 1084 + i * 4
        fx, arg = d[o + 2] & 15, d[o + 3]
        keep = fx in (0xB, 0xD, 0xF) or (fx == 0xE and arg >> 4 in (6, 0xE))
        d[o], d[o + 1], d[o + 2] = 0, 0, fx if keep else 0
        d[o + 3] = arg if keep else 0
    return bytes(d)


def cell(data, channels, order, row, ch):
    if is_s3m(data):
        return s3m_cell(data, order, row, ch)
    p = data[952 + order]
    o = 1084 + ((p * 64 + row) * channels + ch) * 4
    period = ((data[o] & 15) << 8) | data[o + 1]
    smp = (data[o] & 0xF0) | (data[o + 2] >> 4)
    note = "---"
    if period:
        best = min(range(60), key=lambda n: abs(PERIODS[n % 12] / (1 << (n // 12)) * 2 - period))
        note = NOTES[best % 12] + str(best // 12)
    fx = (data[o + 2] & 15, data[o + 3])
    return f"{note} {smp:02d} {fx[0]:X}{fx[1]:02X}" if (period or smp or fx != (0, 0)) else "... .. ..."


def timeline(a, b):
    """Rows of ours (a) against the reference's (b): the same rows in the same order, each as
    long. libopenmpt makes a tick a whole number of output frames (705 for the 705.88 of tempo
    170), modplay keeps the fraction as an Amiga's timer does: a row may differ by a frame a
    tick, and the two drift apart by up to a tenth of a percent, which is not counted."""
    off = a[0][0] - b[0][0] if a and b else 0
    n = min(len(a), len(b))
    bad = []
    for i in range(n - 1):
        if a[i][1:3] != b[i][1:3]:
            bad.append((i, "different row", a[i], b[i]))
            break
        la, lb = a[i + 1][0] - a[i][0], b[i + 1][0] - b[i][0]
        ticks = max(1, round(lb / (2.5 * RATE / max(b[i][4], 1))))
        if abs(la - lb) > ticks + 24:      # a frame a tick, and the slack of both row marks
            bad.append((i, f"{la} frames long, not {lb}", a[i], b[i]))
    return off, n, bad


def lay_over(x, y, lag=240):
    """The best correlation of y against x within +-lag frames, and y's loudness over x's in dB"""
    ex, ey = float(np.dot(x, x)), float(np.dot(y, y))
    if ex == 0.0 or ey == 0.0:
        return 0.0, 0.0
    n = len(x) + 2 * lag
    size = 1 << (n - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(x, size) * np.conj(np.fft.rfft(y, size)), size)
    c = np.concatenate([c[-lag:], c[:lag + 1]])
    return float(c.max() / np.sqrt(ex * ey)), 10 * np.log10(ey / ex)


def toned(data):
    """The module with every sample made one cycle of a sine, looped: what a channel then
    plays has a pitch and a level that can be read off the recording, tick by tick"""
    import math
    if is_s3m(data):
        return s3m_toned(data)
    d = bytearray(data[:1084 + (max(data[952:1080]) + 1) * 64 * SIGS[data[1080:1084]] * 4])
    wave_ = bytes(int(round(127 * math.sin(2 * math.pi * i / 16))) & 255 for i in range(16))
    for i in range(31):
        o = 20 + i * 30
        if d[o + 22] or d[o + 23]:
            d[o + 22:o + 24] = (8).to_bytes(2, "big")
            d[o + 26:o + 30] = (0).to_bytes(2, "big") + (8).to_bytes(2, "big")
            d += wave_
    return bytes(d)


def measure(x, a, b):
    """Pitch in Hz (from the zero crossings) and peak level of x[a:b]; (0, level) if it has no pitch.
    The level is taken over whole cycles where there are any, or it would depend on where in a
    cycle the stretch happens to begin and end."""
    seg = x[a:b]
    if len(seg) < 64:
        return 0.0, 0.0
    level = float(np.sqrt(2 * np.mean(seg * seg)))
    up = np.nonzero((seg[:-1] < 0) & (seg[1:] >= 0))[0]
    if len(up) >= 2:
        whole = seg[up[0] + 1:up[-1] + 1]
        level = float(np.sqrt(2 * np.mean(whole * whole)))
    if len(up) < 3 or level < 2e-4:
        return 0.0, level
    t = up + (-seg[up]) / (seg[up + 1] - seg[up])
    # the cycles' lengths, but for one that is cut short or stretched where a sample starts
    # again in the middle of a note (a sample number with no note, in some S3Ms)
    dt = np.diff(t)
    med = np.median(dt)
    dt = dt[np.abs(dt - med) < 0.2 * med]
    return float(RATE / dt.mean()), level


def level(x, a, ln, g):
    """The level at the end of the tick x[a:a+ln]: over its last third, or over more of it when
    a low note has no whole cycle in that"""
    return level_at(x, a, ln, g)[0]


def level_at(x, a, ln, g):
    """That, and how far into the tick (0..1) the stretch it was taken over lies"""
    for part in (3, 2):
        seg = x[a + ln - ln // part:a + ln - g // 2]
        up = np.nonzero((seg[:-1] < 0) & (seg[1:] >= 0))[0]
        if len(up) >= 2:
            whole = seg[up[0] + 1:up[-1] + 1]
            mid = ln - ln // part + (up[0] + up[-1]) / 2 + 1
            return float(np.sqrt(2 * np.mean(whole * whole))), mid / ln
    seg = x[a + ln // 4:a + ln - g // 2]
    return (float(np.abs(seg).max()) if len(seg) else 0.0), 0.6


def one_channel_toned(args):
    """A channel of the toned module in both players: per tick (order, row, tick, cents, dB, text)"""
    path, data, ch, channels, seconds, vblank = args
    work = os.path.join(HERE, "work")
    sp = os.path.join(work, f"tone_{os.path.basename(path)}_{ch}.mod")
    sdata = solo(toned(data), ch, channels)
    _S3M_ROWS.clear()
    with open(sp, "wb") as f:
        f.write(sdata)
    ref, rrows = reference(sdata, seconds, vblank)
    mine, mrows = ours(sp, seconds, vblank)
    os.remove(sp)
    ref, mine = ref.sum(axis=1).astype(np.float64), mine.sum(axis=1).astype(np.float64)
    res = []
    before = 0.0        # modplay's level in the tick before
    for i in range(min(len(rrows), len(mrows)) - 1):
        if mrows[i][1:3] != rrows[i][1:3]:
            break
        speed = max(mrows[i][3], 1)
        rlen, mlen = rrows[i + 1][0] - rrows[i][0], mrows[i + 1][0] - mrows[i][0]
        ticks = max(1, round(mlen / (2.5 * RATE / max(mrows[i][4], 1))))   # also a row held by EEx
        for t in range(ticks):
            # the middle of the tick: clear of both players' volume ramps and of the marks' slack
            ra = rrows[i][0] + rlen * t // ticks
            ma = mrows[i][0] + mlen * t // ticks
            ln = min(rlen, mlen) // ticks
            # pitch over the tick's middle, level over its last third: libopenmpt glides to a
            # new volume over most of a tick, the engine in a millisecond
            g = max(GUARD, ln // 5)
            fr, _ = measure(ref, ra + g, ra + ln - g // 2)
            fm, _ = measure(mine, ma + g, ma + ln - g // 2)
            lr, where = level_at(ref, ra, ln, g)
            lm = level(mine, ma, ln, g)
            glide = before + (lm - before) * where
            before = lm
            if lr < 2e-4 and lm < 2e-4:
                continue
            if min(lr, lm) < 2e-4:
                cents, db = 0.0, (99.0 if lm > lr else -99.0)
            else:
                cents = 1200 * np.log2(fm / fr) if fr and fm else 0.0
                db = 20 * np.log10(lm / lr)
            # In an S3M libopenmpt glides from one tick's volume to the next across the whole
            # tick (a GUS's volume ramp), modplay is there in a millisecond: this is what the
            # reference would measure if it were heading for modplay's level
            res.append((mrows[i][1], mrows[i][2], t, cents, db,
                        cell(data, channels, mrows[i][1], mrows[i][2], ch), fr, fm,
                        20 * np.log10(glide / lr) if glide > 0 and lr >= 2e-4 else db))
    gain = 0.0
    if is_s3m(data):
        # How loud an S3M is as a whole is the player's choice (libopenmpt takes the file's
        # mixing volume, modplay the number of voices): what is compared is every tick's
        # level against the channel's usual difference
        # (taken from the levels with the glide allowed for: in a channel that slides most of
        # the time the plain ones would put it too low)
        both = [r[8] for r in res if abs(r[4]) < 90]
        gain = float(np.median(both)) if both else 0.0
        fixed = []
        for r in res:
            db = r[4] - gain if abs(r[4]) < 90 else r[4]
            alt = r[8] - gain
            fixed.append(r[:4] + (alt if abs(alt) < abs(db) else db,) + r[5:8])
        res = fixed
    else:
        res = [r[:8] for r in res]
    return ch, res, gain


def one_channel(args):
    path, data, ch, channels, seconds, vblank = args
    work = os.path.join(HERE, "work")
    sp = os.path.join(work, f"solo_{os.path.basename(path)}_{ch}.mod")
    sdata = solo(data, ch, channels)
    with open(sp, "wb") as f:
        f.write(sdata)
    ref, rrows = reference(sdata, seconds, vblank)
    mine, mrows = ours(sp, seconds, vblank)
    os.remove(sp)
    off, n, tbad = timeline(mrows, rrows)
    ref, mine = ref.sum(axis=1), mine.sum(axis=1)
    res = []
    for i in range(n - 1):
        if mrows[i][1:3] != rrows[i][1:3]:
            break
        # a row's frames in each recording, but for its last 2 ms: neither player's row
        # marks are exact to the frame, and the next row's note must not be taken for this one's
        ra, rb = rrows[i][0], rrows[i + 1][0]
        ma = mrows[i][0]
        ln = min(rb - ra, mrows[i + 1][0] - ma, len(ref) - ra, len(mine) - ma) - GUARD
        if ln < 200:
            continue
        x, y = ref[ra:ra + ln].astype(np.float64), mine[ma:ma + ln].astype(np.float64)
        rx, ry = np.sqrt(np.mean(x * x)), np.sqrt(np.mean(y * y))
        if rx < 1e-4 and ry < 1e-4:
            continue
        corr, db = lay_over(x, y)
        if min(rx, ry) < 1e-4:
            corr, db = 0.0, (99.0 if ry > rx else -99.0)
        res.append((mrows[i][1], mrows[i][2], corr, db, cell(data, channels, mrows[i][1], mrows[i][2], ch)))
    return ch, res, len(tbad)


def main():
    ap = argparse.ArgumentParser(description="modplay against libopenmpt")
    ap.add_argument("mod")
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--vblank", action="store_true")
    ap.add_argument("--rows", type=int, default=12, help="how many differing rows to list per channel")
    ap.add_argument("--waves", action="store_true", help="also lay the waveforms over each other")
    a = ap.parse_args()
    data = open(a.mod, "rb").read()
    s3m = is_s3m(data)
    chans = s3m_channels(data) if s3m else list(range(SIGS.get(data[1080:1084], 0)))
    channels = len(chans)
    if not channels:
        sys.exit("not a module modplay plays")
    name = os.path.basename(a.mod)

    ref, rrows = reference(data, a.seconds, a.vblank)
    mine, mrows = ours(a.mod, a.seconds, a.vblank)
    off, n, bad = timeline(mrows, rrows)
    drift = (mrows[n - 1][0] - off) - rrows[n - 1][0]
    print(f"{name}: {channels} channels, {n} rows in {a.seconds:.0f} s")
    print(f"  timeline: {n - 1 - len(bad)} of {n - 1} rows are the same row and as long in both; "
          f"after {rrows[n - 1][0] / RATE:.0f} s modplay is {drift / 48:+.1f} ms off")
    for i, why, x, y in bad[:5]:
        print(f"    row {i}: {why}: modplay position {x[1]} row {x[2]}, libopenmpt position {y[1]} row {y[2]}")

    jobs = [(a.mod, data, ch, channels, a.seconds, a.vblank) for ch in chans]
    with ProcessPoolExecutor() as ex:
        toned_results = list(ex.map(one_channel_toned, jobs))
    print("  with every sample a pure tone, pitch and level of each tick:")
    tt = ta = 0
    by_fx = {}
    for ch, res, gain in toned_results:
        ok = [r for r in res if abs(r[3]) <= CENTS and abs(r[4]) <= DB]
        tt += len(res)
        ta += len(ok)
        print(f"  channel {ch + 1}: {len(ok)} of {len(res)} sounding ticks agree" +
              (f" (modplay {gain:+.1f} dB louder)" if s3m else ""))
        shown = 0
        for o, row, t, cents, db, text, fr, fm in res:
            if abs(cents) <= CENTS and abs(db) <= DB:
                continue
            if s3m:
                fx = text[10:11] if text[10] != "." else "none"
            else:
                fx = text[7:8] if text[7:10] not in ("000", "...") else "none"
            by_fx[fx] = by_fx.get(fx, 0) + 1
            if shown < a.rows:
                shown += 1
                print(f"    position {o:2d} row {row:2d} tick {t:2d}  {text}   pitch {cents:+7.1f} cents "
                      f"({fr:.1f} / {fm:.1f} Hz), level {db:+.1f} dB")
    print(f"  in all: {ta} of {tt} ticks agree ({100.0 * ta / max(tt, 1):.1f} %) "
          f"within {CENTS} cents and {DB} dB")
    if by_fx:
        print("  differing ticks by the command on their row: " +
              ", ".join(f"{k}: {v}" for k, v in sorted(by_fx.items(), key=lambda kv: -kv[1])))
    if not a.waves:
        return 0
    print("  with its own samples, each row's waveform:")
    with ProcessPoolExecutor() as ex:
        results = list(ex.map(one_channel, [(a.mod, data, ch, channels, a.seconds, a.vblank)
                                            for ch in range(channels)]))
    total = agree = 0
    by_fx = {}
    for ch, res, tbad in results:
        ok = [r for r in res if r[2] >= 0.9 and abs(r[3]) <= 1.5]
        total += len(res)
        agree += len(ok)
        med = np.median([r[2] for r in res]) if res else 0
        print(f"  channel {ch + 1}: {len(ok)} of {len(res)} sounding rows agree, median correlation {med:.3f}")
        differ = [r for r in res if not (r[2] >= 0.9 and abs(r[3]) <= 1.5)]
        for o, row, corr, db, text in differ[:a.rows]:
            print(f"    position {o:2d} row {row:2d}  {text}   correlation {corr:.2f}, level {db:+.1f} dB")
        for o, row, corr, db, text in differ:
            fx = text[7:8] if text[7:10] != "000" and text[0] != "." or text[7:10] not in ("000", "...") else "none"
            by_fx[fx] = by_fx.get(fx, 0) + 1
    print(f"  in all: {agree} of {total} rows agree ({100.0 * agree / max(total, 1):.1f} %)")
    if by_fx:
        print("  differing rows by the command on them: " +
              ", ".join(f"{k}: {v}" for k, v in sorted(by_fx.items(), key=lambda kv: -kv[1])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
