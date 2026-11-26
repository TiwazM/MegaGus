<p align="center"><img src="docs/megagus-logo.png" alt="MegaGus" width="640"></p>

# MegaGus

A 32-voice wavetable sound engine for the [MEGA65](https://mega65.org), in the spirit of
the Gravis Ultrasound: you load samples into its RAM, and 32 hardware voices play them at
any pitch, volume and place between left and right, mixed at 48 kHz, with no work for the
CPU. It is part of the MEGA65's FPGA core: this repository is the MEGA65 core
(`development`, `3c0c27fe`) with MegaGus added.

It comes with a module player that plays MOD, S3M and XM files from the SD card, those
with more than 512 KB of samples too: it makes the biggest samples smaller as it loads
them, and only as many as it has to.

- 32 voices, each with its own sample, pitch (24 bits), volume (16 bits) and panning
- 512 KB of sample RAM in the MEGA65's address space: the DMA or `BLOAD` fills it
- 8-bit and 16-bit samples, signed or unsigned
- loops forward and ping-pong, playing backward, an attack before the loop
- linear interpolation, and volume changes ramped so that notes neither start nor stop
  with a click
- a frame counter, 48 000 to the second, as a clock for a player
- sounds together with the SIDs, the OPL and the digital audio, through the MEGA65's mixer
- 2 211 LUTs (1.7 % of the R6's FPGA), 128 block RAMs (the sample RAM) and one DSP block

It is a new design, not a copy of the GUS's GF1 chip, and GUS software does not run on it:
the registers are laid out for the MEGA65, every voice directly addressable, with the
linear volumes trackers use.

## State

Release 1.0 (2026-11-26, engine version 3) is for the **MEGA65 R6** board; no other board
has been built, and no emulator has MegaGus yet. The engine is checked against a bit-exact
model in simulation; on an R6 the first version's output was read back and was the model's,
bit for bit, and the later ones were checked there with the test program and by recordings.
[docs/megagus.md](docs/megagus.md#status) has what was tested and how, and what is
[not there](docs/megagus.md#not-there-yet): interrupts, more sample RAM than 512 KB,
filters.

## The release

| File                           | What                                                     |
|--------------------------------|----------------------------------------------------------|
| `megagus-1.0-mega65r6.cor`     | The core, for a slot of the MEGA65's flash               |
| `megagus-1.0-mega65r6.bit`     | The same as a bitstream, to load over JTAG               |
| `MODPLAY.D81`                  | The module player                                        |
| `MEGAGUS.D81`                  | The programming examples and the test program            |

Everything on the two disks is for MEGA65 mode: `DLOAD` and `RUN`, no `GO64`.

The core is the MEGA65's own of that day with MegaGus in it: everything else works as on
a development core between releases 0.97 and the next.

## Installing

**The ROM.** The core runs with the ROM of release 0.97 (920413), the last stable one,
and nothing on the SD card has to change. Cores built from the MEGA65's `development`
branch otherwise stop at a blue screen with that ROM and want 920417 or later
([mega65-core#899](https://github.com/MEGA65/mega65-core/issues/899)); this one has the
CIA's timers as a real 6526 has them after a reset, which is all that ROM needs
([more](docs/megagus.md#status)).

**Into a flash slot.** As any MEGA65 core: copy the `.cor` file to the SD card, switch the
MEGA65 on with NO SCROLL held down, and in the core menu hold CTRL and press the number of
the slot to put it in. Use one from 2 to 7: slot 0 is the factory core, and slot 1 is where
the MEGA65 falls back to, so both are best left as they are. Then choose the slot to start
the core. The MEGA65 User's Guide describes the menu. (The tests named in the docs were
made with the same bitstream loaded over JTAG.)

**Over JTAG, without flashing.** With a JTAG adapter and MEGA65 Studio's `m65rs`:

    python3 src/tests/megagus/jtagboot.py megagus-1.0-mega65r6.bit

The core runs until the MEGA65 is switched off. (The script does more than load the
bitstream: left alone, a core loaded this way restarts the FPGA from flash slot 1.)

**Is it there?** In MEGA65 mode `PRINT PEEK($EFFF200);PEEK($EFFF201);PEEK($EFFF202)`
prints `77 71 3`: "MG" and the engine's version.

## Playing modules

Copy `MODPLAY.D81` and your modules (`*.mod`, `*.s3m`, `*.xm`) to the SD card: the modules
as files of the card itself, in any folder, not inside a disk image. Mount `MODPLAY.D81`,
then, in MEGA65 mode as the machine starts:

    RUN"*"

| Key                 | What                                                            |
|---------------------|-----------------------------------------------------------------|
| cursor keys, RETURN | choose a module or a folder; play it, open it                   |
| F1, F3, F5, F7      | the channels, the samples, the pattern as it plays, the keys    |
| SPACE               | pause                                                           |
| `+` and `-`         | the next and the previous position of the song                  |
| `,` and `.`         | quieter and louder                                              |
| `V`                 | a MOD's timing: by the tempo or by the vertical blank           |
| ESC                 | back to the list; in the list, leave                            |

It plays MODs of 4 to 32 channels, S3Ms (without AdLib instruments) and XMs, with their
effects, compared tick by tick against libopenmpt. Not IT, and not the old 15-sample MODs.

**Modules bigger than the sample RAM play too.** The file itself can be nearly 8 MB (it
goes into the attic RAM). When its samples are more than 512 KB, the player makes the
biggest ones smaller as it copies them, one at a time until all fit: a 16-bit sample is
kept as 8-bit, then a sample at half its rate, then at a quarter. The others stay as they
are, and short loops are left alone so that they stay in tune. A smaller sample sounds at
its pitch, a little duller; the samples screen (F3) marks which they are. On the R6
`clown.xm` (18 channels, 1 066 KB, 6 samples made smaller) and `alien.xm` (20 channels,
863 KB, 5 made smaller) play this way, and their recordings match what the model plays.

[More about the player](docs/megagus.md#module-player).

## Programming it

Everything is memory: sample RAM at `$E000000`, 16 bytes of registers a voice at
`$EFFF000 + 16 * voice`, a few global registers at `$EFFF200`. A tone in BASIC 65:

    10 POKE $EFFF204,129:POKE $EFFF204,1:REM RESET THE ENGINE, AND ON
    20 FOR I=0 TO 63:V=INT(127*SIN(I*3.14159265/32)):IF V<0 THEN V=V+256
    30 POKE $E000000+I,V:NEXT:REM ONE CYCLE OF A SINE IN THE SAMPLE RAM
    40 R=$EFFF000:POKE R,0:REM VOICE 0, STOPPED
    50 POKE R+1,128:WPOKE R+2,20000:REM PAN, VOLUME
    60 WPOKE R+4,38448:POKE R+6,0:REM FREQ: 440 HZ
    70 WPOKE R+10,0:POKE R+12,0:WPOKE R+13,64:POKE R+15,0:REM THE LOOP: 0 TO 64
    80 WPOKE R+7,0:POKE R+9,0:REM POSITION
    90 POKE R,3:REM RUN AND LOOP
    100 GETKEY A$:POKE R,0

| Offset | Register | Bytes | What                                                     |
|--------|----------|-------|----------------------------------------------------------|
| +0     | CTRL     | 1     | run, loop, ping-pong, 16-bit, backward, unsigned         |
| +1     | PAN      | 1     | 0 left, 128 middle, 255 right                            |
| +2     | VOL      | 2     | 0 to 65535, linear                                       |
| +4     | FREQ     | 3     | `rate * 65536 / 48000`                                   |
| +7     | POS      | 3     | where to play from                                       |
| +10    | START    | 3     | where a loop goes back to                                |
| +13    | END      | 3     | one past the last sample                                 |

- [Programming MegaGus](docs/megagus-programming.md): from an empty program to samples,
  notes, loops, fades and timing, in BASIC 65, C and assembly.
- [docs/megagus.md](docs/megagus.md): every register, how the engine mixes, the player,
  the tests, and the history of what was found on the way.
- `src/examples/megagus/`: the guide's examples. `MEGAGUS.D81` has them ready to run.

## Building

The core builds as mega65-core does, with Vivado (2026.1 was used):

    make bin/mega65r6.bit

MegaGus is `src/vhdl/megagus.vhdl`, switched on for a board by `megagus_ram_bits` in its
top file (`19`, for 512 KB, in `src/vhdl/mega65r6.vhdl`; boards without it build as before).

The C programs are built with [llvm-mos](https://github.com/llvm-mos/llvm-mos-sdk), as
MEGA65-mode programs (`LLVM_MOS`: where the SDK is unpacked; ACME builds the assembly
example):

    make LLVM_MOS=~/llvm-mos src/utilities/megagus/MODPLAY.D81
    make LLVM_MOS=~/llvm-mos src/tests/megagus/megagustest.prg src/examples/megagus/tune.prg
    python3 src/examples/megagus/makedisk.py

## Tests

    python3 src/tests/megagus/megagus_test.py            the VHDL against the model (needs nvc)
    python3 src/tests/megagus/megagustest_host.py        the test program against the model
    python3 src/utilities/megagus/modplay_host.py --selftest     the player against the model
    python3 src/utilities/megagus/modcompare.py SONG.XM  the player against libopenmpt

`MegaGus` in `megagus_test.py` is the model: under 300 lines of Python that give the
engine's output bit for bit, and the reference for an emulator.
On the MEGA65 itself `MEGAGUSTEST` on `MEGAGUS.D81` checks the engine and then plays nine
things to listen to (`DLOAD "MEGAGUSTEST"` and `RUN`). [More about the tests](docs/megagus.md#tests).

## Branches

`megagus` is this work. `development` is the MEGA65 core it was built on, unchanged.

## Licence and thanks

MegaGus is under the GNU Lesser General Public License, version 3, as the MEGA65 core is
([LICENSE.txt](LICENSE.txt)). It contains no code of the Gravis Ultrasound or of any
emulation of it; the name is a nod to it.

It stands on the MEGA65 core and its tools, the work of the MEGA65 team, and its player
was put right against [libopenmpt](https://lib.openmpt.org).

---

What follows is the MEGA65 core's own README.

# MEGA65 = Enhanced C65 running in FPGA.

Please refer to the [main documentation index](https://github.com/MEGA65/mega65-core/blob/master/docs/index.md)
for the index file of the main DOC documentation.  

You will optionally need the a MEGA65 ROM file, if you wish to use BASIC65, you can download diffs
against one original C65 ROM [here](https://files.mega65.org?id=MEGA65ROMdiff). If you are a
registered MEGA65 owner, you can also download [full ROMs](https://files.mega65.org?id=MEGA65ROM).

If you do not have such a file, the MEGA65 contains the free and open-source
OpenROM alternative.

# Downloads

Follow the following links to download the latest releases and development builds:

* [MEGA65R6 Release](https://files.mega65.org?id=CoreMEGA65R6norom)
* [MEGA65R3 Release](https://files.mega65.org?id=CoreMEGA65R3norom)
* [MEGA65R2 Release](https://files.mega65.org?id=CoreMEGA65R2)
* [Nexys4DDR Release](https://files.mega65.org/?id=CoreNEXYS4DDR)
* [Development Builds](https://builder.mega65.org/job/mega65-core/job/development/)

## Automated Build Server

The MEGA65 project maintains an automated build server called [builder](https://builder.mega65.org/).
Here you can find builds of core, tools, and manual.
