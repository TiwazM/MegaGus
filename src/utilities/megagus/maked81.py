#!/usr/bin/env python3
"""
Makes a D81 disk image with modplay on it: the program first, so that RUN"*" gets it, and
the file of its overlays.

  ./maked81.py [OUT.d81] [DIRECTORY with modplay.prg and MODPLAY.OVL]

A D81 is 80 tracks of 40 sectors of 256 bytes. Track 40 has the header (sector 0), the map
of free sectors (1 and 2) and the directory (from 3); a file is a chain of sectors, each
beginning with the track and sector of the next, or with 0 and how much of the last is used.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TRACKS, SECTORS = 80, 40
DIR_TRACK = 40


def offset(track, sector):
    return ((track - 1) * SECTORS + sector) * 256


def padded(name, n=16):
    return name.encode("ascii").ljust(n, b"\xa0")


def make(files, disk_name="MEGAGUS PLAYER", disk_id="MG"):
    """files: [(name on the disk, bytes)], all of them programs"""
    img = bytearray(TRACKS * SECTORS * 256)
    free = {t: [True] * SECTORS for t in range(1, TRACKS + 1)}
    for s in range(4):
        free[DIR_TRACK][s] = False
    spare = [(t, s) for t in range(1, TRACKS + 1) if t != DIR_TRACK for s in range(SECTORS)]
    entries = []
    for name, data in files:
        chain = [spare.pop(0) for _ in range(max(1, -(-len(data) // 254)))]
        for i, (t, s) in enumerate(chain):
            free[t][s] = False
            part = data[i * 254:(i + 1) * 254]
            o = offset(t, s)
            if i + 1 < len(chain):
                img[o], img[o + 1] = chain[i + 1]
            else:
                img[o], img[o + 1] = 0, len(part) + 1
            img[o + 2:o + 2 + len(part)] = part
        entries.append((name, chain[0], len(chain)))
    if len(entries) > 8:
        sys.exit("more files than this makes a directory for")

    o = offset(DIR_TRACK, 0)                    # the header
    img[o:o + 4] = bytes([DIR_TRACK, 3, 0x44, 0])
    img[o + 4:o + 20] = padded(disk_name)
    img[o + 20:o + 29] = b"\xa0\xa0" + disk_id.encode("ascii") + b"\xa0" + b"3D" + b"\xa0\xa0"
    for k, first in ((1, 1), (2, 41)):          # the map of free sectors, 40 tracks a sector
        o = offset(DIR_TRACK, k)
        img[o:o + 8] = bytes([DIR_TRACK if k == 1 else 0, 2 if k == 1 else 0xFF, 0x44, 0xBB]) + \
            disk_id.encode("ascii") + bytes([0xC0, 0])
        for i in range(40):
            f = free[first + i]
            bits = sum(1 << b for b in range(SECTORS) if f[b])
            img[o + 16 + i * 6:o + 22 + i * 6] = bytes([sum(f)]) + bits.to_bytes(5, "little")
    o = offset(DIR_TRACK, 3)                    # the directory
    img[o], img[o + 1] = 0, 0xFF
    for i, (name, (t, s), blocks) in enumerate(entries):
        e = o + i * 32
        img[e + 2:e + 5] = bytes([0x82, t, s])   # a program, closed
        img[e + 5:e + 21] = padded(name)
        img[e + 30:e + 32] = blocks.to_bytes(2, "little")
    return bytes(img)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "MODPLAY.D81")
    src = sys.argv[2] if len(sys.argv) > 2 else HERE
    files = [("MODPLAY", open(os.path.join(src, "modplay.prg"), "rb").read())]
    files.append(("MODPLAY.OVL", open(os.path.join(src, "MODPLAY.OVL"), "rb").read()))
    with open(out, "wb") as f:
        f.write(make(files))
    blocks = sum(max(1, -(-len(d) // 254)) for _, d in files)
    print(f"{out}: {len(files)} files, {blocks} blocks of 3160")


if __name__ == "__main__":
    main()
