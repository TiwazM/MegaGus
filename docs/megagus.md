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
- Run on a MEGA65 R6 (2026-11-23, the bitstream loaded over JTAG): the 12 tests of the
  [test program](#test-program) pass and its nine sounds all play.
- The engine's output on that machine is the model's, bit for bit: read back through the
  audio mixer's `$D6FC`/`$D6FD` while all 32 voices played a chord started in one frame,
  16 384 output frames of each side had no difference from the model. The mixer's speaker
  outputs after it were the expected mix of the two sides, to within rounding, and far
  from clipping. What comes after the mixer (the DAC, HDMI) was not measured.
- What was heard as crackle came from how the test program played its sounds, and the
  model's recording had it too: voices restarted at full level, PAN jumped (it has no
  ramp), voices stopped without a fade, and the chord had C3, E3 and G3 as sines next to
  each other, which beat (in the model its level on the left fluttered by a quarter at
  66 Hz; spread wide, by about 2 %). The program now avoids each of these. On the machine
  (heard 2026-11-24, over its analogue audio output) the sounds are clean but for a faint
  trace still heard in the chord. The model's recording of the chord, which is what the
  engine puts out on the machine, does not have it when played on a computer: it arises
  after the mixer, in the core's audio output.
- The cause of that trace was in the core, not in MegaGus: the board's top level
  (`mega65r6.vhdl`, the `audio_counter` process) passed the mixer's output to the DAC's
  clock 47 943 times a second, not 48 000, because its counter did not count in the clock
  where it wraps. Engine and DAC both run at 48 000, so 57 times a second a frame of the
  engine's was skipped and one was sent twice. A model of that path puts a 57 Hz buzz on
  the chord, 24 to 35 dB below it depending on how the DAC's frames happen to lie against
  the engine's (which drifts over about half an hour); with the counter corrected the
  model's path is exact. The R6 top level now has it corrected (one line), and that core
  builds and meets timing. Heard on the machine (2026-11-24): the chord played five times
  on the corrected core was fine, and the uncorrected core, built from the same sources and
  heard right after, was clearly worse, in the chord and in the other sounds too. The other
  boards' top levels have the same counter and were left alone.
- A [MOD player](#mod-player) (2026-11-24). On this computer, against the model: its
  self-test passes (145 checks, the cc65 build in cc65's simulator writing the engine exactly
  what the native build writes), and the first minute of seven real modules with 4, 6 and 8
  channels plays without clipping, the two builds again alike on the two compared. On the R6
  the same day: it lists the SD card's modules, loads them (one read back from the sample
  RAM was the file, byte for byte; 306 KB take under three seconds) and plays at the right
  tempo; a tune made here, an 8-channel and a 4-channel module were heard and sounded right.
  Nobody has compared its playing with ProTracker's or another player's.
- No emulator support yet.

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

BASIC 65 (this listing has not been run): a 64-byte sine in the sample RAM,
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

## Test program

`src/tests/megagus/megagustest.prg` checks the engine on a real MEGA65 and then plays things
to listen to. Build it with

    make USE_LOCAL_CC65=1 src/tests/megagus/megagustest.prg

(or without `USE_LOCAL_CC65=1`, with the cc65 in the tree). It is a C64-mode program: with
the MegaGus core running, `GO64`, load it and `RUN`, or with the m65 tool
`m65 -4 -r src/tests/megagus/megagustest.prg`, or with MEGA65 Studio's `m65rs go64` and
`m65rs prg -r src/tests/megagus/megagustest.prg`.

To try the core without flashing it, `src/tests/megagus/jtagboot.py` loads `bin/mega65r6.bit`
over JTAG with `m65rs` and keeps it running. `m65rs jtag bit` alone is not enough: the core
has MEGAFLASH in it, which on a first boot restarts the FPGA from flash slot 1. The script
halts the CPU as the new core comes up, sets the Hypervisor's "not the first boot" flag and
resets. It also loads CIA 2's timer A with `$FFFF`: on a boot started this way the timer
still holds its power-on value of 1, and the ROM (920413) then waits on it forever before
BASIC starts. (Why a normal boot doesn't meet this has not been looked into.)

It first prints one line a test:

| Test                 | What it checks                                                       |
|----------------------|----------------------------------------------------------------------|
| reset                | Bit 7 of CONTROL: voices stop, the globals go back to their defaults |
| sample ram           | Every byte of the RAM, each 256-byte block with its own pattern      |
| voice registers      | Every register of voices 0, 15 and 31 reads back as written          |
| plays once and ends  | RUN clears, ENDED sets, POS stops on the last sample                 |
| backward             | The same toward START                                                |
| 16-bit               | The same in 16-bit samples                                           |
| loop                 | POS stays inside the loop and moves                                  |
| stop and go on       | RUN = 0 holds POS; RUN = 1 carries on                                |
| engine off and on    | Bit 0 of CONTROL holds every voice                                   |
| ping-pong            | POS stays inside the loop, going both ways                           |
| 48 khz               | 96 000 frames take two seconds of video frames                       |
| 32 voices            | All run at once                                                      |

A test that fails says what it read and what it expected (in hex). All of this is silent.
Then, after a key, nine things to hear, each announced on the screen: a 440 Hz tone in the
middle, left, right, moving from left to right, fading out and in, a scale, the tone from a
16-bit sample, four drum hits left and right, and a chord on all 32 voices (C major spread
over four octaves, each note four times). None of them should click, crackle or distort.
Afterwards any key but `Q` plays them again. (Typing `RUN` again does not work, as with any
cc65 program of this kind: load it again instead.)

What it should sound like is in `src/tests/megagus/work/megagustest.wav` after

    python3 src/tests/megagus/megagustest_host.py

which builds the same C source for this computer and runs it against the engine's model:
the tests pass there, and the sounds are checked to be what the screen says.

The program without MegaGus says so and stops ("no megagus in this core"). Its own far
memory routines can be tried in an emulator: built with `-DATTIC_SELFTEST` it runs its RAM
test on the attic RAM instead.

## MOD player

`src/utilities/megagus/modplay.prg` plays ProTracker-style modules from the SD card:

    make USE_LOCAL_CC65=1 src/utilities/megagus/modplay.prg

It is a C64-mode program, loaded and run like the test program. It lists the files named
`*.mod` or `mod.*` in the SD card's current directory (the first 44): cursor up and down,
RETURN plays, `Q` leaves. While a module plays, SPACE pauses, `+` and `-` skip to the next
and the previous position, `V` changes the timing (below), `Q` stops it. The screen shows
the module's title, its sample names (often its text: the first 17 whole) and where the
song is.

**What it plays.** Modules with 31 samples and 4 channels (`M.K.`, `M!K!`, `FLT4`, `4CHN`)
or more (`6CHN`, `8CHN`, `OCTA`, `CD81`, `xxCH`, up to 31), as big as the sample RAM
(512 KB). Not S3M, XM or IT, and not the old 15-sample modules.

**How.** The whole file is copied into the sample RAM as it is. The samples are played from
where they lie in the file, and each row of a pattern is read from there when its turn
comes, so the player keeps nothing but the 1084-byte header. A channel is a voice. To start
a note it stops the voice, writes PAN to END with one DMA copy and starts it: the engine
never sees a new position with an old end. Volume and pitch changes are written as they
happen. A sample's loop is the voice's START and END, with POS at the sample's beginning.

Voice 31 is the clock. It plays nothing (VOL 0) round the first 64 KB, one byte an output
frame, so the low 16 bits of its POS count frames at 48 kHz; a tick of the song is
`120000 / BPM` of them, kept to 1/256 of a frame. The tempo therefore doesn't depend on the
video mode or the CIA, and pausing the engine pauses the song.

The master volume is `128 / channels`: every channel at full level on one side cannot clip.
Channels are panned as on an Amiga (left, right, right, left) but less far apart, at 64
and 192.

**Commands.** 0 arpeggio, 1 and 2 slides, 3 tone portamento, 4 vibrato, 5, 6, 7 tremolo,
9 sample offset, A volume slide, B position jump, C volume, D pattern break, F speed and
tempo; E1 and E2 fine slides, E4 and E7 waveforms (sine, ramp, square), E5 finetune, E6
pattern loop, E9 retrigger, EA and EB fine volume, EC note cut, ED note delay, EE pattern
delay. 8 and E8 set the pan, but not in a ProTracker (`M.K.`, `M!K!`, `FLT4`) module, where
they are not commands. Not done: E0 (the Amiga's filter), E3 (glissando), EF (invert loop),
and the swap to another sample without a new note.

**Timing.** An F command below 32 is a speed (ticks a row), from 32 up a tempo (beats a
minute): ProTracker's CIA timing. Modules from before that mean a speed by every F, and
some say so in their sample names ("play with vblank"). `V` switches to that VBlank timing
and back: 50 ticks a second, whatever F says. It can't be told from the file, so a module
starts with CIA timing. The player keeps both readings of the song's F commands as it plays,
so `V` in the middle of a song gives the speed and tempo the other timing would have had
from the start. It only makes a difference in a module with an F of 32 or more.

Two things are not as ProTracker has them, neither by more than a period unit: a finetuned
note's period is the plain one times `2^(-finetune/96)`, not from its table, and an
arpeggio's notes are the period times `2^(-n/12)`. Slides stop at periods 113 and 856 in a
ProTracker module, at 28 and 3424 in the others.

**On this computer.** The same source runs here against the engine's model:

    python3 src/utilities/megagus/modplay_host.py FILE.mod       plays it into work/FILE.wav
    python3 src/utilities/megagus/modplay_host.py --selftest

(`--vblank` for VBlank timing.) The self-test makes four test modules (`makemod.py`: every
command in turn, each with what the engine must then have been told), plays them with the player built by this computer's
compiler and again with the cc65 build in cc65's simulator (`sim65`: 16-bit ints, as on the
MEGA65), checks all of it against the engine's registers at each tick, and wants both builds
to have written exactly the same to the engine. `makemod.py` also makes `work/demo.mod`,
half a minute of music of its own to listen to.

What that does not test: the parts only a MEGA65 has (the file list, loading from the SD
card, the screen and the keys), and whether the commands are understood as ProTracker
understands them where the test modules and the player share a misreading.

Reading the MEGA65's screen or memory through the serial monitor while a module plays stops
the CPU each time, and the song with it: the voices hold their notes meanwhile. After a
stop of more than a sixth of a second the player goes on from where it is and does not
hurry through the ticks it missed.

## Not there (yet)

- Interrupts. Poll RUNNING, ENDED or POS; time a player with a raster or CIA interrupt, or
  with a voice's POS as the [MOD player](#mod-player) does.
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
| `src/tests/megagus/megagustest.c`       | The test program for the MEGA65                   |
| `src/tests/megagus/megagustest_host.py` | The same program on this computer, with the model |
| `src/utilities/megagus/modplay.c`       | The MOD player                                    |
| `src/utilities/megagus/modplay_host.py` | The player on this computer, and its self-test    |
| `src/utilities/megagus/makemod.py`      | The test modules and a tune of its own            |

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
