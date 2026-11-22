# MegaGus

A 32-voice wavetable sound engine in the MEGA65 core, in the spirit of the Gravis
Ultrasound: samples in the engine's own RAM, each voice with its own pitch, volume, panning
and looping, mixed in hardware with interpolation. A tracker's player sets registers on
its tick and the hardware does the rest, at no cost to the CPU.

It is a new design, not a model of the Ultrasound's GF1, and it isn't compatible with it.
What it keeps is the idea; what it changes:

- All 32 voices always play at the full output rate, 48 kHz. (A GF1 slows down as voices
  are added: 44.1 kHz up to 14 voices, 19.3 kHz at 32.)
- Every voice register is directly addressable: no voice-select and register-select ports.
- Volume is linear, 16 bits, as trackers compute it. Changes ramp over a millisecond or so,
  so they don't click.
- Addresses are plain byte addresses, for 16-bit samples too.
- A loop's end is interpolated toward its first sample by the hardware. (GF1 software has
  to copy that sample past the loop's end.)
- Sample RAM is in the MEGA65's address space: the DMA or an SD card load fills it directly.

## Status

- Built on mega65-core's `development` branch (`3c0c27fe`), for the MEGA65 R6 only
  (`megagus_ram_bits => 19` in `mega65r6.vhdl`; other boards build without it).
- Verified in simulation (see [Tests](#tests)) and built for the R6 (Vivado 2026.1,
  2026-11-22): all timing constraints met (worst slack 0.082 ns), 267 of 365 block RAMs
  (128 of them the sample RAM), 76 888 LUTs (57 %) in all. The engine's multiplier is one
  DSP block, its voice tables are LUT RAM.
- Not yet run on a MEGA65: nobody has heard it.
- No player software and no emulator support yet.

## Where it is

| Address               | What                                                       |
|-----------------------|------------------------------------------------------------|
| `$E000000`-`$E07FFFF` | Sample RAM, 512 KB                                         |
| `$E080000`-`$EFFEFFF` | Reserved for more sample RAM (reads 0)                     |
| `$EFFF000`-`$EFFF1FF` | Voice registers: voice *v* (0-31) at `$EFFF000 + v*16`     |
| `$EFFF200`-`$EFFF211` | Global registers                                           |

A sample address in a voice register is the low 24 bits of its CPU address: sample RAM
address `$000000` is `$E000000`.

It is on the slow device bus (as the attic RAM and the cartridge port are): an access takes
a handful of CPU clocks at 40 MHz. Reach it with 32-bit pointers (`LDA [zp],Z`), or with the
DMA and megabyte `$E0` as the destination to load samples.

The engine's audio joins the MEGA65 mixer's digital audio pair (the one the CPU's audio DMA
channels and `$D6F8`-`$D6FB` use) and is governed by that pair's mixer coefficients, after
its own master volume. The two sum, and clip rather than wrap.

## Voice registers

Sixteen bytes a voice. Multi-byte values are little-endian.

| Offset | Name  | Size | What                                                           |
|--------|-------|------|----------------------------------------------------------------|
| +0     | CTRL  | 1    | See below                                                      |
| +1     | PAN   | 1    | 0 left .. 128 centre .. 255 right                              |
| +2     | VOL   | 2    | 0 .. 65535, linear                                             |
| +4     | FREQ  | 3    | Samples per output frame, 8.16 fixed point                     |
| +7     | POS   | 3    | Position: byte address in sample RAM. Writing +9 commits it    |
| +10    | START | 3    | Loop start: byte address of the loop's first sample            |
| +13    | END   | 3    | End: byte address one past the last sample                     |

**CTRL**

| Bit | Name     | What                                                                    |
|-----|----------|-------------------------------------------------------------------------|
| 0   | RUN      | Write 1 to play, 0 to stop. Reads 1 while the voice plays               |
| 1   | LOOP     | At END go back to START (else stop there)                               |
| 2   | PINGPONG | With LOOP: turn round at END and at START instead                       |
| 3   | 16BIT    | Samples are 16 bits, little-endian, on even addresses (else 8 bits)     |
| 4   | BACKWARD | Play toward START. Reads the direction the voice is going               |
| 5   | UNSIGNED | Samples are unsigned (else two's complement)                            |
| 6   |          | Reserved, write 0                                                       |
| 7   | ENDED    | Read only: the voice played to its end and stopped                      |

**FREQ** is how far the position moves each output frame: `$010000` plays one sample a
frame, so a sample recorded at 48 kHz at its own pitch.

    FREQ = rate * 65536 / 48000          rate: the sample rate to play at, in Hz
    FREQ = 4842694 / period              an Amiga (PAL) period, as in a MOD
    FREQ = 11418                         8363 Hz, a MOD or S3M sample's C

The output rate is readable (global `$210`), should a build ever use another.

**POS** takes effect when its high byte (+9) is written, with the fraction cleared: write
low, middle, high. Reading +7 gives the low byte of the position now and holds the other two
for the reads of +8 and +9 that follow: read low, middle, high.

**Playing.** With RUN set the voice plays from POS in its direction, moving FREQ a frame,
its level going to VOL. Forward, it acts when the position reaches END; backward, when it
falls below START:

- no LOOP: it stops (RUN reads 0, ENDED reads 1), holding its last sample while its level
  ramps to nothing;
- LOOP: it continues from START (backward: from END);
- LOOP and PINGPONG: it turns round.

POS may start before START: a sample's attack plays once, then its loop. Between samples the
output is interpolated linearly; at the end of a forward loop toward the loop's first
sample, at any other end the last sample is held.

Writing RUN = 0 stops the voice where it is: POS keeps its place, the level ramps to
nothing, and RUN = 1 carries on from there. Any write to CTRL clears ENDED.

**Order of writes.** The engine takes a voice's CTRL and POS once a frame (48 000 times a
second), the other registers as they are when it gets to that voice. To start a note
cleanly: stop the voice or leave it, write PAN, VOL, FREQ, START, END, POS, and CTRL last.
A write to CTRL or POS is never lost, wherever in a frame it falls: the voice plays it from
the next frame on. A VOL, FREQ, START or END caught between two of its bytes plays that
one frame with the old and new bytes mixed; stop the voice first where that matters.

## Global registers

| Address    | Name    | R/W | Reset | What                                                    |
|------------|---------|-----|-------|---------------------------------------------------------|
| `$EFFF200` | ID      | R   | `$4D` | "M"                                                     |
| `$EFFF201` | ID      | R   | `$47` | "G"                                                     |
| `$EFFF202` | VERSION | R   | `$01` |                                                         |
| `$EFFF203` | RAMSIZE | R   | `$08` | Sample RAM in units of 64 KB                            |
| `$EFFF204` | CONTROL | R/W | `$01` | Bit 0: engine on. Bit 7 (write): reset the engine       |
| `$EFFF205` | MASTER  | R/W | `$20` | Master volume: `$40` is the sum as it is                |
| `$EFFF206` | RAMP    | R/W | `$40` | Volume ramp rate; 0: none, volumes change at once       |
| `$EFFF207` | VOICES  | R   | `$20` | 32                                                      |
| `$EFFF208` | RUNNING | R   | 0     | 4 bytes, a bit a voice (voice 0 is bit 0 of `$EFFF208`) |
| `$EFFF20C` | ENDED   | R/W | 0     | 4 bytes, a bit a voice; write 1 to clear                |
| `$EFFF210` | RATE    | R   | 48000 | 2 bytes: output frames a second                         |

To find it, read `$EFFF200` and `$EFFF201`: on a core without MegaGus they don't read
"MG".

**CONTROL.** With bit 0 clear the engine stops: every voice holds its position and the
output is silent, until it is set again. Writing bit 7 does what the reset line does: all
voices stop at once and CONTROL, MASTER and RAMP go back to their reset values. The voice
registers and the sample RAM keep what they hold through either.

**MASTER** scales the sum of all voices: `out = sum * MASTER / 64`, clipped to 16 bits. A
voice at full VOL with a full-scale sample, panned hard to one side, is full scale there
by itself, so at `$40` two such voices clip. `$20`, the default, has room for a four-channel
MOD; with more voices playing loud, set it lower.

**RAMP.** A voice's level moves toward its VOL (toward nothing, once it has stopped) by
`RAMP * 16` each frame: `$40` crosses the full range in 1.3 ms, `$01` in 85 ms, `$FF` in a
third of a millisecond. This is what keeps a volume change, a note's start and its stop from
clicking. It is one rate for all voices.

## Mixing

For each voice, each frame:

    sample = s0 + (s1 - s0) * fraction        s0, s1: this sample and the next, as 16 bits
    v      = sample * level / 65536
    left  += v * (255 - PAN) / 256
    right += v * PAN / 256

PAN is linear, so a voice in the centre is at half level on each side.

## A first sound

BASIC 65 (untested on hardware, as everything here): a 64-byte sine in the sample RAM,
looped by voice 0 at 440 Hz.

    10 M=$E000000:R=$EFFF000
    20 FOR I=0 TO 63:V=INT(127*SIN(I/32*3.14159)):IF V<0 THEN V=V+256
    30 POKE M+I,V:NEXT
    40 POKE R+1,128:POKE R+2,255:POKE R+3,255:REM PAN, VOL
    50 F=INT(440*64*65536/48000):REM A 64-SAMPLE WAVE, 440 TIMES A SECOND
    60 POKE R+4,F-256*INT(F/256):POKE R+5,INT(F/256):POKE R+6,0
    70 FOR I=7 TO 15:POKE R+I,0:NEXT:POKE R+13,64:REM POS 0, START 0, END 64
    80 POKE R+9,0:POKE R,3:REM COMMIT POS; RUN + LOOP
    90 GETKEY A$:POKE R,2:REM STOP

## Not there (yet)

- Interrupts. Poll RUNNING, ENDED or POS; time a player with a raster or CIA interrupt.
- More than 512 KB. The registers have room for 16 MB; the attic RAM would have to be
  shared with the CPU and be fast enough for 32 voices.
- Per-voice ramp rates, volume envelopes, filters (which IT modules want).

## Files

| File                                    | What                                              |
|-----------------------------------------|---------------------------------------------------|
| `src/vhdl/megagus.vhdl`                 | The engine and its sample RAM                     |
| `src/vhdl/slow_devices.vhdl`            | Where it sits on the bus (`megagus_ram_bits`)     |
| `src/vhdl/audio_complex.vhdl`           | Where its audio joins the mixer                   |
| `src/tests/megagus/megagus_test.py`     | A bit-exact model of the engine, and the tests    |
| `src/vhdl/test_megagus.vhdl`            | Testbench: the engine against the model           |
| `src/vhdl/test_megagus_bus.vhdl`        | Testbench: the engine in `slow_devices.vhdl`      |

## Tests

    python3 src/tests/megagus/megagus_test.py

needs [nvc](https://github.com/nickg/nvc) and Python 3. It runs a script of some 60 000
register and sample RAM accesses through the VHDL: every kind of voice (8 and 16 bits,
signed and unsigned, each loop mode, both directions), changes while they play, the ramp,
the master volume, clipping, both resets, then random accesses at random clocks, in the
middle of the engine's work on the same voice too. The model replays each access at the
clock edge the VHDL took it and has to agree on every one of 30 000 output frames and on
every byte read back. A second testbench reaches the engine through `slow_devices.vhdl` as
the CPU does.

`MegaGus` in that file is also the reference for an emulator: `frame()` is one output
sample, `write()` and `read()` are the bus.
