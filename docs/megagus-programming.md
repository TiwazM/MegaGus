# Programming MegaGus

How to make sound with MegaGus from your own program: BASIC 65, C or assembly. The
registers are described one by one in [megagus.md](megagus.md); this is the way through
them. The examples are in `src/examples/megagus/`, and ready to run on the release's
`MEGAGUS.D81` (see [The examples](#the-examples)).

You need a MEGA65 running the MegaGus core: [the README](../README.md#installing) says how
to get it there. What follows describes the engine of release 1.0, which calls itself
version 3; where an older one differs, it says so.

## The idea

MegaGus is 32 voices and half a megabyte of sample RAM. You put samples (recorded or
computed waveforms) into the RAM once. To make a sound you tell a voice where its sample
is, how fast to play it, how loud and where between left and right, and set it going. From
then on the hardware plays it, 48 000 times a second, with no work for the CPU, until the
sample ends or you stop it. All 32 voices are mixed into the MEGA65's ordinary audio output.

A voice does not know about notes or instruments. A note is a sample played at a certain
speed; a tune is your program starting, changing and stopping voices at the right moments.

## Reaching it

Everything is in the MEGA65's 28-bit address space:

| What            | Where                                 |
|-----------------|---------------------------------------|
| Sample RAM      | `$E000000` to `$E07FFFF` (512 KB)     |
| Voice registers | `$EFFF000 + 16 * voice`, voices 0-31  |
| Global registers| `$EFFF200` to `$EFFF217`              |

These are beyond what `LDA $nnnn` reaches, so each language has its own way:

- **BASIC 65**: `POKE`, `PEEK` and `WPOKE` (two bytes at once) take the full address:
  `POKE $EFFF001,128`.
- **C (llvm-mos or cc65, with mega65-libc)**: `lpoke(address, value)`, `lpeek(address)`,
  and `lcopy(from, to, count)` for many bytes at once, from `memory.h`.
- **Assembly**: a 32-bit pointer in the zero page and `LDA [ptr],Z` / `STA [ptr],Z`; the
  DMA for blocks, with `$E0` as the megabyte.

An access takes a little longer than one to ordinary memory (a handful of clocks at
40 MHz), which only matters when you move a lot: use the DMA or `lcopy` for samples.

**Is it there?** `$EFFF200` reads `$4D` and `$EFFF201` reads `$47` ("MG") on a core with
MegaGus, and `$EFFF202` reads the engine's version (see [Versions](#versions)). Check
before anything else, and start from a known state by resetting the engine: write `$81` and
then `$01` to `$EFFF204`. That stops every voice and puts the master volume and the ramp
rate back to their defaults; it leaves the sample RAM alone.

## Getting a sample in

A sample is a row of numbers in the sample RAM: by default signed 8-bit (-128 to 127, the
form Amiga and MOD samples have). A voice can also read unsigned 8-bit (WAV's 8-bit form)
or signed or unsigned 16-bit, little-endian; you say which when you start the voice. Where
in the RAM a sample goes is up to you: keep a note of each one's first address and length.

**Computing one.** A single cycle of a waveform is enough for a tone. In BASIC:

    FOR I=0 TO 63:V=INT(127*SIN(I*3.14159265/32)):IF V<0 THEN V=V+256
    POKE $E000000+I,V:NEXT

**Copying one from your program's memory.** In C, `lcopy((long)wave, 0xE000000UL, 64);`.
In assembly, a DMA copy with destination megabyte `$E0` (see `tone.asm`).

**Loading one from disk.** In BASIC 65, straight into the sample RAM:

    BLOAD "KICK.RAW",P($E000000),R

`R` loads the file as it is, with no load address in front. In C, read the file a sector at
a time and `lcopy` each into place (the module player's `load()` in
`src/utilities/megagus/modplay.c` does that).

**Making the file.** Any audio file becomes a raw sample with ffmpeg, here mono, 22 050 Hz,
signed 8-bit:

    ffmpeg -i kick.wav -ac 1 -ar 22050 -f s8 kick.raw

Use `-f s16le` for 16-bit. The rate you choose is the sample's own pitch: you need it again
to play it. A lower rate takes less RAM and sounds duller; 22 050 Hz is fine for drums and
most instruments, 11 025 Hz for bass.

## Playing it

A voice has 16 registers' worth of bytes:

| Offset | Name  | Bytes | What                                                    |
|--------|-------|-------|---------------------------------------------------------|
| +0     | CTRL  | 1     | bit 0 run, 1 loop, 2 ping-pong, 3 16-bit, 4 backward,   |
|        |       |       | 5 unsigned; reads back bit 7: played to its end         |
| +1     | PAN   | 1     | 0 left, 128 middle, 255 right                           |
| +2     | VOL   | 2     | 0 to 65535                                              |
| +4     | FREQ  | 3     | how fast: 65536 is one sample each output frame         |
| +7     | POS   | 3     | where to play from (address in the sample RAM)          |
| +10    | START | 3     | where a loop goes back to                               |
| +13    | END   | 3     | one past the last sample                                |

Addresses here are the sample's address without the `$E000000`: a sample at `$E000400` is
at `$000400` for POS, START and END. Low byte first throughout.

To start a sound, in this order:

1. write 0 to CTRL: the voice stops while you set it up. What it last put out fades away
   over the next millisecond, whatever you write meanwhile;
2. write PAN, VOL, FREQ, START and END;
3. write POS, low byte first: the write to its third byte is what sets it;
4. write CTRL: 1 to play once to END, 3 to loop between START and END (add 8 for a 16-bit
   sample, 32 for an unsigned one).

In C all of 2 and 3 can be one `lcopy` of 15 bytes (see `voice()` in `tune.c`). Or 1 to 3
are one copy of all 16, with bit 0 clear in the CTRL byte, and 4 the one write after it:
the module player starts every note that way, with the DMA.

The voice starts with the next output frame, at most 1/48000 s later, and its level rises
from where it was (from nothing, if the voice was silent) to VOL in about a millisecond.

*On an engine before version 3* a stopped voice went on reading its sample while it faded,
as CTRL, POS and the sample RAM were at that moment. There, stop an unsigned or a 16-bit
sample by clearing bit 0 of CTRL alone, with the other bits as they were, and do not write
over a sample that has only just stopped.

**How fast.** FREQ decides the pitch:

    FREQ = rate * 65536 / 48000        to play a sample at the rate it was recorded at
    FREQ = hz * length * 65536 / 48000 for a one-cycle waveform of `length` samples at hz

For a recorded sample, a semitone up is FREQ times 1.05946, an octave up FREQ times 2:

    FREQ(n semitones above its own pitch) = rate * 65536 / 48000 * 2^(n/12)

A table of the twelve ratios (times 65536: 65536, 69433, 73562, 77936, 82570, 87480, 92682,
98193, 104032, 110218, 116772, 123715) and a shift for the octave avoids floating point.
FREQ can be anything from 1 to `$FFFFFF`; above `$020000` or so (more than an octave above
the sample's own rate) a recorded sample starts to lose its highest tones to aliasing.

## Loops

- **Once**: CTRL 1. The voice plays from POS to END and stops by itself.
- **Loop**: CTRL 3. At END it continues from START, for ever. For a one-cycle waveform START
  is its first sample and END one past its last.
- **Attack, then loop**: POS may be before START. An instrument sample with a loop near its
  end plays from POS = its first sample, and from then on round START..END.
- **Ping-pong**: CTRL 7. It turns round at END and at START instead of jumping, which hides
  a loop whose two ends don't match.
- **Backward**: bit 4.

A loop that clicks once a round has ends that don't meet: choose START and END where the
wave crosses zero in the same direction, or use ping-pong.

## Volume, panning, and not clicking

VOL is linear: 32768 is half the level. PAN is linear too, so a voice in the middle is at
half level on each side. Both can be written at any time while the voice plays.

The engine never jumps to a new volume: it ramps there in about a millisecond (the RAMP
register, `$EFFF206`, sets how fast for all voices: `$40` by default, `$01` for the
slowest, 85 ms across the full range, 0 for no ramp). That is what you use to avoid
clicks:

- **To stop a sound**, write 0 to CTRL. The voice holds the last value it put out and
  that fades with the ramp: no click, wherever in the wave it was. (Before version 3,
  write 0 to VOL first, wait a few milliseconds, and then stop.)
- **To let a sound die away**, a millisecond is too short: lower VOL yourself, a step
  every few milliseconds, and stop the voice when it has reached 0.
- **To start a note where one is still sounding**, give the old one its fade: stop the
  voice, wait two milliseconds, then start the new note. Started at once, the new note
  begins at the old one's level with a small step in the wave, which a loud bass shows.
  With 32 voices there is a better way: start the new note on another voice and let the
  old one fade on its own.
- **PAN has no ramp.** Move it in small steps, or while the voice is quiet.

## Many voices

The 32 voices are simply added. One voice at full VOL playing a full-scale sample, panned
hard to one side, fills that side by itself; the master volume (`$EFFF205`) then scales the
sum: `out = sum * MASTER / 64`, clipped. The default `$20` halves it, which leaves room for
about four loud voices. With more, lower MASTER or the voices' VOL: a sum that clips sounds
harsh. A rule that is safe: `MASTER = 128 / (number of voices that can be loud at once)`.

How you share the voices out is your program's business. A common plan: the first few for
music, the rest for sound effects, an effect taking the voice that has been idle longest
(the RUNNING register, four bytes at `$EFFF208`, has a bit for each voice that is playing).

## Changing a sound while it plays

Write the new FREQ, VOL or PAN and the voice has it from the next output frame. A pitch
slide, a vibrato or a fade is your program writing a slightly different value some tens of
times a second, from its main loop or from an interrupt (a raster interrupt gives 50 or
60 a second; a tracker's tick is the same thing).

A multi-byte register written while its voice plays can be caught by the engine between
two bytes, for one frame of 1/48000 s. With VOL the ramp hides it, and one frame at a
slightly wrong FREQ is not heard in a slide; where it must be exact, stop the voice, write,
and start it again.

## Knowing when

- **Has it finished?** CTRL bit 0 reads 1 while the voice plays. Bit 7 reads 1 once a voice
  has played to its END and stopped by itself; `$EFFF20C` has those bits for all voices,
  and writing a 1 clears one.
- **Where is it?** POS reads back the voice's position as it plays. Read the low byte
  first: that holds the other two for the next two reads.
- **A clock.** The engine counts the frames it plays, 48 000 a second and in step with the
  sound, in four bytes from `$EFFF214` (FRAMES). Read the lowest byte first: that holds
  the others for the reads that follow. Two bytes go round every 1.365 s, which is enough
  to wait by: `while ((now() - start) < frames)`. Writing anything to `$EFFF214` starts
  the count from 0, and it stands still while the engine is off. `tune.c` keeps time that
  way, and the module player takes its ticks from it.
- **On an engine of version 1** there is no counter. There a voice does it: with VOL 0,
  looping at FREQ `$010000`, it moves exactly one byte each output frame, and its POS is
  the clock. `tune.c` falls back to that.

## Ahead of time: the queue

A tune's changes have to come at the right moment, to a millisecond or so, and a program
is not always free at that moment: an effect that fills the whole frame, a loader, code
that must not be interrupted. From version 4 the engine can be handed the writes ahead of
time, each with the frame it is for, and makes them itself on that frame.

| Address    | Name  | What                                                             |
|------------|-------|------------------------------------------------------------------|
| `$EFFF218` | QTIME | 3 bytes: the frame (as FRAMES counts) the next writes are for    |
| `$EFFF21B` | QREG  | 2 bytes: the register the next write is for, as from `$EFFF000`  |
| `$EFFF21D` | QDATA | write: queue this value for that register; QREG goes up by one   |
| `$EFFF21E` | QSTAT | read: bit 0 empty, bit 1 full, bit 7 a write was lost; write     |
|            |       | `$80`: forget all that is queued                                 |
| `$EFFF21F` | QFREE | read: room, in sixteens of writes                                |

A note on voice 2, a quarter of a second from now, in C:

    unsigned long t = frames() + 12000;          /* FRAMES, read low byte first */
    lpoke(0xEFFF218UL, t); lpoke(0xEFFF219UL, t >> 8); lpoke(0xEFFF21AUL, t >> 16);
    lpoke(0xEFFF21BUL, 0x20); lpoke(0xEFFF21CUL, 0);      /* voice 2's CTRL: $020 */
    for (i = 0; i < 16; i++)
      lpoke(0xEFFF21DUL, note[i]);               /* CTRL without RUN, PAN, ... END */
    lpoke(0xEFFF21BUL, 0x20);                    /* and CTRL again, with RUN */
    lpoke(0xEFFF21DUL, note[0] | 1);

All of it takes effect on frame `t`, together, before that frame's voices are worked out.

- Queue the writes in the order of their frames: they are made in the order they were
  queued. One for a frame that has gone by is made at once.
- The queue holds 4096 writes. A tracker's tick is some tens of them, its fullest (eight
  notes at once) 136: half a second of an eight-channel tune is a few hundred. Read QFREE
  if in doubt; a write that finds the queue full is lost and sets bit 7 of QSTAT.
- To pause: switch the engine off (CONTROL = 0). FRAMES stands still, the voices hold, and
  what is queued waits; CONTROL = 1 goes on from there. To stop and start elsewhere in the
  tune: write `$80` to QSTAT, stop the voices, and queue from the new place.
- Do not write to FRAMES while writes are queued: their times are in its count.
- A queued write can be to a global register too: CONTROL = 0 queued for a frame stops
  the engine exactly there; steps of MASTER fade a tune out.

## Versions

`$EFFF202` reads the engine's version. A program written for one version runs on every
later one.

| Version | Since      | What came with it                                          |
|---------|------------|------------------------------------------------------------|
| 1       | 2026-11-22 | The engine: 32 voices, 512 KB, all that is not named below |
| 2       | 2026-11-25 | FRAMES at `$EFFF214`: the engine's own clock               |
| 3       | 2026-11-26 | A stopped voice fades its last value: CTRL = 0 stops any   |
|         |            | voice cleanly. This is the engine of release 1.0           |
| 4       | 2026-11-27 | The queue: writes handed over ahead of time, each made on  |
|         |            | its frame. On the branch `megagus-queue`, not released     |

A program that wants to run on all of them reads the version once and, below 3, stops its
voices the old way (see [Playing it](#playing-it)); below 2, keeps time with a voice. The
queue is there from 4 only.

## With the rest of the MEGA65's sound

MegaGus comes out with everything else, through the MEGA65's audio mixer, on the channel
pair the digital audio (`$D6F8`-`$D6FB` and the audio DMA) uses. SIDs, the OPL and MegaGus
all play together; the mixer's volume for that pair applies after MegaGus's own MASTER.

No emulator has MegaGus yet: these programs need a MEGA65 with the MegaGus core (so far
built for the R6 board).

## When nothing is heard

- `$EFFF200`/`$EFFF201` don't read `$4D`/`$47`: the core has no MegaGus.
- CONTROL (`$EFFF204`) bit 0 is clear: the engine is off. Write 1.
- VOL or MASTER is 0, or FREQ is 0.
- END is not above POS, or the addresses still have the `$E000000` in them.
- POS was written high byte first: its third byte must be the last.
- The sample is unsigned 8-bit (a WAV) played as signed: it plays, but harshly distorted.
  Set CTRL bit 5.
- A 16-bit sample starts on an odd address or is played without CTRL bit 3: noise.
- Clicks at the start and end of notes: see [above](#volume-panning-and-not-clicking).

## The examples

| File         | Language            | What it does                                        |
|--------------|---------------------|-----------------------------------------------------|
| `chord.bas`  | BASIC 65            | computes a sine and plays a chord on three voices   |
| `sample.bas` | BASIC 65            | loads a sample from disk and plays it on a key      |
| `tune.c`     | C (llvm-mos)        | a short tune on three voices, timed by the engine   |
| `tone.asm`   | assembly (ACME)     | a tone, with a DMA copy and a 32-bit pointer        |

`kick.raw` is a drum for `sample.bas`. Build lines for the C and the assembly are at the
top of each file; the BASIC listings are plain text, to type in, to paste into a transfer
tool, or to turn into program files with a tokenizer. All four are for MEGA65 mode. The C
example is built with llvm-mos and `src/utilities/megagus/mega65.ld`, a plan of memory for
a C program that starts with `RUN`, prints through the KERNAL and ends back in BASIC
([more](megagus.md#in-mega65-mode)).

**On a disk.** The release has all four, with the drum and the
[test program](megagus.md#test-program), as `MEGAGUS.D81` (`makedisk.py` makes it). Mount
it, and

| To run     | Type                                |
|------------|-------------------------------------|
| the chord  | `DLOAD "CHORD"` and `RUN`           |
| the sample | `DLOAD "SAMPLE"` and `RUN`          |
| the tone   | `DLOAD "TONE"` and `RUN`            |
| the tune   | `DLOAD "TUNE"` and `RUN`            |
| the tests  | `DLOAD "MEGAGUSTEST"` and `RUN`     |

All four were run from a disk like it on a MEGA65 R6 with the core of release 1.0
(2026-11-26; the tune as a C64-mode program then, and again as it is now) and recorded
over HDMI: the chord is its three notes (261.7, 329.7 and 392 Hz, the lowest
on the left and the highest on the right), the tone 440 Hz, the drum plays at each press of
SPACE (and the sample RAM begins with `kick.raw`'s bytes after the `BLOAD`), and the tune
plays its two seconds and says `done`.
