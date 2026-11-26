#!/usr/bin/env python3
"""
Makes MEGAGUS.D81: the examples of docs/megagus-programming.md and the test program on one
disk image, ready to mount on a MEGA65.

  ./makedisk.py [OUT.d81]

It wants the programs built first (the lines are at the top of tune.c and tone.asm, and
`make src/tests/megagus/megagustest.prg` for the test program) and the two BASIC listings
as program files, chord.prg and sample.prg: any BASIC 65 tokenizer makes those, MEGA65
Studio's with `m65rs basic tokenize chord.bas`.

On the disk:
  CHORD, SAMPLE   BASIC 65: in MEGA65 mode DLOAD "CHORD" and RUN
  KICK.RAW        the drum SAMPLE loads
  TONE            assembly: in MEGA65 mode DLOAD "TONE" and RUN
  TUNE            C: in C64 mode (GO64) LOAD "TUNE",8 and RUN
  MEGAGUSTEST     the test program: in C64 mode LOAD "MEGAGUSTEST",8 and RUN
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..", "..")
sys.path.insert(0, os.path.join(ROOT, "src", "utilities", "megagus"))
from maked81 import make  # noqa: E402

FILES = [
    ("CHORD", "chord.prg"),
    ("SAMPLE", "sample.prg"),
    ("KICK.RAW", "kick.raw"),
    ("TONE", "tone.prg"),
    ("TUNE", "tune.prg"),
    ("MEGAGUSTEST", os.path.join(ROOT, "src", "tests", "megagus", "megagustest.prg")),
]


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "MEGAGUS.D81")
    files = []
    for name, path in FILES:
        path = os.path.join(HERE, path)
        if not os.path.exists(path):
            sys.exit(f"{os.path.relpath(path)} is not there: see the top of this file")
        files.append((name, open(path, "rb").read()))
    with open(out, "wb") as f:
        f.write(make(files, disk_name="MEGAGUS", disk_id="MG"))
    blocks = sum(max(1, -(-len(d) // 254)) for _, d in files)
    print(f"{out}: {len(files)} files, {blocks} blocks of 3160")


if __name__ == "__main__":
    main()
