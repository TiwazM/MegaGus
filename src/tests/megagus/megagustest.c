/*
  MegaGus test program for the MEGA65.

  Finds the engine, tests its sample RAM and its registers, checks how voices run, loop,
  end and are timed, then plays a few things to listen to. See docs/megagus.md.

  Built with llvm-mos (mos-mega65-clang and ../../utilities/megagus/mega65.ld) it is a
  MEGA65-mode program: DLOAD it and RUN. It reaches MegaGus with the DMA, writes with the
  KERNAL, and ends back in BASIC. Built with a host compiler it speaks a line
  protocol on stdin/stdout, which megagustest_host.py answers from the engine's model: the
  same tests, run before anything is on hardware.

  Built with -DATTIC_SELFTEST it only tries its own far memory and timing routines, on the
  attic RAM: for an emulator, which has no MegaGus.
*/

#include <stdio.h>
#include <string.h>

#ifdef ATTIC_SELFTEST
#define MG_RAM 0x8000000UL
#else
#define MG_RAM 0xE000000UL
#endif
#define MG_REG 0xEFFF000UL

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
#define PINGPONG 0x04
#define BIT16 0x08
#define BACKWARD 0x10
#define UNSIGNED 0x20
#define ENDED 0x80

/* the globals */
#define G_ID 0x200
#define G_VERSION 0x202
#define G_RAMSIZE 0x203
#define G_CONTROL 0x204
#define G_MASTER 0x205
#define G_RAMP 0x206
#define G_VOICES 0x207
#define G_RUNNING 0x208
#define G_ENDED 0x20C
#define G_RATE 0x210

/* where the samples go */
#define SINE8 0x000000UL  /* 64 bytes: one cycle, 8 bits */
#define SINE16 0x000100UL /* 128 bytes: one cycle, 16 bits */
#define DRUM 0x000200UL   /* 2048 bytes: a falling tone dying away */
#define DRUM_LEN 2048U
#define SCRATCH 0x001000UL /* whatever the RAM test left: played silently */

/* FREQ for a 64-sample cycle at these pitches: hz * 64 * 65536 / 48000 */
#define F_C3 0x2CA7UL
#define F_E3 0x3842UL
#define F_G3 0x42E7UL
#define F_C4 0x594DUL
#define F_A4 0x9630UL

static const unsigned int scale[8] = { 0x594D, 0x643D, 0x7083, 0x7734, 0x85CD, 0x9630, 0xA894, 0xB29A };

static const unsigned char sine8[64] = {
  0x00, 0x0c, 0x19, 0x25, 0x31, 0x3c, 0x47, 0x51, 0x5a, 0x62, 0x6a, 0x70, 0x75, 0x7a, 0x7d, 0x7e,
  0x7f, 0x7e, 0x7d, 0x7a, 0x75, 0x70, 0x6a, 0x62, 0x5a, 0x51, 0x47, 0x3c, 0x31, 0x25, 0x19, 0x0c,
  0x00, 0xf4, 0xe7, 0xdb, 0xcf, 0xc4, 0xb9, 0xaf, 0xa6, 0x9e, 0x96, 0x90, 0x8b, 0x86, 0x83, 0x82,
  0x81, 0x82, 0x83, 0x86, 0x8b, 0x90, 0x96, 0x9e, 0xa6, 0xaf, 0xb9, 0xc4, 0xcf, 0xdb, 0xe7, 0xf4
};

static const unsigned char sine16[128] = {
  0x00, 0x00, 0x8c, 0x0c, 0xf9, 0x18, 0x28, 0x25, 0xfb, 0x30, 0x56, 0x3c, 0x1c, 0x47, 0x33, 0x51,
  0x82, 0x5a, 0xf1, 0x62, 0x6d, 0x6a, 0xe2, 0x70, 0x41, 0x76, 0x7c, 0x7a, 0x89, 0x7d, 0x61, 0x7f,
  0xff, 0x7f, 0x61, 0x7f, 0x89, 0x7d, 0x7c, 0x7a, 0x41, 0x76, 0xe2, 0x70, 0x6d, 0x6a, 0xf1, 0x62,
  0x82, 0x5a, 0x33, 0x51, 0x1c, 0x47, 0x56, 0x3c, 0xfb, 0x30, 0x28, 0x25, 0xf9, 0x18, 0x8c, 0x0c,
  0x00, 0x00, 0x74, 0xf3, 0x07, 0xe7, 0xd8, 0xda, 0x05, 0xcf, 0xaa, 0xc3, 0xe4, 0xb8, 0xcd, 0xae,
  0x7e, 0xa5, 0x0f, 0x9d, 0x93, 0x95, 0x1e, 0x8f, 0xbf, 0x89, 0x84, 0x85, 0x77, 0x82, 0x9f, 0x80,
  0x01, 0x80, 0x9f, 0x80, 0x77, 0x82, 0x84, 0x85, 0xbf, 0x89, 0x1e, 0x8f, 0x93, 0x95, 0x0f, 0x9d,
  0x7e, 0xa5, 0xcd, 0xae, 0xe4, 0xb8, 0xaa, 0xc3, 0x05, 0xcf, 0xd8, 0xda, 0x07, 0xe7, 0x74, 0xf3
};

static unsigned char buf[256];
static unsigned char buf2[256];

/* ------------------------------------------------------------------------------------------
   The machine: far memory, video frames, the screen
   ------------------------------------------------------------------------------------------ */
#ifdef __MEGA65__

#include <cbm.h>

#define POKE(a, v) (*(volatile unsigned char *)(a) = (v))
#define PEEK(a) (*(volatile unsigned char *)(a))
#define say printf

/* An enhanced DMA list: F018B format, source and destination megabyte, then the job */
struct dmalist {
  unsigned char opt_format;
  unsigned char opt_src_mb;
  unsigned char src_mb;
  unsigned char opt_dst_mb;
  unsigned char dst_mb;
  unsigned char opt_end;
  unsigned char command;
  unsigned int count;
  unsigned int src;
  unsigned char src_bank;
  unsigned int dst;
  unsigned char dst_bank;
  unsigned char sub_cmd;
  unsigned int modulo;
};

/* The DMA reads the list and reads and writes the program's memory behind the compiler's
   back: the list is volatile, and around the job the compiler is told that all memory may
   have been read and written, so that it neither drops a store nor keeps a stale value. */
static volatile struct dmalist dl = { 0x0B, 0x80, 0, 0x81, 0, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0 };
static volatile unsigned char dbyte;

static void dma(void)
{
  __asm__ volatile("" ::: "memory");
  POKE(0xD702U, 0);
  POKE(0xD704U, 0);
  POKE(0xD701U, (unsigned char)((unsigned int)&dl >> 8));
  POKE(0xD705U, (unsigned char)((unsigned int)&dl & 0xFF)); /* enhanced list: go */
  __asm__ volatile("" ::: "memory");
}

static void far_src(unsigned long a)
{
  dl.src_mb = (unsigned char)(a >> 20);
  dl.src_bank = (unsigned char)((a >> 16) & 0x0F);
  dl.src = (unsigned int)(a & 0xFFFFUL);
}

static void far_dst(unsigned long a)
{
  dl.dst_mb = (unsigned char)(a >> 20);
  dl.dst_bank = (unsigned char)((a >> 16) & 0x0F);
  dl.dst = (unsigned int)(a & 0xFFFFUL);
}

static unsigned char mg_peek(unsigned long a)
{
  far_src(a);
  dl.dst_mb = 0;
  dl.dst_bank = 0;
  dl.dst = (unsigned int)&dbyte;
  dl.count = 1;
  dma();
  return dbyte;
}

static void mg_poke(unsigned long a, unsigned char v)
{
  dbyte = v;
  far_dst(a);
  dl.src_mb = 0;
  dl.src_bank = 0;
  dl.src = (unsigned int)&dbyte;
  dl.count = 1;
  dma();
}

static void mg_put(unsigned long dst, const unsigned char *src, unsigned int n)
{
  far_dst(dst);
  dl.src_mb = 0;
  dl.src_bank = 0;
  dl.src = (unsigned int)src;
  dl.count = n;
  dma();
}

static void mg_get(unsigned char *dst, unsigned long src, unsigned int n)
{
  far_src(src);
  dl.dst_mb = 0;
  dl.dst_bank = 0;
  dl.dst = (unsigned int)dst;
  dl.count = n;
  dma();
}

/* Until the raster is next below the visible screen: once a video frame */
static void wait_frame(void)
{
  while (PEEK(0xD011U) & 0x80)
    ;
  while (!(PEEK(0xD011U) & 0x80))
    ;
}

static unsigned char video_fps(void)
{
  return (PEEK(0xD06FU) & 0x80) ? 60 : 50;
}

/* A key, as the KERNAL has it (PETSCII): what was typed before does not count */
static unsigned char wait_key(void)
{
  unsigned char k;
  while (cbm_k_getin())
    ;
  while ((k = cbm_k_getin()) == 0)
    ;
  return k;
}

static void setup(void)
{
  POKE(0xD02FU, 0x47); /* MEGA65 registers on */
  POKE(0xD02FU, 0x53);
}

#else /* the host: megagustest_host.py is the MEGA65 */

#include <stdarg.h>
#include <stdlib.h>

static void say(const char *fmt, ...)
{
  char text[512];
  char *p;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof text, fmt, ap);
  va_end(ap);
  fputs("P ", stdout);
  for (p = text; *p; p++) {
    if (*p == '\n')
      fputs("\\n", stdout);
    else
      putchar(*p);
  }
  putchar('\n');
}

static unsigned char answer(void)
{
  unsigned int v;
  fflush(stdout);
  if (scanf("%x", &v) != 1)
    exit(2);
  return (unsigned char)v;
}

static unsigned char mg_peek(unsigned long a)
{
  printf("R %lx\n", a);
  return answer();
}

static void mg_poke(unsigned long a, unsigned char v)
{
  printf("W %lx %x\n", a, v);
}

static void mg_put(unsigned long dst, const unsigned char *src, unsigned int n)
{
  unsigned int i;
  printf("C %lx", dst);
  for (i = 0; i < n; i++)
    printf(" %x", src[i]);
  putchar('\n');
}

static void mg_get(unsigned char *dst, unsigned long src, unsigned int n)
{
  unsigned int i;
  printf("D %lx %u\n", src, n);
  for (i = 0; i < n; i++)
    dst[i] = answer();
}

static void wait_frame(void)
{
  puts("F");
}

static unsigned char video_fps(void)
{
  return 50;
}

static unsigned char wait_key(void) /* nobody there: a key the first time, then the end */
{
  static unsigned char asked;

  return asked++ ? 'q' : ' ';
}

static void setup(void)
{
}

#endif

/* ------------------------------------------------------------------------------------------
   The registers
   ------------------------------------------------------------------------------------------ */
static unsigned char rd(unsigned int r)
{
  return mg_peek(MG_REG + r);
}

static void wr(unsigned int r, unsigned char v)
{
  mg_poke(MG_REG + r, v);
}

static void wr16(unsigned int r, unsigned int v)
{
  wr(r, (unsigned char)(v & 0xFF));
  wr(r + 1, (unsigned char)(v >> 8));
}

static void wr24(unsigned int r, unsigned long v)
{
  wr(r, (unsigned char)(v & 0xFF));
  wr(r + 1, (unsigned char)((v >> 8) & 0xFF));
  wr(r + 2, (unsigned char)((v >> 16) & 0xFF));
}

/* low byte first: reading it holds the other two */
static unsigned long rd24(unsigned int r)
{
  unsigned long v;
  v = rd(r);
  v |= (unsigned long)rd(r + 1) << 8;
  v |= (unsigned long)rd(r + 2) << 16;
  return v;
}

static unsigned long rd32(unsigned int r)
{
  unsigned long v;
  v = rd24(r);
  v |= (unsigned long)rd(r + 3) << 24;
  return v;
}

#define V(v) ((unsigned int)(v) << 4)

/* Start a voice: everything, CTRL last */
static void voice(unsigned char v, unsigned char ctrl, unsigned long pos, unsigned long start,
                  unsigned long end, unsigned long freq, unsigned int vol, unsigned char pan)
{
  unsigned int b;
  b = V(v);
  wr(b + CTRL, 0);
  wr(b + PAN, pan);
  wr16(b + VOL, vol);
  wr24(b + FREQ, freq);
  wr24(b + START, start);
  wr24(b + END, end);
  wr24(b + POS, pos);
  wr(b + CTRL, ctrl);
}

static void wait_frames(unsigned int n)
{
  while (n--)
    wait_frame();
}

/* ------------------------------------------------------------------------------------------
   The checks: a line a test, the details only where one fails
   ------------------------------------------------------------------------------------------ */
static unsigned int tests, failed_tests;
static unsigned char failures;

static void test(const char *name)
{
  unsigned char n;
  ++tests;
  failures = 0;
  say("%s", name);
  for (n = (unsigned char)strlen(name); n < 26; n++) /* not %-26s: not every printf has it */
    say(" ");
}

static void fail_header(void)
{
  if (!failures)
    say("failed\n");
  ++failures;
}

static void check(const char *what, unsigned long got, unsigned long want)
{
  if (got != want) {
    fail_header();
    say(" %s: %lx not %lx\n", what, got, want);
  }
}

static void check_range(const char *what, unsigned long got, unsigned long lo, unsigned long hi)
{
  if (got < lo || got > hi) {
    fail_header();
    say(" %s: %lx not %lx-%lx\n", what, got, lo, hi);
  }
}

static void done(void)
{
  if (failures)
    ++failed_tests;
  else
    say("ok\n");
}

/* ------------------------------------------------------------------------------------------
   The sample RAM
   ------------------------------------------------------------------------------------------ */
static void block_pattern(unsigned int blk)
{
  unsigned int seed;
  unsigned char x, i;
  seed = blk * 37U + (blk >> 8) * 101U;
  x = (unsigned char)(seed >> 8);
  i = 0;
  do {
    buf[i] = (unsigned char)(i * 3 + (unsigned char)seed) ^ x;
  } while (++i);
}

static void test_ram(unsigned char size64k)
{
  unsigned int blocks, blk, bad;
  unsigned long first_bad;

  test("sample ram");
  blocks = (unsigned int)size64k << 8; /* of 256 bytes */
  bad = 0;
  first_bad = 0;
  /* every block its own pattern, all written before any is read: a block that lands on
     another's address shows */
  for (blk = 0; blk < blocks; blk++) {
    block_pattern(blk);
    mg_put(MG_RAM + ((unsigned long)blk << 8), buf, 256);
  }
  for (blk = 0; blk < blocks; blk++) {
    block_pattern(blk);
    mg_get(buf2, MG_RAM + ((unsigned long)blk << 8), 256);
    if (memcmp(buf, buf2, 256)) {
      if (!bad)
        first_bad = (unsigned long)blk << 8;
      ++bad;
    }
  }
  check("bad 256-byte blocks", bad, 0);
  if (bad)
    say(" first at %lx\n", first_bad);

  /* single bytes, as a program pokes them */
  mg_poke(MG_RAM + 0x12345UL, 0x5A);
  mg_poke(MG_RAM + 0x12346UL, 0xA5);
  check("byte", mg_peek(MG_RAM + 0x12345UL), 0x5A);
  check("next byte", mg_peek(MG_RAM + 0x12346UL), 0xA5);

#ifndef ATTIC_SELFTEST
  /* nothing beyond its end, and a write there lands nowhere */
  block_pattern(0);
  mg_poke(MG_RAM + ((unsigned long)size64k << 16), 0xAA);
  check("beyond the end", mg_peek(MG_RAM + ((unsigned long)size64k << 16)), 0);
  check("first byte after that", mg_peek(MG_RAM), buf[0]);
#endif
  done();
}

static void load_samples(void)
{
  unsigned int i, n, phase, level;
  mg_put(MG_RAM + SINE8, sine8, 64);
  mg_put(MG_RAM + SINE16, sine16, 128);
  /* the drum: the sine, its pitch falling to a third and its level to nothing over 2048
     bytes (phase: 8.8 steps through the 64 bytes of sine8) */
  phase = 0;
  for (n = 0; n < DRUM_LEN; n += 256) {
    for (i = 0; i < 256; i++) {
      level = (DRUM_LEN - (n + i)) >> 3; /* 256 .. 0 */
      buf[i] = (unsigned char)(((int)(signed char)sine8[(phase >> 8) & 63] * (int)level) >> 8);
      phase += 205 - ((n + i) >> 4); /* 205 .. 78 */
    }
    mg_put(MG_RAM + DRUM + n, buf, 256);
  }
}

/* ------------------------------------------------------------------------------------------
   The registers and the voices, all at VOL 0: nothing to hear
   ------------------------------------------------------------------------------------------ */
static void test_globals(void)
{
  test("reset");
  wr(G_MASTER, 0x77);
  wr(G_RAMP, 0x11);
  wr(V(5) + CTRL, RUN | LOOP);
  wr(G_CONTROL, 0x80);
  check("control", rd(G_CONTROL), 0x01);
  check("master", rd(G_MASTER), 0x20);
  check("ramp", rd(G_RAMP), 0x40);
  check("running", rd32(G_RUNNING), 0);
  check("ended", rd32(G_ENDED), 0);
  wr(G_MASTER, 0x55);
  check("master written", rd(G_MASTER), 0x55);
  wr(G_MASTER, 0x20);
  done();
}

static void test_registers(void)
{
  static const unsigned char which[3] = { 0, 15, 31 };
  unsigned char i, r, v;

  test("voice registers");
  wr(G_CONTROL, 0); /* the engine stopped: nothing moves */
  for (i = 0; i < 3; i++) {
    v = which[i];
    for (r = 1; r < 16; r++)
      wr(V(v) + r, (unsigned char)((v << 3) + r * 17) ^ 0xA5);
  }
  for (i = 0; i < 3; i++) {
    v = which[i];
    for (r = 1; r < 16; r++)
      check("byte", rd(V(v) + r), (unsigned char)((unsigned char)((v << 3) + r * 17) ^ 0xA5));
    wr(V(v) + CTRL, 0x6E);
    check("ctrl", rd(V(v) + CTRL), 0x6E);
    wr(V(v) + CTRL, BACKWARD);
    check("ctrl backward", rd(V(v) + CTRL), BACKWARD);
    wr(V(v) + CTRL, 0);
  }
  wr(G_CONTROL, 1);
  wait_frames(2);
  check("pos kept", rd24(V(31) + POS), ((unsigned long)((unsigned char)((31 << 3) + 9 * 17) ^ 0xA5) << 16)
                                         | ((unsigned long)((unsigned char)((31 << 3) + 8 * 17) ^ 0xA5) << 8)
                                         | ((unsigned char)((31 << 3) + 7 * 17) ^ 0xA5));
  done();
}

static void test_one_shot(void)
{
  test("plays once and ends");
  voice(0, RUN, 0, 0, 64, 0x010000UL, 0, 128);
  check("running at once", rd(G_RUNNING) & 1, 1);
  wait_frames(2);
  check("running", rd(G_RUNNING) & 1, 0);
  check("ended bit", rd(G_ENDED) & 1, 1);
  check("ctrl", rd(V(0) + CTRL) & (RUN | ENDED), ENDED);
  check("pos", rd24(V(0) + POS), 63);
  wr(G_ENDED, 0x01);
  check("ended cleared", rd(V(0) + CTRL) & ENDED, 0);
  done();

  test("backward");
  voice(0, RUN | BACKWARD, 63, 0, 64, 0x010000UL, 0, 128);
  wait_frames(2);
  check("running", rd(G_RUNNING) & 1, 0);
  check("ctrl", rd(V(0) + CTRL) & (RUN | ENDED), ENDED);
  check("pos", rd24(V(0) + POS), 0);
  done();

  test("16-bit");
  voice(3, RUN | BIT16, 0x100, 0x100, 0x140, 0x010000UL, 0, 128);
  wait_frames(2);
  check("ctrl", rd(V(3) + CTRL) & (RUN | ENDED | BIT16), ENDED | BIT16);
  check("pos", rd24(V(3) + POS), 0x13E);
  done();
}

static void test_loops(void)
{
  unsigned char i, c, seen;
  unsigned long p, p1;

  test("loop");
  voice(1, RUN | LOOP, 0, 16, 48, 0x00F3A7UL, 0, 128);
  wait_frames(2);
  check("running", rd(G_RUNNING) & 2, 2);
  seen = 0;
  p1 = rd24(V(1) + POS);
  for (i = 0; i < 12; i++) {
    wait_frame();
    p = rd24(V(1) + POS);
    check_range("pos", p, 16, 47);
    if (p != p1)
      seen = 1;
  }
  check("moves", seen, 1);
  done();

  test("stop and go on");
  wr(V(1) + CTRL, LOOP);
  wait_frame();
  check("running", rd(G_RUNNING) & 2, 0);
  p1 = rd24(V(1) + POS);
  wait_frames(3);
  check("pos held", rd24(V(1) + POS), p1);
  check("not ended", rd(V(1) + CTRL) & ENDED, 0);
  wr(V(1) + CTRL, RUN | LOOP);
  check("running again", rd(G_RUNNING) & 2, 2);
  done();

  test("engine off and on");
  wr(G_CONTROL, 0);
  wait_frame();
  p1 = rd24(V(1) + POS);
  wait_frames(3);
  check("pos held", rd24(V(1) + POS), p1);
  check("still running", rd(G_RUNNING) & 2, 2);
  wr(G_CONTROL, 1);
  seen = 0;
  for (i = 0; i < 12; i++) {
    wait_frame();
    if (rd24(V(1) + POS) != p1)
      seen = 1;
  }
  check("moves again", seen, 1);
  wr(V(1) + CTRL, 0);
  done();

  test("ping-pong");
  /* a 32-sample loop at 1/256 sample a frame: one way in 0.17 s */
  voice(2, RUN | LOOP | PINGPONG, 16, 16, 48, 0x000100UL, 0, 128);
  seen = 0;
  for (i = 0; i < 40; i++) {
    wait_frame();
    c = rd(V(2) + CTRL);
    check_range("pos", rd24(V(2) + POS), 16, 47);
    seen |= (c & BACKWARD) ? 2 : 1;
  }
  check("both directions", seen, 3);
  check("running", rd(V(2) + CTRL) & RUN, RUN);
  wr(V(2) + CTRL, 0);
  done();
}

static void test_rate(void)
{
  unsigned int n, want;

  /* 6000 samples at 1/16 sample a frame: 96000 frames, 2 seconds at 48 kHz */
  test("48 khz");
  want = (unsigned int)video_fps() * 2;
  wait_frame();
  voice(4, RUN, SCRATCH, SCRATCH, SCRATCH + 6000, 0x001000UL, 0, 128);
  for (n = 0; n < 600; n++) {
    wait_frame();
    if (!(rd(G_RUNNING) & 0x10))
      break;
  }
  check_range("video frames", n, want - 3, want + 3);
  if (failures)
    say(" took %u video frames, at %u hz\n", n, (unsigned int)video_fps());
  done();
}

static void test_all_voices(void)
{
  unsigned char v;

  test("32 voices");
  for (v = 0; v < 32; v++)
    voice(v, RUN | LOOP, 0, 0, 64, 0x010000UL + v, 0, 128);
  check("running", rd32(G_RUNNING), 0xFFFFFFFFUL);
  wait_frames(2);
  check("still running", rd32(G_RUNNING), 0xFFFFFFFFUL);
  wr(V(9) + CTRL, 0);
  check("one stopped", rd32(G_RUNNING), 0xFFFFFDFFUL);
  wr(G_CONTROL, 0x80);
  check("after a reset", rd32(G_RUNNING), 0);
  done();
}

/* ------------------------------------------------------------------------------------------
   Things to listen to
   ------------------------------------------------------------------------------------------ */
static void sine(unsigned char v, unsigned long freq, unsigned int vol, unsigned char pan)
{
  voice(v, RUN | LOOP, SINE8, SINE8, SINE8 + 64, freq, vol, pan);
}

/* Fade a running voice out and wait until it is silent: before a stop or a jump in PAN,
   which would otherwise cut the sound off with a click */
static void hush(unsigned char v)
{
  wr16(V(v) + VOL, 0);
  wait_frames(3);
}

static void listen(void)
{
  unsigned char fps, v, i;
  unsigned int n;
  /* C major, spread wide: C3 C4 G4 C5 E5 G5 C6 E6, at least 125 Hz from one to the next.
     (Sines closer than that beat against each other: C3 E3 G3 together buzz at 33 Hz.) */
  static const unsigned long chord[8] = { F_C3,      F_C4,      F_G3 << 1, F_C4 << 1,
                                          F_E3 << 2, F_G3 << 2, F_C4 << 2, F_E3 << 3 };

  fps = video_fps();
  wr(G_CONTROL, 0x80);
  wr(G_RAMP, 4); /* levels move over 16 ms: no sound starts or ends abruptly */

  say("1 a tone (440 hz) in the middle\n");
  sine(0, F_A4, 0xC000U, 128);
  wait_frames(fps + fps / 2);

  say("2 left\n"); /* PAN has no ramp, a jump clicks: move it while the voice is silent */
  hush(0);
  wr(V(0) + PAN, 0);
  wr16(V(0) + VOL, 0xC000U);
  wait_frames(fps);
  say("3 right\n");
  hush(0);
  wr(V(0) + PAN, 255);
  wr16(V(0) + VOL, 0xC000U);
  wait_frames(fps);
  say("4 from left to right\n");
  hush(0);
  wr(V(0) + PAN, 0);
  wr16(V(0) + VOL, 0xC000U);
  for (n = 0; n < 256; n += 2) {
    wr(V(0) + PAN, (unsigned char)n);
    wait_frame();
  }
  hush(0);
  wr(V(0) + PAN, 128);
  wr16(V(0) + VOL, 0xC000U);

  say("5 fading out and in\n"); /* a step a video frame, each smoothed by the slowest ramp */
  wr(G_RAMP, 1);
  for (n = 0; n < 48; n++) {
    wr16(V(0) + VOL, 0xC000U - n * 0x400U);
    wait_frame();
  }
  for (n = 0; n <= 48; n++) {
    wr16(V(0) + VOL, n * 0x400U);
    wait_frame();
  }
  wr(G_RAMP, 4);
  hush(0);
  wr(V(0) + CTRL, LOOP);
  wait_frames(fps / 4);

  say("6 a scale\n"); /* one voice going on: only its FREQ changes */
  sine(0, scale[0], 0xC000U, 128);
  for (i = 0; i < 8; i++) {
    wr24(V(0) + FREQ, scale[i]);
    wait_frames(fps / 4);
  }
  hush(0);
  wr(V(0) + CTRL, LOOP);
  wait_frames(fps / 4);

  say("7 the tone from a 16-bit sample\n");
  voice(1, RUN | LOOP | BIT16, SINE16, SINE16, SINE16 + 128, F_A4, 0xC000U, 128);
  wait_frames(fps + fps / 2);
  hush(1);
  wr(V(1) + CTRL, LOOP | BIT16);
  wait_frames(fps / 4);

  say("8 four drum hits, left and right\n"); /* the drum's own shape: the fastest ramp */
  wr(G_RAMP, 0x40);
  for (i = 0; i < 4; i++) {
    voice(2, RUN, DRUM, DRUM, DRUM + DRUM_LEN, 0x004000UL, 0xFFFFU, (i & 1) ? 224 : 32);
    wait_frames(fps / 2);
  }

  /* the 16-bit sine, the slowest ramp (in and out in 85 ms); each note four times, a
     little apart in pitch and spread from left to right */
  say("9 a chord on all 32 voices\n");
  wr(G_MASTER, 0x04);
  wr(G_RAMP, 1);
  for (v = 0; v < 32; v++)
    voice(v, RUN | LOOP | BIT16, SINE16, SINE16, SINE16 + 128, chord[v & 7] + v * 3, 0xFFFFU,
          (unsigned char)(v * 8 + 4));
  wait_frames(fps * 2 + fps / 2);
  for (v = 0; v < 32; v++)
    wr16(V(v) + VOL, 0);
  wait_frames(fps / 4);
  wr(G_CONTROL, 0x80);
}

int main(void)
{
  unsigned char size64k;

  setup();
#ifdef ATTIC_SELFTEST
  say("far memory self-test\n\n");
  test_ram(8);
  test("video frames");
  wait_frames(video_fps());
  done();
  say("%u hz video, id reads %02x %02x\n", (unsigned int)video_fps(), rd(G_ID), rd(G_ID + 1));
  size64k = 0;
  return failed_tests ? 1 : 0;
#else
  say("megagus test\n\n");
  if (rd(G_ID) != 0x4D || rd(G_ID + 1) != 0x47) {
    say("no megagus in this core:\n");
    say("%02x %02x at efff200, not 4d 47\n", rd(G_ID), rd(G_ID + 1));
    return 1;
  }
  size64k = rd(G_RAMSIZE);
  say("version %u, %u voices, %u hz,\n", rd(G_VERSION), rd(G_VOICES),
      (unsigned int)rd(G_RATE) | ((unsigned int)rd(G_RATE + 1) << 8));
  say("%u kb of sample ram\n\n", (unsigned int)size64k * 64U);

  test_globals();
  test_ram(size64k);
  load_samples();
  test_registers();
  test_one_shot();
  test_loops();
  test_rate();
  test_all_voices();

  if (failed_tests)
    say("\n%u of %u tests failed\n", failed_tests, tests);
  else
    say("\nall %u tests passed\n", tests);

  say("\na key for the sounds\n");
  wait_key();
  do {
    listen();
    say("\ndone: q ends, another key plays\nthe sounds again\n");
  } while ((wait_key() | 0x20) != 0x71); /* Q, in ASCII or PETSCII */
  return failed_tests ? 1 : 0;
#endif
}
