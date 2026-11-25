/*
 * MegaGus from C: a short tune on three voices, with a sample made here.
 *
 *   cl65 --config ../../tests/vicii.cfg -O -I ../../mega65-libc/cc65/include -o tune.prg \
 *        tune.c ../../mega65-libc/cc65/src/memory.c ../../mega65-libc/cc65/src/memory_asm.s
 *
 * A C64-mode program (GO64, then load and RUN it). docs/megagus-programming.md goes through it.
 */
#include <stdio.h>
#include <memory.h>

#define MG_RAM 0xE000000UL /* the sample RAM */
#define MG_REG 0xEFFF000UL /* voice v's registers are at MG_REG + v * 16 */
#define MG_GLOBAL 0xEFFF200UL

/* a voice's registers */
#define CTRL 0
#define PAN 1
#define VOL 2
#define FREQ 4
#define POS 7
#define START 10
#define END 13
#define RUN 0x01
#define LOOP 0x02

#define CLOCK 31 /* the voice that keeps time on an engine without a frame counter */

static unsigned char counter; /* the engine counts its frames (version 2 on) */

static unsigned char block[16];

static void put24(unsigned char *p, unsigned long v)
{
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
}

/* Start a voice: from pos, looping between start and end if ctrl has LOOP. All but CTRL goes
   in one copy while the voice is stopped; CTRL comes last. */
static void voice(unsigned char v, unsigned char ctrl, unsigned long pos, unsigned long start,
                  unsigned long end, unsigned long freq, unsigned int vol, unsigned char pan)
{
  long reg = MG_REG + (long)v * 16;

  block[PAN] = pan;
  block[VOL] = (unsigned char)vol;
  block[VOL + 1] = (unsigned char)(vol >> 8);
  put24(block + FREQ, freq);
  put24(block + POS, pos);
  put24(block + START, start);
  put24(block + END, end);
  lpoke(reg + CTRL, 0);
  lcopy((long)(block + 1), reg + 1, 15);
  lpoke(reg + CTRL, ctrl);
}

static void volume(unsigned char v, unsigned int vol)
{
  long reg = MG_REG + (long)v * 16;
  lpoke(reg + VOL, (unsigned char)vol); /* low byte first: a frame may fall between the two, */
  lpoke(reg + VOL + 1, (unsigned char)(vol >> 8)); /* which the ramp smooths over */
}

/* Time, in output frames (48 000 a second), its low 16 bits. The engine counts them at
   $EFFF214. One of version 1 does not: there a voice with VOL 0 goes round the first 64 KB of
   the sample RAM a byte a frame, and its position is the time. Either way, reading the low
   byte holds the next one for the read that follows. */
static unsigned int now(void)
{
  unsigned char lo;
  if (counter) {
    lo = lpeek(MG_GLOBAL + 0x14);
    return lo | ((unsigned int)lpeek(MG_GLOBAL + 0x15) << 8);
  }
  lo = lpeek(MG_REG + CLOCK * 16 + POS);
  return lo | ((unsigned int)lpeek(MG_REG + CLOCK * 16 + POS + 1) << 8);
}

static void wait_ms(unsigned int ms)
{
  unsigned int start = now();
  unsigned int frames = ms * 48U; /* up to 1365 ms: the 16 bits go round every 1.365 s */
  while ((unsigned int)(now() - start) < frames)
    ;
}

/* FREQ for a 64-sample wave played hz times a second: hz * 64 * 65536 / 48000 */
static unsigned long freq_of(unsigned int hz)
{
  return (unsigned long)hz * 262144UL / 3000UL;
}

/* the tune: frequencies in Hz, three notes a step (0: the voice rests) */
static const unsigned int tune[][3] = {
  { 262, 0, 0 },   { 330, 0, 0 },   { 392, 0, 0 },   { 523, 0, 0 },
  { 392, 330, 0 }, { 330, 262, 0 }, { 262, 330, 392 }, { 262, 330, 392 },
};

int main(void)
{
  static unsigned char wave[64];
  unsigned char i, v, step;

  if (lpeek(MG_GLOBAL) != 0x4D || lpeek(MG_GLOBAL + 1) != 0x47) { /* "MG"; not as 'M', 'G': cc65
                                                                    makes those PETSCII */
    printf("no megagus in this core\n");
    return 1;
  }
  lpoke(MG_GLOBAL + 4, 0x81); /* reset the engine: every voice stops */
  lpoke(MG_GLOBAL + 4, 0x01); /* and on */

  /* a sample: one cycle of a soft square wave, signed 8-bit, copied into the sample RAM */
  for (i = 0; i < 64; i++)
    wave[i] = (unsigned char)(i < 4 ? i * 24 : i < 32 ? 96 : i < 36 ? 96 - (i - 32) * 48 : -96);
  lcopy((long)wave, MG_RAM, 64);
  counter = (unsigned char)(lpeek(MG_GLOBAL + 2) >= 2);
  if (!counter) /* the clock voice: silent, so what it runs through does not matter */
    voice(CLOCK, RUN | LOOP, 0, 0, 0x10000UL, 0x010000UL, 0, 128);

  for (step = 0; step < sizeof tune / sizeof tune[0]; step++) {
    for (v = 0; v < 3; v++)
      if (tune[step][v])
        voice(v, RUN | LOOP, 0, 0, 64, freq_of(tune[step][v]), 16000, (unsigned char)(64 + v * 64));
    wait_ms(200);
    for (v = 0; v < 3; v++)
      volume(v, 0); /* let the note die away: the ramp takes about a millisecond */
    wait_ms(50);
  }
  for (v = 0; v < 3; v++)
    lpoke(MG_REG + (long)v * 16 + CTRL, 0);
  if (!counter)
    lpoke(MG_REG + CLOCK * 16L + CTRL, 0);
  printf("done\n");
  return 0;
}
