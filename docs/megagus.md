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
- A [MOD player](#module-player) (2026-11-24). On this computer, against the model: its
  self-test passes (145 checks, the cc65 build in cc65's simulator writing the engine exactly
  what the native build writes), and the first minute of seven real modules with 4, 6 and 8
  channels plays without clipping, the two builds again alike on the two compared. On the R6
  the same day: it lists the SD card's modules, loads them (one read back from the sample
  RAM was the file, byte for byte; 306 KB take under three seconds) and plays at the right
  tempo; a tune made here, an 8-channel and a 4-channel module were heard and sounded right.
- The player [against libopenmpt](#module-player) (2026-11-24, on this computer): over the
  first minute of nine modules every row is the same row and as long, and all but 32 of
  111 476 ticks agree in pitch (within 5 cents) and level (within 0.5 dB). It found six
  things in the player, which are fixed; one row is unexplained.
- The module player plays S3M as well, keeps the file in the attic RAM and makes samples
  smaller when they are more than the sample RAM holds (2026-11-25). On this computer: the
  self-test passes, and over the first 40 seconds of ten real S3Ms with 4 to 16 channels
  every one of 4 755 rows is the same row and as long as in libopenmpt, and 106 525 of
  106 719 ticks agree in pitch and level; what differs is nearly all the level of single
  ticks in steep volume slides (see [there](#module-player)).
- The engine has a frame counter, FRAMES at `$EFFF214` (2026-11-25, VERSION 2): the VHDL and
  the model agree on it, and the R6 core with it builds and meets timing (WNS +0.113 ns).
  The player keeps time by it, which leaves all 32 voices for channels.
- On the R6, with that core and that player (2026-11-25): FRAMES went up by 175 929 in 3.67
  s. Four S3Ms and three MODs were played and recorded over HDMI, and every second of each
  recording has the spectrum and the level of what the model plays for the same module
  (`hwcompare.py`): 16 channels and 549 KB among them, one saved by Impulse Tracker, and a
  module with a megabyte of samples of which two were made smaller on the machine and still
  sound at 440.2 Hz. Heard by nobody yet: by a capture card.
- The module player plays XM (FastTracker 2) too, and has screens of its own in 80 columns:
  the SD card's modules with its folders, and while a module plays its channels with a bar
  for each one's level, its samples, and its pattern (2026-11-25). With XM the program is
  more than C64-mode memory holds at once, and is now five files: `modplay.prg` and four
  overlays that it reads from the SD card. On this computer: the self-test passes with XM
  in it, and over the first 40 seconds of four real XMs with 8 to 20 channels every one of
  1 933 rows is the same row and as long as in libopenmpt, and 56 889 of
  56 991 ticks agree in pitch and level (see [there](#module-player)); MOD and S3M
  compare as they did before. The screens were tried in the Xemu emulator with the engine
  left out. **Not run on a MEGA65 yet**: no XM has been heard, and how long a tick of
  twenty XM channels takes the real CPU is not known.
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
| `$EFFF202` | VERSION | R   | `$02` | 2: with FRAMES                                          |
| `$EFFF203` | RAMSIZE | R   | `$08` | Sample RAM in units of 64 KB                            |
| `$EFFF204` | CONTROL | R/W | `$01` | Bit 0: engine on. Bit 7 (write): reset the engine       |
| `$EFFF205` | MASTER  | R/W | `$20` | Master volume: `$40` is the sum as it is                |
| `$EFFF206` | RAMP    | R/W | `$40` | Volume ramp rate; 0: none, volumes change at once       |
| `$EFFF207` | VOICES  | R   | `$20` | 32                                                      |
| `$EFFF208` | RUNNING | R   | 0     | 4 bytes, a bit a voice (voice 0 is bit 0 of `$EFFF208`) |
| `$EFFF20C` | ENDED   | R/W | 0     | 4 bytes, a bit a voice; write 1 to clear                |
| `$EFFF210` | RATE    | R   | 48000 | 2 bytes: output frames a second                         |
| `$EFFF214` | FRAMES  | R/W | 0     | 4 bytes: frames played; a write to `$EFFF214` clears it |

To find it, read `$EFFF200` and `$EFFF201`: on a core without MegaGus they don't read
"MG".

**CONTROL.** With bit 0 clear the engine stops: every voice holds its position and the
output is silent, until it is set again. Writing bit 7 does what the reset line does: all
voices stop at once and CONTROL, MASTER and RAMP go back to their reset values. The voice
registers and the sample RAM keep what they hold through either.

**FRAMES** is the engine's own timer: it goes up by one with every output frame, 48 000
times a second, in step with the sound, and so is a clock for whatever plays music on the
engine (the [module player](#module-player) counts its ticks off it). It stands still
while the engine is switched off (CONTROL bit 0), as the voices do, so a paused song stays
where it is. A read of `$EFFF214` returns the lowest byte and holds the other three for the
reads of `$EFFF215` to `$EFFF217` that follow, so the four belong to the same moment; one
or two bytes are enough for most (two go round every 1.365 s). A write of any value to
`$EFFF214` starts it from 0, and so does a reset. It is there from VERSION 2; before that a
silent voice had to be the clock (a voice with VOL 0 and FREQ `$010000` moves its POS a
byte a frame).

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

[Programming MegaGus](megagus-programming.md) goes from an empty program to notes, loops,
fades and timing, in BASIC 65, C and assembly, with examples in `src/examples/megagus/`.

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

## Module player

`src/utilities/megagus/modplay.prg` plays ProTracker-style modules, Scream Tracker 3
modules (S3M) and FastTracker 2 modules (XM) from the SD card:

    make USE_LOCAL_CC65=1 src/utilities/megagus/modplay.prg

That makes five files, which all go into the same directory of the SD card: `modplay.prg`
and `MODPLAY.OV1` to `MODPLAY.OV4`. It is a C64-mode program, loaded and run like the test
program. It reads the four overlays when it starts, and says so if one is missing or is
another build's.

It shows the modules (`*.mod`, `mod.*`, `*.s3m`, `*.xm`) and the folders of the SD card's
current directory, up to 250 of them, in 80 columns: cursor up and down choose (left and
right go a page), RETURN plays a module or opens a folder, ESC leaves. While a module plays
there are four screens, on F1, F3, F5 and F7:

- the channels: each one's note and instrument, a bar for how loud it is, and where it is
  between left and right;
- the samples (an XM's instruments): name, size, 8 or 16 bits, loop, whether it was made
  smaller to fit, and a mark at those that are sounding. The names are often the module's
  text;
- the pattern as it plays, five channels of it (cursor left and right for the others);
- the keys.

SPACE pauses, `+` and `-` go to the next and the previous position, `,` and `.` make it
quieter and louder, `V` changes a MOD's timing (below), ESC goes back to the list. Above
the screens are the song's name and kind, and its position, pattern, row, speed, tempo and
the time played.

**What it plays.** MODs with 31 samples and 4 channels (`M.K.`, `M!K!`, `FLT4`, `4CHN`) or
more (`6CHN`, `8CHN`, `OCTA`, `CD81`, `xxCH`, up to 32); S3Ms with up to 32 channels and up
to 99 samples; XMs (version 1.04) with up to 32 channels, 128 instruments and 99 samples in
all. The file can be nearly as big as the attic RAM (8 MB).
Not IT, not the old 15-sample modules, and of an S3M not the AdLib instruments.

**How.** The whole file is copied into the MEGA65's attic RAM (`$8000000`) as it is, and the
samples from there into the sample RAM, one after the other. Each row of a pattern is read
from the attic RAM when its turn comes, so the player itself keeps little more than the
header. A channel is a voice. To start a note it stops the voice, writes PAN to END with one
DMA copy and starts it: the engine never sees a new position with an old end. Volume and
pitch changes are written as they happen. A sample's loop is the voice's START and END, with
POS at the sample's beginning. An S3M's rows are packed, each as long as what is in it: the
player notes where each row of the playing pattern began as it comes to it, and for a jump
into rows it has not seen it reads its way there.

**In parts.** With XM the program came to some 60 KB, and a C64-mode program has 50. So it
is a main part and four overlays, which take turns in two places in memory
(`modplay.cfg` is the plan of it): the file list or the playing screens in one, the player
of a MOD and an S3M or that of an XM in the other. All four are read from the SD card once,
when the program starts, and kept in the MEGA65's bank 5: putting one in place is then a
copy of a few kilobytes, and a change of folder does not lose them. Each begins with the
number of its build, and the program does not take another build's.

The screen is the VIC-IV's 80 columns with its memory in bank 4, written a row at a time
with the DMA. While a module plays, one row of a screen is drawn between two looks at the
clock, so that no tick waits for the screen. The keys come from the MEGA65's keyboard
register (`$D610`).

**More samples than sample RAM.** When a module's samples are more than the 512 KB, the
biggest are made smaller as they are copied, until all fit: a 16-bit sample is kept as
8-bit; when there are no more of those, a sample is kept at half its rate (two frames
averaged into one), and when all are, at a quarter. A sample with a loop shorter than 512
frames is left alone, since halving that would put it out of tune. The voice plays a
smaller sample at half or a quarter of the speed, so it sounds at its pitch, duller. The
screen says how many were made smaller. (XTC-Play does the like on a GUS with 512 KB.)

The clock is the engine's frame counter (FRAMES): the player reads its low 16 bits, and a
tick of the song is `120000 / BPM` frames, kept to 1/256 of a frame. The tempo therefore
doesn't depend on the video mode or the CIA, and pausing the engine pauses the song. All 32
voices are free for channels. On an engine of version 1, which has no counter, voice 31 is
the clock instead (it plays nothing, VOL 0, round the first 64 KB, a byte a frame, and its
POS is read) and a module can have 31 channels.

The master volume of a MOD is `128 / channels`: every channel at full level on one side
cannot clip. Its channels are panned as on an Amiga (left, right, right, left) but less far
apart, at 64 and 192. An S3M with many channels never has them all at full level, and would
be very quiet by that rule: its master volume is `96 / channels + 8`. Its channels are left
and right as the file says (at `$33` and `$CC`, or by its table of pans; all in the middle
if it is a mono module).

**A MOD's commands.** 0 arpeggio, 1 and 2 slides, 3 tone portamento, 4 vibrato, 5, 6, 7 tremolo,
9 sample offset, A volume slide, B position jump, C volume, D pattern break, F speed and
tempo; E1 and E2 fine slides, E4 and E7 waveforms (sine, ramp, square), E5 finetune, E6
pattern loop, E9 retrigger, EA and EB fine volume, EC note cut, ED note delay, EE pattern
delay. 8 and E8 set the pan, but not in a ProTracker (`M.K.`, `M!K!`, `FLT4`) module, where
they are not commands. Not done: E0 (the Amiga's filter), E3 (glissando), EF (invert loop),
and the swap to another sample without a new note (but a sample named with a tone
portamento does start if the voice is silent).

**An S3M's commands.** A speed, B position jump, C pattern break, D volume slide (with its
fine forms `DxF` and `DFx`), E and F slides (fine `EFx`, extra fine `EEx`), G tone
portamento, H vibrato, I tremor, J arpeggio, K and L (a volume slide with the vibrato or the
portamento going on), O sample offset, Q retrigger with its volume change, R tremolo, T
tempo, U fine vibrato, V the song's volume, X pan; S2 a rate from the finetune table, S3 and
S4 waveforms, S8 pan, SB pattern loop, SC note cut, SD note delay, SE pattern delay; a
volume or a pan in the volume column, `^^^` to cut a note; the `+++` and `---` markers in
the list of positions. Channels the patterns never use get no voice (modules often declare
all 32 and play on eight). Not done: AdLib instruments, the right half of a stereo sample,
S1 (glissando), SA, SF.

How an S3M is played depends on what made it, and the player follows libopenmpt in that
(read off it with small modules made for the purpose):

- Saved by Impulse Tracker: notes are tuned exactly; each kind of command remembers its own
  last argument (volume slides, pitch slides, tremor, arpeggio, retrigger and the S commands
  one each); a sample number with no note starts that sample at once, at the pitch the
  channel is at.
- Saved by Scream Tracker 3: notes come from its table of twelve periods (up to 4 cents from
  exact); D, E, F, I, J, K, L, Q, R and S share one remembered argument; version 3.00 slides
  the volume on a row's first tick too. A sample number with no note only sets the volume,
  as on a GUS, but in 3.00's modules and in 3.20's made with a Sound Blaster it starts the
  sample, as above.
- In all of them a tempo counts from the tick that sets it, a vibrato and a tremolo sound
  on a row's first tick where they stand, the tremolo is half as deep as a MOD's, and the
  ramp waveform goes the other way.

**An XM.** Instruments with up to 16 samples each (the note says which one plays), their
volume and panning envelopes with sustain point and loop, the fade after the key goes up,
and the instrument's own vibrato (sine, square, the two ramps, growing in if it has a
sweep); 8-bit and 16-bit samples, looped forward or back and forth, each with its own
volume, place between the sides, finetune and transposition; linear periods and an Amiga's;
patterns of 1 to 256 rows. An instrument's settings and envelopes stay where they are, in
the file in the attic RAM. The player keeps a table of where each instrument and pattern
is (behind the file, in the attic RAM too), and for a channel only the stretch of the
envelope it is on: its value and what a tick adds. When the envelope comes to its next
point, that is fetched anew.

Its commands: 0 arpeggio, 1 and 2 slides, 3 tone portamento, 4 vibrato, 5, 6, 7 tremolo,
8 pan, 9 sample offset, A volume slide, B position jump, C volume, D pattern break, F speed
and tempo, G the song's volume and H its slide, K key off, L envelope position, P pan slide,
R retrigger with its volume change, T tremor, X1 and X2 extra fine slides; E1, E2, E4 to
E7, E9 and EA to EE as in a MOD; in the volume column a volume, volume slides and fine
slides, vibrato speed and vibrato, pan and pan slides, tone portamento. Not done: E3
(glissando).

An XM is played as FastTracker 2 plays it, mistakes included, since modules were made by
ear with those. The player takes that from libopenmpt, measured with modules made for the
purpose:

- finetune moves in sixteenths of a semitone;
- a side gets the square root of its share, where the engine's PAN is the share itself:
  the player moves PAN towards the middle and raises the level until both sides get what
  FastTracker gives them;
- which step of an arpeggio sounds depends on the speed, because FastTracker counts a
  row's ticks down;
- where an envelope's loop ends on its sustain point, the loop goes round while the key is
  down;
- the instrument's vibrato, while it is still growing in, all but stops when the key goes
  up;
- L moves the panning envelope only if the volume envelope has a sustain point;
- a ramp tremolo turns over where the vibrato is in its second half;
- R counts its ticks across rows and past notes, begins at one on a row with an instrument
  number, and passes over a row's first tick when the volume column sets a volume;
- ED with no note plays the channel's note again, and its row does nothing on its first
  tick;
- after a pattern loop (E6) the next pattern begins at the row the loop went back to,
  unless a break names another;
- a sample offset past the end plays nothing, loop or not.

In two things libopenmpt may not be FastTracker, and the player is as libopenmpt: a slide to
a note on a voice that has stopped (its sample played out) starts the sample again, and a
vibrato or a tremolo sounds on a row's first tick where it stands. One thing is not copied:
FastTracker, and libopenmpt after it, glide to a new volume over a whole tick, and the
engine is there in a millisecond.

An XM's voice in the middle is at 0.71 of full level on each side, so the master volume is
`181 / channels` up to four channels, where nothing can clip, and 34 for more: at that the
loudest of the four real XMs tried here peaks at 0.93 of full scale.

**A MOD's timing.** An F command below 32 is a speed (ticks a row), from 32 up a tempo (beats a
minute): ProTracker's CIA timing. Modules from before that mean a speed by every F, and
some say so in their sample names ("play with vblank"). `V` switches to that VBlank timing
and back: 50 ticks a second, whatever F says. It can't be told from the file, so a module
starts with CIA timing. The player keeps both readings of the song's F commands as it plays,
so `V` in the middle of a song gives the speed and tempo the other timing would have had
from the start. It only makes a difference in a module with an F of 32 or more.

**A MOD's pitch.** Notes are tuned by ProTracker's own table (its periods for the octave C-1 to B-1
at each finetune; the other octaves are halves and doubles of those, at finetune 0
ProTracker's own). An arpeggio's notes come from the same table, and one that reaches above
B-3 in a ProTracker module wraps to the table's start as it does there. Periods are kept in
quarters, so a vibrato is four times finer than an Amiga's. Slides stop at periods 113 and
856 in a ProTracker module, at 28 and 3424 in the others. In a ProTracker module a vibrato
row's first tick is at the plain pitch; in the others the vibrato sounds there too. A tone
portamento goes to the last note given with 3 or 5 and stops for good once it is there. A
new tempo counts from the tick after the one that sets it, but on the song's first row.

**On this computer.** The same source runs here against the engine's model:

    python3 src/utilities/megagus/modplay_host.py FILE.mod       plays it into work/FILE.wav
    python3 src/utilities/megagus/modplay_host.py --selftest

(`--vblank` for VBlank timing; the attic RAM is the harness's.) The self-test makes four
test MODs (`makemod.py`: every command in turn, each with what the engine must then have
been told), plays them with the player built by this computer's compiler and again with the
cc65 build in cc65's simulator (`sim65`: 16-bit ints, as on the MEGA65), checks all of it
against the engine's registers at each tick, and wants both builds to have written exactly
the same to the engine. `makemod.py` also makes `work/demo.mod`, half a minute of music of
its own to listen to.

For S3M it makes three test modules (`makes3m.py`: the commands in turn, once as Scream
Tracker 3.20 would save it, once as Impulse Tracker and once as Scream Tracker 3.00) and
wants the two builds to write the same for each; what the commands should do is the
comparison's to say (below). A fourth, `big.s3m`, has a megabyte of samples: the player has
to make its 16-bit sample 8-bit and halve another, and each must still sound at 440 Hz; a
fifth is the same with signed samples, and the two builds must sound exactly alike on it.
(These tests found a cc65 build that panned wrongly where the host's build did not.) Last,
a MOD and an S3M are played with the clock an engine of version 1 has, a silent voice
(`-k`), and must come out as the same song.

For XM it makes `test.xm` (`makexm.py`, on `xmfile.py`, which reads and writes XMs): 13
instruments (envelopes with sustain points and loops, fades, each kind of own vibrato, two
samples in one instrument, a 16-bit sample looped back and forth) and every command in
turn on four channels, in patterns of 64, 37, 16 and 8 rows with a loop, a break and a jump;
and `test_amiga.xm`, the same song with an Amiga's periods. Both builds must write the same
for each. `big.xm` has 700 KB of samples, one of them 16-bit, stored as an XM stores them
(each value as its difference from the one before): the builds must sound exactly alike on
it, two samples must have been made smaller, and its notes be at 440.2, 440.2 and 130.7 Hz.

What that does not test: the parts only a MEGA65 has (the file list, loading from the SD
card, the screens and the keys, the overlays), and whether the commands are understood as
their trackers understand them where the test modules and the player share a misreading.
For the first there is an emulator, for the second another player.

**The screens in an emulator.** Xemu has no MegaGus, so the program can be built without
one: with `-DNOGUS` it takes the engine as there and lets its clock go on by itself, and
with `-DKEYS=...` it presses those keys by itself and then tells Xemu to stop, which saves a
picture of the screen (Xemu's `-headless -testing -screenshot`, and `-hdosvirt` to give it
a folder of this computer as the SD card). The file list, a folder, and the playing screens
for a MOD, an S3M and an XM were looked at that way, and an 862 KB XM whose samples had to
be made smaller. The overlays are read and put in place there as on the machine. Xemu also
has the MEGA65's multiplier (`$D770`), which the player uses for an XM's levels after
trying it once: it gave the right product.

A tick was timed there as well (`-DTICKTIME`: by the CIA's timer, with the raster agreeing):
3.5 ms on average and 7 ms at most for an XM of 20 channels, 1.2 and 2.6 ms for one of 16,
0.7 ms for a MOD of 6 channels (8 ms where every channel has a note: finding a note's place
among the periods is slow). Ticks are 15 to 20 ms apart. Xemu's clock is not the real
CPU's, so this says how the cost is spread and roughly how big it is, no more; and an
emulator cannot say how any of it sounds.

**Against another player.** `modcompare.py` plays a module with modplay (on the model) and
with [libopenmpt](https://lib.openmpt.org/) (`brew install libopenmpt`), which is the
reference for how modules sound:

    python3 src/utilities/megagus/modcompare.py FILE.mod [--seconds 60] [--waves]

It compares the timeline (the same rows in the same order, each as long) and then every
channel by itself: the module is written out with every sample made a pure tone and the
other channels' notes removed, both players play it, and for every tick of every row the
pitch and the level are read off the two recordings. A tick agrees within 5 cents and
0.5 dB. Where they don't, the row and its command are listed. (`--waves` also lays the
waveforms of the module's own samples over each other, which shows sample offsets and
loops, but drops out at the slightest difference in pitch.)

The first minute of nine modules (2026-11-24, libopenmpt 0.8.10): the two test modules
of the self-test and seven real ones with 4, 6 and 8 channels. All 5 863 rows are the same
row and as long in both players. Of 111 476 sounding ticks 111 444 agree. Of the 32 that
don't, 25 differ in level only, on a tick where the volume changes (see below), and 7 are
one row of one module, a tone portamento with no note that libopenmpt slides by 15 cents
and modplay leaves where it is; why was not found.

(Run again on 2026-11-25, with the file in the attic RAM and the engine's frame counter as
the clock: the same 111 444 of 111 476.)

The comparison is what made the player's pitch what it is now. Before it, a finetuned note
could be 4 cents off and an arpeggio's notes 6 (computed, not from ProTracker's table),
a vibrato had a quarter of the resolution, an arpeggio above B-3 played a wrong note, a
tone portamento could slide when it should not, and a tempo change came one tick early.

**S3Ms against libopenmpt** (2026-11-25, the first 40 seconds, 5 cents and 0.5 dB). The
three test modules of `makes3m.py`: 3 875 of 3 875 ticks as Scream Tracker 3.20 saves it,
3 884 of 3 887 as 3.00, 3 881 of 3 887 as Impulse Tracker. Ten real modules:

| Module        | Made with        | Channels | Ticks that agree   |
|---------------|------------------|----------|--------------------|
| sun.s3m       | Scream Tracker   | 4        | 7 650 of 7 651     |
| RCDROPS.S3M   | Scream Tracker   | 8        | 11 003 of 11 013   |
| anamnesis.s3m | Scream Tracker   | 6        | 6 968 of 6 976     |
| abfi.s3m      | Scream Tracker   | 14       | 18 398 of 18 398   |
| monolife.s3m  | Scream Tracker   | 9        | 10 483 of 10 502   |
| ZEN.S3M       | Scream Tracker   | 16       | 13 973 of 13 973   |
| DELERIUM.S3M  | Impulse Tracker  | 4        | 8 292 of 8 323     |
| ERR5000.S3M   | Impulse Tracker  | 8        | 4 249 of 4 288     |
| NUKKU.S3M     | Impulse Tracker  | 12       | 4 805 of 4 849     |
| CHEESE.S3M    | Impulse Tracker  | 14       | 20 704 of 20 746   |

All 4 755 rows are the same row and as long. What is left:

- Volume slides (D, K), 148 ticks, level only. In an S3M libopenmpt glides from one tick's
  volume to the next across the whole tick, as a GUS's volume ramp does; the engine is
  there in a millisecond. The comparison allows for the glide (it compares the reference
  with where it would be if it were heading for modplay's level), and what remains are
  steep slides at low volumes where that reckoning is off by more than half a dB.
- Long pitch slides in Impulse Tracker's modules (E, F), 39 ticks in ERR5000 and 4 in the
  test: libopenmpt keeps the pitch of those in whole Hertz and loses a fraction of a period
  on every tick of a slide, up to 6 periods after twenty ticks in the test; modplay slides
  by exactly the periods the command says.
- A sample number with no note, 7 ticks in DELERIUM and 3 in the tests: the tick the new
  sample starts on, where libopenmpt fades the old one out and the pitch cannot be read.

(Read again on 2026-11-25 with the player in overlays and the comparison taking a pitch from
two cycles where a tick has no three: 106 522 of the 106 719, CHEESE two fewer and RCDROPS
one, and the MODs 111 444 of 111 476 as before.)

**XMs against libopenmpt** (2026-11-25, the first 40 seconds, 5 cents and 0.5 dB). The two
test modules of `makexm.py`: 2 990 of 3 000 ticks with linear periods, 2 991 with an Amiga's
(what differs is a tremolo's level on five ticks, and single ticks where a note begins).
Four real modules, all made with FastTracker 2:

| Module          | Channels | Ticks that agree       |
|-----------------|----------|------------------------|
| PAGER11.XM      | 8        | 12 520 of 12 587       |
| kill.xm         | 16       | 3 386 of 3 386         |
| alien.xm        | 20       | 26 579 of 26 613       |
| clown.xm        | 18       | 14 404 of 14 405       |

All 1 933 rows are the same row and as long. Most of the 102 ticks that differ are
in PAGER11.XM, in one instrument whose volume envelope, panning envelope and own vibrato all
move at once: single ticks a dB or so apart, and the tick on which a note that is sounding
is started again from an envelope that begins at nothing, where libopenmpt takes the tick
to fade the old note out and modplay is silent at once.

Two allowances are in those numbers. libopenmpt glides to a tick's volume across the whole
tick, as FastTracker does, and the comparison allows for that as it does for an S3M's
slides. And libopenmpt's levels of an XM move in steps, as if its envelopes had 256 values
and were rounded down, so the last ticks of a sound that dies away are a step or two lower
there than the straight line modplay follows: a quiet tick may be two such steps off. With one step allowed and no more, kill.xm, whose sounds
are all short and die away, has 3 096 of its 3 386 ticks agree and alien.xm 25 876.

A thing to know about libopenmpt and XMs: it decides from the layout of the file whether
FastTracker wrote it, and plays a file that looks otherwise by other rules (a straight-line
pan law, for one). The comparison writes its one-channel copies of a module as FastTracker
would (an instrument with no samples as 33 bytes); before it did, every level was off.

An S3M's overall level is the player's choice (libopenmpt takes the file's mixing volume,
modplay the number of voices), so levels are compared against each channel's usual
difference. Left and right were compared on the test module: the share on the right is
the same to three decimals for the file's own pans and for the X, S8 and volume-column
pans.

What the comparison cannot settle, because the two players differ on purpose: libopenmpt
makes a tick a whole number of output frames (705 for the 705.88 of tempo 170), modplay
keeps the fraction, as an Amiga's timer nearly does, so at such tempos the two drift apart
by up to a tenth of a percent; libopenmpt glides to a new volume over most of a tick, the
engine in a millisecond; and libopenmpt's highest note is period 113.25, ProTracker's and
modplay's 113.

**On the machine** (2026-11-25, R6, the core with FRAMES, by a USB capture card on HDMI
and `m65rs live record`). `hwcompare.py RECORDING.wav MODEL.wav` lays a recording over what
the model plays: second by second, how alike the spectra are (1 is the same; two different
songs gave 0.44 and 0.50), the level, and the share on the right. Not sample by sample:
the player on the machine looks at its clock a fraction of a millisecond earlier or later
than the one on the model, and the capture card is not exact either (it adds a few units
of noise and samples by its own clock, some 22 parts in a million off the MEGA65's, as
MEGA65 Studio's own check of it found).

| Module        | What it shows                          | Spectrum, median (lowest) | Level  |
|---------------|----------------------------------------|---------------------------|--------|
| SUN.S3M       | an S3M, mono                           | 0.969 (0.952)             | 0.8 dB |
| ZEN.S3M       | 16 channels, 549 KB, stereo            | 0.944 (0.913)             | 0.9 dB |
| DELERIUM.S3M  | saved by Impulse Tracker               | 0.974 (0.964)             | 0.2 dB |
| ROYAL.MOD     | a MOD, 6 channels, tempo 170           | 0.998 (0.971)             | 1.0 dB |
| FINKEL.MOD    | a MOD, 4 channels                      | 0.943 (0.844)             | 0.8 dB |
| DEMO.MOD      | a ProTracker MOD                       | 0.959 (0.937)             | 0.2 dB |

`big.s3m`, the megabyte of samples: the screen says "2 smaller", and the four notes are at
440.2, 440.2, 130.7 and 440.2 Hz in the recording, as in the model and in libopenmpt. It
loads in about 8 seconds, most of it reading the SD card; the making smaller took over
half a minute before its two common cases were written without 32-bit sums. The attic RAM
and the sample RAM read back from the machine after loading ROYAL.MOD were the file and
its samples, byte for byte. Loading an 18 KB module takes 0.2 s.

Two things about the machine that are not MegaGus's:

- Over HDMI the MEGA65's mixer gives each side a third of the other: a voice panned hard
  left arrives at 1.00 left and 0.34 right. A module is that much narrower there than the
  engine makes it (ZEN's share on the right: 0.37 in the model, 0.44 recorded).
- With "DVI" set (`$D61A` bit 1, as this machine's configuration has it) HDMI carries no
  sound at all. For the recordings the bit was cleared until the next reset.

Reading the MEGA65's screen or memory through the serial monitor while a module plays stops
the CPU each time, and the song with it: the voices hold their notes meanwhile. After a
stop of more than a sixth of a second the player goes on from where it is and does not
hurry through the ticks it missed.

## Not there (yet)

- Interrupts. Poll RUNNING, ENDED, POS or FRAMES; time a player with a raster or CIA
  interrupt, or off FRAMES as the [module player](#module-player) does.
- More than 512 KB. The registers have room for 16 MB; the attic RAM would have to be
  shared with the CPU and be fast enough for 32 voices.
- Per-voice ramp rates and filters (which IT modules want). Envelopes: an XM's are the
  player's work, a tick at a time.

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
| `src/utilities/megagus/modplay.c`       | The module player (MOD, S3M and XM)               |
| `src/utilities/megagus/modplay.cfg`     | Its plan of memory: the program and its overlays  |
| `src/utilities/megagus/modplay_host.py` | The player on this computer, and its self-test    |
| `src/utilities/megagus/makemod.py`      | The test MODs and a tune of its own               |
| `src/utilities/megagus/makes3m.py`      | The test S3Ms, one too big for the sample RAM     |
| `src/utilities/megagus/makexm.py`       | The test XMs, one too big for the sample RAM      |
| `src/utilities/megagus/xmfile.py`       | XMs read and written, for tests and comparison    |
| `src/utilities/megagus/modcompare.py`   | The player against libopenmpt, channel by channel |
| `src/utilities/megagus/hwcompare.py`    | A recording of the MEGA65 against the model       |
| `docs/megagus-programming.md`           | How to use it from BASIC, C and assembly          |
| `src/examples/megagus/`                 | The programs that guide goes through              |

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
