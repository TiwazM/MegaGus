/*
  MOD player for MegaGus on the MEGA65 (docs/megagus.md).

  Plays ProTracker-style modules: 31 samples, 4 channels ("M.K.", "M!K!", "FLT4", "4CHN") or
  more ("6CHN", "8CHN", "OCTA", "CD81", "xxCH" up to 31). The whole file goes into the
  MegaGus sample RAM as it is, so a module can be as big as that RAM (512 KB): the samples
  are played from where they lie in the file, the patterns are read from there a row at a
  time. One voice a channel; voice 31 plays nothing and is the clock: its position moves one
  byte an output frame, 48 000 a second, and the song's ticks are counted off it.

  Built with cc65 it is a C64-mode program (load to $0801, RUN) that lists the modules in
  the SD card's current directory. Built with a host compiler, or with cc65 for its
  simulator (-t sim6502), it plays the file named on its command line once through, speaking
  megagustest's line protocol on stdin/stdout: modplay_host.py answers that from the
  engine's model and writes what it plays to a WAV file.

  Not done: the E0 (filter), E3 (glissando) and EF (invert loop) commands, the swap of a
  sample without a new note, 15-sample modules.

  An F command from 32 up is a tempo (beats a minute), as ProTracker's CIA timing has it.
  Older modules mean a speed by every F: for those there is VBlank timing (the V key; -v on
  the host).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__CC65__) && !defined(__SIM6502__)
#define ON_MEGA65
#endif

typedef unsigned char u8;
typedef unsigned short u16; /* 16 bits here and on the host */
typedef short s16;
typedef unsigned long u32; /* at least 32 */

/* MegaGus */
#define MG_RAM 0xE000000UL
#define MG_REG 0xEFFF000UL

#define CTRL 0
#define PAN 1
#define VOL 2
#define FREQ 4
#define POS 7
#define START 10
#define END 13

#define RUN 0x01
#define LOOP 0x02

#define G_ID 0x200
#define G_RAMSIZE 0x203
#define G_CONTROL 0x204
#define G_MASTER 0x205

#define V(v) ((u16)(v) << 4)

#define MAXCH 31
#define CLOCK 31 /* the voice that keeps time */

/* ------------------------------------------------------------------------------------------
   The machine: far memory, the file, the screen
   ------------------------------------------------------------------------------------------ */
static u8 sector[512];

#ifdef ON_MEGA65

#include <conio.h>

#define POKE(a, v) (*(unsigned char *)(a) = (v))
#define PEEK(a) (*(unsigned char *)(a))

/* mega65-libc's fileio.s and dirent.s: the Hypervisor's files */
struct m65_dirent {
  unsigned long d_ino;
  unsigned short d_off;
  unsigned long d_reclen; /* the file's length */
  unsigned short d_type;  /* its FAT attributes */
  char d_name[256];
};
void closeall(void);
unsigned char open(char *filename);
void close(unsigned char fd);
unsigned short read512(unsigned char *buffer);
unsigned char opendir(void);
struct m65_dirent *readdir(unsigned char);
void closedir(unsigned char);

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

static struct dmalist dl = { 0x0B, 0x80, 0, 0x81, 0, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0 };
static unsigned char dbyte;

static void dma(void)
{
  POKE(0xD702U, 0);
  POKE(0xD704U, 0);
  POKE(0xD701U, (unsigned char)((unsigned int)&dl >> 8));
  POKE(0xD705U, (unsigned char)((unsigned int)&dl & 0xFF)); /* enhanced list: go */
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

static u8 mg_peek(u32 a)
{
  far_src(a);
  dl.dst_mb = 0;
  dl.dst_bank = 0;
  dl.dst = (unsigned int)&dbyte;
  dl.count = 1;
  dma();
  return dbyte;
}

static void mg_poke(u32 a, u8 v)
{
  dbyte = v;
  far_dst(a);
  dl.src_mb = 0;
  dl.src_bank = 0;
  dl.src = (unsigned int)&dbyte;
  dl.count = 1;
  dma();
}

static void mg_put(u32 dst, const u8 *src, u16 n)
{
  far_dst(dst);
  dl.src_mb = 0;
  dl.src_bank = 0;
  dl.src = (unsigned int)src;
  dl.count = n;
  dma();
}

static void mg_get(u8 *dst, u32 src, u16 n)
{
  far_src(src);
  dl.dst_mb = 0;
  dl.dst_bank = 0;
  dl.dst = (unsigned int)dst;
  dl.count = n;
  dma();
}

#else /* the host: modplay_host.py is the MEGA65 */

static u8 answer(void)
{
  unsigned int v;
  fflush(stdout);
  if (scanf("%x", &v) != 1)
    exit(2);
  return (u8)v;
}

static u8 mg_peek(u32 a)
{
  printf("R %lx\n", a);
  return answer();
}

static void mg_poke(u32 a, u8 v)
{
  printf("W %lx %x\n", a, v);
}

static void mg_put(u32 dst, const u8 *src, u16 n)
{
  u16 i;
  printf("C %lx", dst);
  for (i = 0; i < n; i++)
    printf(" %x", src[i]);
  putchar('\n');
}

static void mg_get(u8 *dst, u32 src, u16 n)
{
  u16 i;
  printf("D %lx %u\n", src, (unsigned int)n);
  for (i = 0; i < n; i++)
    dst[i] = answer();
}

/* nothing to do until the next tick: let a little time pass */
static void idle(void)
{
  puts("I");
}

#endif

static u8 rd(u16 r)
{
  return mg_peek(MG_REG + r);
}

static void wr(u16 r, u8 v)
{
  mg_poke(MG_REG + r, v);
}

/* ------------------------------------------------------------------------------------------
   The module
   ------------------------------------------------------------------------------------------ */
static u8 hdr[1084];
static u8 channels;
static u8 protracker; /* a 4-channel ProTracker module: its periods stop at 113 and 856 */
static u8 songlen;
static u8 npatterns;
static u32 filesize;
static u32 ramsize;

static u32 s_pos[32];    /* where each sample lies in the sample RAM */
static u32 s_end[32];    /* one past its last byte */
static u32 s_lstart[32]; /* its loop, if s_lend is not 0 */
static u32 s_lend[32];
static u8 s_vol[32];
static u8 s_ft[32];

/* Signatures as numbers: cc65 would turn the letters of a string into PETSCII */
static u8 sig_is(const u8 *s, u8 a, u8 b, u8 c, u8 d)
{
  return s[0] == a && s[1] == b && s[2] == c && s[3] == d;
}

static u8 digit(u8 c)
{
  return (u8)(c >= 0x30 && c <= 0x39);
}

/* The header in hdr[]: 0 if this is a module that can be played, else why not */
static const char *parse_header(void)
{
  u8 i, top;
  const u8 *s = hdr + 1080;
  const u8 *p;
  u32 at, len, ls, ll;

  channels = 0;
  protracker = 0;
  if (sig_is(s, 0x4D, 0x2E, 0x4B, 0x2E) || sig_is(s, 0x4D, 0x21, 0x4B, 0x21) || /* M.K. M!K! */
      sig_is(s, 0x46, 0x4C, 0x54, 0x34)) {                                       /* FLT4 */
    channels = 4;
    protracker = 1;
  }
  else if (sig_is(s, 0x4F, 0x43, 0x54, 0x41) || sig_is(s, 0x43, 0x44, 0x38, 0x31)) /* OCTA CD81 */
    channels = 8;
  else if (digit(s[0]) && s[1] == 0x43 && s[2] == 0x48 && s[3] == 0x4E) /* xCHN */
    channels = (u8)(s[0] - 0x30);
  else if (digit(s[0]) && digit(s[1]) && s[2] == 0x43 && s[3] == 0x48) /* xxCH */
    channels = (u8)((s[0] - 0x30) * 10 + (s[1] - 0x30));
  if (channels == 0)
    return "not a 31-sample module";
  if (channels > MAXCH)
    return "more channels than the player has";

  songlen = hdr[950];
  if (songlen == 0 || songlen > 128)
    return "no song in it";
  top = 0;
  for (i = 0; i < 128; i++)
    if (hdr[952 + i] > top)
      top = hdr[952 + i];
  npatterns = (u8)(top + 1);

  at = 1084UL + (u32)npatterns * 256UL * channels;
  if (at > filesize)
    return "the file is shorter than its patterns";
  for (i = 1; i < 32; i++) {
    p = hdr + 20 + (u16)(i - 1) * 30;
    len = (((u32)p[22] << 8) | p[23]) << 1;
    ls = (((u32)p[26] << 8) | p[27]) << 1;
    ll = (((u32)p[28] << 8) | p[29]) << 1;
    s_ft[i] = (u8)(p[24] & 0x0F);
    s_vol[i] = (u8)(p[25] > 64 ? 64 : p[25]);
    if (at + len > filesize) /* cut short: play what there is */
      len = filesize > at ? filesize - at : 0;
    s_pos[i] = at;
    s_end[i] = at + len;
    s_lstart[i] = 0;
    s_lend[i] = 0;
    if (ll > 2 && ls < len) {
      if (ls + ll > len)
        ll = len - ls;
      s_lstart[i] = at + ls;
      s_lend[i] = at + ls + ll;
    }
    at += len;
  }
  return 0;
}

/* ------------------------------------------------------------------------------------------
   The player
   ------------------------------------------------------------------------------------------ */
/* 32768 * 2^(-finetune/96), finetune 0..7 and -8..-1: a period tuned by eighths of a semitone */
static const u16 ft_ratio[16] = { 32768U, 32532U, 32298U, 32066U, 31835U, 31606U, 31379U, 31153U,
                                  34716U, 34467U, 34219U, 33973U, 33728U, 33486U, 33245U, 33005U };
/* 32768 * 2^(-n/12): a period n semitones up */
static const u16 arp_ratio[16] = { 32768U, 30929U, 29193U, 27554U, 26008U, 24548U, 23170U, 21870U,
                                   20643U, 19484U, 18390U, 17358U, 16384U, 15464U, 14596U, 13777U };
static const u8 sine[32] = { 0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                             255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97,  74,  49,  24 };

static u8 c_smp[MAXCH];    /* the sample the channel plays, 0: none yet */
static u8 c_vol[MAXCH];    /* 0..64 */
static u8 c_ft[MAXCH];
static u8 c_pan[MAXCH];
static u16 c_per[MAXCH];   /* its period, 0: no note yet */
static u16 c_target[MAXCH]; /* where a tone portamento is going */
static u8 c_fx[MAXCH];     /* this row's command and its argument */
static u8 c_arg[MAXCH];
static u8 c_tspd[MAXCH];   /* remembered: tone portamento speed, vibrato, tremolo, offset */
static u8 c_vib[MAXCH];
static u8 c_trm[MAXCH];
static u8 c_offs[MAXCH];
static u8 c_wave[MAXCH];   /* vibrato waveform (low nibble), tremolo (high) */
static u8 c_vibpos[MAXCH];
static u8 c_trmpos[MAXCH];
static s16 c_vibd[MAXCH];  /* this tick's vibrato: added to the period */
static s16 c_trmd[MAXCH];  /* this tick's tremolo: added to the volume */
static u8 c_arp[MAXCH];    /* this tick's arpeggio: semitones up */
static u16 c_dper[MAXCH];  /* a delayed note (EDx) waiting for its tick */
static u8 c_loopstart[MAXCH];
static u8 c_loopcnt[MAXCH];
static u16 o_per[MAXCH];   /* what the voice was last given: period and volume */
static u8 o_vol[MAXCH];

static u8 order, row, tickno, speed, bpm;
static u8 vblank; /* the module is from before tempos: every F command is a speed */
/* The song's F commands, read both ways at once, so that the timing can be changed in the
   middle of a song and is then what it would have been from the start. */
static u8 speed_cia, bpm_cia, speed_vbl;
static u8 patdelay, in_delay;
static u8 song_ended; /* the song came to its end, or jumped back: it has been played through */
static u8 rowbuf[MAXCH * 4];
static u8 regs[16];

static u16 per_min, per_max;

/* the clock */
static u16 clock_last;  /* the frame of the last tick */
static u16 tick_frames; /* frames until the next */
static u32 tick_len;    /* a tick in 1/256 frames */
static u8 tick_frac;

static void set_bpm(u8 b)
{
  bpm = b;
  tick_len = 30720000UL / b; /* 2.5 s / bpm, in 1/256 output frames */
}

static void use_timing(void)
{
  u8 b = vblank ? 125 : bpm_cia; /* VBlank: 50 ticks a second, always */

  speed = vblank ? speed_vbl : speed_cia;
  if (b != bpm)
    set_bpm(b);
}

static void next_tick_length(void)
{
  u32 t = tick_len + tick_frac;
  tick_frames = (u16)(t >> 8);
  tick_frac = (u8)(t & 0xFF);
}

static u16 clock_now(void)
{
  u8 lo = rd(V(CLOCK) + POS); /* reading the low byte holds the others */
  return (u16)(lo | ((u16)rd(V(CLOCK) + POS + 1) << 8));
}

static u16 tune(u16 period, u8 ft)
{
  if (ft == 0)
    return period;
  return (u16)(((u32)period * ft_ratio[ft] + 16384UL) >> 15);
}

static void put24(u8 *p, u32 v)
{
  p[0] = (u8)(v & 0xFF);
  p[1] = (u8)((v >> 8) & 0xFF);
  p[2] = (u8)((v >> 16) & 0xFF);
}

/* What the channel sounds like this tick: its period with arpeggio and vibrato, its volume
   with tremolo */
static u16 period_now(u8 ch)
{
  u32 p = c_per[ch];
  s16 q;
  if (c_arp[ch])
    p = (p * arp_ratio[c_arp[ch]] + 16384UL) >> 15;
  q = (s16)p + c_vibd[ch];
  if (q < 1)
    q = 1;
  return (u16)q;
}

static u8 volume_now(u8 ch)
{
  s16 v = (s16)c_vol[ch] + c_trmd[ch];
  if (v < 0)
    v = 0;
  if (v > 64)
    v = 64;
  return (u8)v;
}

/* Give the voice what changed since it was last told */
static void flush(u8 ch)
{
  u16 p, vol16;
  u8 v;
  if (c_per[ch] == 0)
    return;
  p = period_now(ch);
  v = volume_now(ch);
  if (v != o_vol[ch]) {
    vol16 = (u16)v * 1023U;
    regs[0] = (u8)(vol16 & 0xFF);
    regs[1] = (u8)(vol16 >> 8);
    mg_put(MG_REG + V(ch) + VOL, regs, 2);
    o_vol[ch] = v;
  }
  if (p != o_per[ch]) {
    put24(regs, 4842694UL / p);
    mg_put(MG_REG + V(ch) + FREQ, regs, 3);
    o_per[ch] = p;
  }
}

/* Start the channel's sample, offs bytes in. The voice is stopped while its registers
   change (one copy, PAN to END), so the engine never sees a new position with an old end. */
static void play(u8 ch, u32 offs)
{
  u8 smp = c_smp[ch];
  u8 ctrl = RUN;
  u16 p, vol16;
  u32 pos, st, en;

  if (smp == 0 || s_end[smp] <= s_pos[smp] || c_per[ch] == 0) {
    wr(V(ch) + CTRL, 0);
    return;
  }
  pos = s_pos[smp] + offs;
  st = s_pos[smp];
  en = s_end[smp];
  if (s_lend[smp]) {
    ctrl = RUN | LOOP;
    st = s_lstart[smp];
    en = s_lend[smp];
    if (pos >= en)
      pos = st;
  }
  else if (pos >= en) { /* an offset past the end: nothing to play */
    wr(V(ch) + CTRL, 0);
    return;
  }
  p = period_now(ch);
  o_per[ch] = p;
  o_vol[ch] = volume_now(ch);
  vol16 = (u16)o_vol[ch] * 1023U;
  regs[0] = c_pan[ch];
  regs[1] = (u8)(vol16 & 0xFF);
  regs[2] = (u8)(vol16 >> 8);
  put24(regs + 3, 4842694UL / p);
  put24(regs + 6, pos);
  put24(regs + 9, st);
  put24(regs + 12, en);
  wr(V(ch) + CTRL, 0);
  mg_put(MG_REG + V(ch) + PAN, regs, 15);
  wr(V(ch) + CTRL, ctrl);
}

static void slide_period(u8 ch, s16 by)
{
  s16 p = (s16)c_per[ch] + by;
  if (p < (s16)per_min)
    p = (s16)per_min;
  if (p > (s16)per_max)
    p = (s16)per_max;
  c_per[ch] = (u16)p;
}

static void slide_volume(u8 ch, s16 by)
{
  s16 v = (s16)c_vol[ch] + by;
  if (v < 0)
    v = 0;
  if (v > 64)
    v = 64;
  c_vol[ch] = (u8)v;
}

static void volume_slide(u8 ch)
{
  u8 a = c_arg[ch];
  if (a >> 4)
    slide_volume(ch, (s16)(a >> 4));
  else
    slide_volume(ch, -(s16)(a & 0x0F));
}

static void tone_portamento(u8 ch)
{
  u16 t = c_target[ch];
  u16 s = c_tspd[ch];
  if (t == 0 || c_per[ch] == 0)
    return;
  if (c_per[ch] < t) {
    c_per[ch] += s;
    if (c_per[ch] > t)
      c_per[ch] = t;
  }
  else if (c_per[ch] > t) {
    if (c_per[ch] - t < s)
      c_per[ch] = t;
    else
      c_per[ch] -= s;
  }
}

static u16 wave(u8 form, u8 pos)
{
  u8 i = (u8)(pos & 31);
  switch (form & 3) {
  case 1: /* ramp down */
    return (pos & 32) ? (u16)(255 - (i << 3)) : (u16)(i << 3);
  case 2: /* square */
    return 255;
  default:
    return sine[i];
  }
}

static void vibrato(u8 ch)
{
  u8 pos = c_vibpos[ch];
  s16 d = (s16)((wave(c_wave[ch], pos) * (u16)(c_vib[ch] & 0x0F)) >> 7);
  c_vibd[ch] = (pos & 32) ? -d : d;
  c_vibpos[ch] = (u8)((pos + (c_vib[ch] >> 4)) & 63);
}

static void tremolo(u8 ch)
{
  u8 pos = c_trmpos[ch];
  s16 d = (s16)((wave((u8)(c_wave[ch] >> 4), pos) * (u16)(c_trm[ch] & 0x0F)) >> 6);
  c_trmd[ch] = (pos & 32) ? -d : d;
  c_trmpos[ch] = (u8)((pos + (c_trm[ch] >> 4)) & 63);
}

/* where the song goes after this row */
static u8 jump_order, break_row, do_jump, do_break, do_loop, loop_row;

/* A new row: its notes, and what its commands do at once */
static void do_row(void)
{
  u8 ch, smp, fx, arg, x, start;
  u16 per;
  u32 offs;
  const u8 *p;

  mg_get(rowbuf, MG_RAM + 1084UL + ((u32)hdr[952 + order] * 64UL + row) * (u32)(channels * 4),
         (u16)(channels * 4));
  do_jump = do_break = do_loop = 0;
  break_row = 0;

  for (ch = 0; ch < channels; ch++) {
    p = rowbuf + ch * 4;
    smp = (u8)((p[0] & 0xF0) | (p[2] >> 4));
    per = (u16)(((u16)(p[0] & 0x0F) << 8) | p[1]);
    fx = (u8)(p[2] & 0x0F);
    arg = p[3];
    x = (u8)(arg & 0x0F);
    c_fx[ch] = fx;
    c_arg[ch] = arg;
    c_arp[ch] = 0;
    if (fx != 4 && fx != 6)
      c_vibd[ch] = 0;
    if (fx != 7)
      c_trmd[ch] = 0;
    start = 0;
    offs = 0;

    if (smp && smp < 32) {
      c_smp[ch] = smp;
      c_vol[ch] = s_vol[smp];
      c_ft[ch] = s_ft[smp];
    }
    if (fx == 0x0E && (arg >> 4) == 5)
      c_ft[ch] = x;
    if (per) {
      if ((fx == 3 || fx == 5) && c_per[ch]) /* slide to it */
        c_target[ch] = tune(per, c_ft[ch]);
      else if (fx == 0x0E && (arg >> 4) == 0x0D && x) /* later in the row */
        c_dper[ch] = per;
      else {
        c_per[ch] = tune(per, c_ft[ch]);
        c_target[ch] = c_per[ch];
        if (!(c_wave[ch] & 0x04))
          c_vibpos[ch] = 0;
        if (!(c_wave[ch] & 0x40))
          c_trmpos[ch] = 0;
        start = 1;
      }
    }

    switch (fx) {
    case 0x03:
      if (arg)
        c_tspd[ch] = arg;
      break;
    case 0x04:
      if (x)
        c_vib[ch] = (u8)((c_vib[ch] & 0xF0) | x);
      if (arg >> 4)
        c_vib[ch] = (u8)((c_vib[ch] & 0x0F) | (arg & 0xF0));
      break;
    case 0x07:
      if (x)
        c_trm[ch] = (u8)((c_trm[ch] & 0xF0) | x);
      if (arg >> 4)
        c_trm[ch] = (u8)((c_trm[ch] & 0x0F) | (arg & 0xF0));
      break;
    case 0x08: /* not a ProTracker command: its modules may use the number for other things */
      if (!protracker) {
        c_pan[ch] = arg;
        wr(V(ch) + PAN, arg);
      }
      break;
    case 0x09:
      if (arg)
        c_offs[ch] = arg;
      offs = (u32)c_offs[ch] << 8;
      break;
    case 0x0B:
      jump_order = arg;
      do_jump = 1;
      break;
    case 0x0C:
      c_vol[ch] = (u8)(arg > 64 ? 64 : arg);
      break;
    case 0x0D:
      break_row = (u8)((arg >> 4) * 10 + x);
      if (break_row > 63)
        break_row = 0;
      do_break = 1;
      break;
    case 0x0E:
      switch (arg >> 4) {
      case 0x1:
        slide_period(ch, -(s16)x);
        break;
      case 0x2:
        slide_period(ch, (s16)x);
        break;
      case 0x4:
        c_wave[ch] = (u8)((c_wave[ch] & 0xF0) | x);
        break;
      case 0x6:
        if (x == 0)
          c_loopstart[ch] = row;
        else if (c_loopcnt[ch] == 0) {
          c_loopcnt[ch] = x;
          do_loop = 1;
          loop_row = c_loopstart[ch];
        }
        else if (--c_loopcnt[ch]) {
          do_loop = 1;
          loop_row = c_loopstart[ch];
        }
        break;
      case 0x7:
        c_wave[ch] = (u8)((c_wave[ch] & 0x0F) | (x << 4));
        break;
      case 0x8:
        if (!protracker) {
          c_pan[ch] = (u8)(x * 17);
          wr(V(ch) + PAN, c_pan[ch]);
        }
        break;
      case 0xA:
        slide_volume(ch, (s16)x);
        break;
      case 0xB:
        slide_volume(ch, -(s16)x);
        break;
      case 0xC:
        if (x == 0)
          c_vol[ch] = 0;
        break;
      case 0xE:
        patdelay = x;
        break;
      default:
        break;
      }
      break;
    case 0x0F:
      if (arg) {
        speed_vbl = arg;
        if (arg < 32)
          speed_cia = arg;
        else
          bpm_cia = arg;
        use_timing();
      }
      break;
    default:
      break;
    }

    if (start)
      play(ch, offs);
    else
      flush(ch);
  }
}

/* The ticks of a row after its first */
static void do_effects(void)
{
  u8 ch, arg, x;

  for (ch = 0; ch < channels; ch++) {
    arg = c_arg[ch];
    x = (u8)(arg & 0x0F);
    switch (c_fx[ch]) {
    case 0x00:
      if (arg) {
        switch (tickno % 3) {
        case 1:
          c_arp[ch] = (u8)(arg >> 4);
          break;
        case 2:
          c_arp[ch] = x;
          break;
        default:
          c_arp[ch] = 0;
          break;
        }
      }
      break;
    case 0x01:
      if (c_per[ch])
        slide_period(ch, -(s16)arg);
      break;
    case 0x02:
      if (c_per[ch])
        slide_period(ch, (s16)arg);
      break;
    case 0x03:
      tone_portamento(ch);
      break;
    case 0x04:
      vibrato(ch);
      break;
    case 0x05:
      tone_portamento(ch);
      volume_slide(ch);
      break;
    case 0x06:
      vibrato(ch);
      volume_slide(ch);
      break;
    case 0x07:
      tremolo(ch);
      break;
    case 0x0A:
      volume_slide(ch);
      break;
    case 0x0E:
      switch (arg >> 4) {
      case 0x9: /* play the sample again every x ticks */
        if (x && tickno % x == 0) {
          play(ch, 0);
          continue;
        }
        break;
      case 0xC:
        if (tickno == x)
          c_vol[ch] = 0;
        break;
      case 0xD:
        if (tickno == x && c_dper[ch]) {
          c_per[ch] = tune(c_dper[ch], c_ft[ch]);
          c_target[ch] = c_per[ch];
          c_dper[ch] = 0;
          play(ch, 0);
          continue;
        }
        break;
      default:
        break;
      }
      break;
    default:
      break;
    }
    flush(ch);
  }
}

static void new_pattern(void)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++) {
    c_loopstart[ch] = 0;
    c_loopcnt[ch] = 0;
  }
}

/* After a row's last tick: the next row */
static void advance(void)
{
  u8 next = order;
  if (do_loop) {
    row = loop_row;
    return;
  }
  if (do_jump || do_break) {
    next = do_jump ? jump_order : (u8)(order + 1);
    if (do_jump && jump_order <= order)
      song_ended = 1;
    row = break_row;
  }
  else if (++row >= 64) {
    row = 0;
    next = (u8)(order + 1);
  }
  if (next != order || do_jump || do_break) {
    if (next >= songlen) {
      next = 0;
      song_ended = 1;
    }
    order = next;
    new_pattern();
  }
}

static void row_mark(void); /* the machine's: a new row is about to play */

/* One tick of the song */
static void tick(void)
{
  if (tickno == 0 && !in_delay) {
    row_mark();
    do_row();
  }
  else
    do_effects();
  if (++tickno >= speed) {
    tickno = 0;
    if (patdelay) { /* EEx: the row lasts x rows longer */
      patdelay--;
      in_delay = 1;
    }
    else {
      in_delay = 0;
      advance();
    }
  }
}

static void silence(void)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++)
    wr(V(ch) + CTRL, 0);
}

/* The engine and the player ready for the song in the sample RAM */
static void song_start(void)
{
  u8 ch, m;

  wr(G_CONTROL, 0x80);
  m = (u8)(128 / channels); /* all channels at full level on one side don't clip */
  if (m < 2)
    m = 2;
  wr(G_MASTER, m);

  memset(c_smp, 0, sizeof c_smp);
  memset(c_vol, 0, sizeof c_vol);
  memset(c_per, 0, sizeof c_per);
  memset(c_target, 0, sizeof c_target);
  memset(c_tspd, 0, sizeof c_tspd);
  memset(c_vib, 0, sizeof c_vib);
  memset(c_trm, 0, sizeof c_trm);
  memset(c_offs, 0, sizeof c_offs);
  memset(c_wave, 0, sizeof c_wave);
  memset(c_vibd, 0, sizeof c_vibd);
  memset(c_trmd, 0, sizeof c_trmd);
  memset(c_arp, 0, sizeof c_arp);
  memset(c_dper, 0, sizeof c_dper);
  memset(o_per, 0, sizeof o_per);
  memset(o_vol, 0xFF, sizeof o_vol);
  for (ch = 0; ch < channels; ch++)
    c_pan[ch] = ((ch & 3) == 0 || (ch & 3) == 3) ? 64 : 192; /* as an Amiga, less far apart */
  new_pattern();

  per_min = protracker ? 113 : 28;
  per_max = protracker ? 856 : 3424;
  order = row = tickno = 0;
  patdelay = in_delay = 0;
  song_ended = 0;
  speed_cia = speed_vbl = 6;
  bpm_cia = 125;
  bpm = 0;
  use_timing();

  /* the clock: silent, a byte a frame round the first 64 KB */
  memset(regs, 0, 16);
  regs[6] = 1;  /* FREQ $010000 */
  regs[15] = 1; /* END $010000 */
  mg_put(MG_REG + V(CLOCK) + PAN, regs + 1, 15);
  wr(V(CLOCK) + CTRL, RUN | LOOP);
  tick_frac = 0;
  clock_last = clock_now();
  tick_frames = 0; /* the first tick: now */
}

/* A tick if it is time for one. Returns 1 if there was. */
static u8 song_poll(void)
{
  u16 now = clock_now();
  u16 late = (u16)(now - clock_last);
  if (late < tick_frames)
    return 0;
  /* Held up for long (the CPU was stopped, by a monitor say): go on from here, don't rush
     through what was missed */
  if (late > 8000 && late > (u16)(tick_frames << 1))
    clock_last = now;
  else
    clock_last = (u16)(clock_last + tick_frames);
  tick();
  next_tick_length(); /* after the tick: a new tempo counts from the row that sets it */
  return 1;
}

/* ------------------------------------------------------------------------------------------
   The program
   ------------------------------------------------------------------------------------------ */
#ifdef ON_MEGA65

#define MAXFILES 44
#define NAMELEN 65
static char names[MAXFILES][NAMELEN];
static u32 sizes[MAXFILES];
static u8 nfiles;

static u8 lower(u8 c)
{
  return (u8)((c >= 0x41 && c <= 0x5A) ? c + 0x20 : c);
}

/* an ASCII character on the C64's screen (lower and upper case set) */
static void put_ascii(u8 c)
{
  if (c >= 0x61 && c <= 0x7A)
    c = (u8)(c - 0x20);
  else if (c >= 0x41 && c <= 0x5A)
    c = (u8)(c + 0x80);
  else if (c < 0x20 || c > 0x5F)
    c = 0x2E;
  cputc((char)c);
}

static void put_text(const u8 *s, u8 n)
{
  u8 i;
  for (i = 0; i < n && s[i]; i++)
    put_ascii(s[i]);
}

/* name.mod or mod.name */
static u8 is_mod(const char *n)
{
  u8 l = (u8)strlen(n);
  if (l < 5)
    return 0;
  if (lower((u8)n[l - 4]) == 0x2E && lower((u8)n[l - 3]) == 0x6D && lower((u8)n[l - 2]) == 0x6F &&
      lower((u8)n[l - 1]) == 0x64)
    return 1;
  return lower((u8)n[0]) == 0x6D && lower((u8)n[1]) == 0x6F && lower((u8)n[2]) == 0x64 && n[3] == 0x2E;
}

static void find_files(void)
{
  unsigned char d;
  struct m65_dirent *e;

  nfiles = 0;
  closeall();
  d = opendir();
  /* The name is taken from mega65-libc's dirent; the length and the attributes from the
     Hypervisor's own, which readdir leaves at $0400: the library reads those two a byte too
     early (the Hypervisor's short name is 13 bytes, not 12). */
  while (nfiles < MAXFILES && (e = readdir(d)) != 0) {
    if ((PEEK(0x0400U + 86) & 0x18) || !is_mod(e->d_name) || strlen(e->d_name) >= NAMELEN)
      continue;
    strcpy(names[nfiles], e->d_name);
    sizes[nfiles] = *(unsigned long *)(0x0400U + 82);
    nfiles++;
  }
  closedir(d);
}

static void show_files(u8 sel, u8 top)
{
  u8 i;
  clrscr();
  cputs("megagus mod player\r\n\r\n");
  if (nfiles == 0) {
    cputs("no .mod files in this directory\r\nof the sd card. q: leave");
    return;
  }
  for (i = top; i < nfiles && i < top + 20; i++) {
    gotoxy(0, (unsigned char)(3 + i - top));
    revers(i == sel);
    put_text((const u8 *)names[i], 32);
    revers(0);
    gotoxy(33, (unsigned char)(3 + i - top));
    cprintf("%4lu k", (sizes[i] + 1023UL) >> 10);
  }
  gotoxy(0, 24);
  cputs("up/down, return: play   q: leave");
}

/* The file into the sample RAM. 0, or why not. */
static const char *load(u8 n)
{
  unsigned char fd;
  u16 got;
  u32 at = 0;

  filesize = sizes[n];
  if (filesize > ramsize)
    return "bigger than the sample ram";
  if (filesize < 1084UL)
    return "too short for a module";
  closeall();
  fd = open(names[n]);
  if (fd == 0xFF)
    return "can't open it";
  /* A sector at a time, as many as the directory says the file has: read512's count is
     of no use (this mega65-libc loses its low byte). */
  while (at < filesize) {
    read512(sector);
    got = (u16)(filesize - at > 512UL ? 512UL : filesize - at);
    mg_put(MG_RAM + at, sector, got);
    at += got;
    if ((at & 0x3FFFUL) == 0) {
      gotoxy(0, 24);
      cprintf("loading %3lu k of %lu      ", at >> 10, (filesize + 1023UL) >> 10);
    }
  }
  close(fd);
  mg_get(hdr, MG_RAM, 1084);
  return parse_header();
}

static void show_song(u8 n)
{
  u8 i;
  clrscr();
  cputs("megagus mod player\r\n\r\n");
  put_text((const u8 *)names[n], 39);
  cputs("\r\n");
  put_text(hdr, 20);
  cprintf("\r\n%u channels, %u patterns, %lu k\r\n", channels, npatterns, (filesize + 1023UL) >> 10);
  /* the sample names: often the module's text */
  /* the first 17 whole, one a line as their writer saw them; the rest as far as they fit */
  for (i = 1; i < 32; i++) {
    if (i < 18) {
      gotoxy(0, (unsigned char)(6 + i));
      put_text(hdr + 20 + (u16)(i - 1) * 30, 22);
    }
    else {
      gotoxy(23, (unsigned char)(i - 11));
      put_text(hdr + 20 + (u16)(i - 1) * 30, 17);
    }
  }
  gotoxy(0, 24);
  cputs("space:pause +/-:skip v:timing q:stop");
}

static void show_timing(void)
{
  gotoxy(0, 6);
  cputs(vblank ? "timing: vblank (every f is a speed)" : "timing: cia (f from 32 is a tempo) ");
}

static void row_mark(void)
{
  gotoxy(0, 5);
  cprintf("position %3u/%-3u pattern %3u row %2u ", order + 1, songlen, hdr[952 + order], row);
}

static void play_song(u8 n)
{
  u8 paused = 0;
  char k;

  vblank = 0;
  show_song(n);
  show_timing();
  song_start();
  for (;;) {
    song_poll();
    if (!kbhit())
      continue;
    k = cgetc();
    if (k == 'q' || k == 3)
      break;
    if (k == ' ') {
      paused = (u8)!paused;
      wr(G_CONTROL, (u8)(paused ? 0 : 1)); /* the clock stops with the engine */
    }
    else if (k == 'v') {
      vblank = (u8)!vblank;
      use_timing();
      show_timing();
    }
    else if (k == '+' || k == '-') {
      silence();
      if (k == '+')
        order = (u8)(order + 1 < songlen ? order + 1 : 0);
      else
        order = (u8)(order ? order - 1 : 0);
      row = 0;
      tickno = 0;
      patdelay = in_delay = 0;
      new_pattern();
    }
  }
  wr(G_CONTROL, 0x80);
}

int main(void)
{
  u8 sel = 0, top = 0;
  char k;
  const char *why;

  POKE(0xD02FU, 0x47); /* MEGA65 registers on */
  POKE(0xD02FU, 0x53);
  POKE(0x0000U, 65); /* 40 MHz */

  if (rd(G_ID) != 0x4D || rd(G_ID + 1) != 0x47) {
    cputs("this core has no megagus\r\n");
    return 1;
  }
  ramsize = (u32)rd(G_RAMSIZE) << 16;
  find_files();
  show_files(sel, top);
  for (;;) {
    k = cgetc();
    if (k == 'q' || k == 3)
      break;
    if (nfiles == 0)
      continue;
    if (k == 17 && sel + 1 < nfiles) /* cursor down */
      sel++;
    else if (k == (char)145 && sel) /* cursor up */
      sel--;
    else if (k == 13) {
      why = load(sel);
      if (why) {
        gotoxy(0, 24);
        cprintf("%-39s", why);
        cgetc();
      }
      else
        play_song(sel);
    }
    if (sel < top)
      top = sel;
    if (sel >= top + 20)
      top = (u8)(sel - 19);
    show_files(sel, top);
  }
  clrscr();
  return 0;
}

#else /* the host: play the file named on the command line once through */

static u16 rows_marked, switch_at; /* -vN: change the timing over before row N (from 1) */

/* a mark for the harness: position, pattern, row, and the tempo as the row begins */
static void row_mark(void)
{
  if (++rows_marked == switch_at) { /* as the V key does on the machine */
    vblank = (u8)!vblank;
    use_timing();
  }
  printf("M %u %u %u %u %u\n", (unsigned)order, (unsigned)hdr[952 + order], (unsigned)row, (unsigned)speed,
         (unsigned)bpm);
}

int main(int argc, char **argv)
{
  FILE *f;
  u16 got;
  u32 at = 0;
  const char *why;

  while (argc > 2 && argv[1][0] == '-' && argv[1][1] == 'v') { /* -v: VBlank timing; -vN */
    if (argv[1][2])
      switch_at = (u16)atoi(argv[1] + 2);
    else
      vblank = 1;
    argv++;
    argc--;
  }
  if (argc < 2) {
    fprintf(stderr, "usage: modplay [-v] [-vN] file.mod\n");
    return 2;
  }
  f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "can't open %s\n", argv[1]);
    return 2;
  }
  ramsize = (u32)rd(G_RAMSIZE) << 16;
  while ((got = (u16)fread(sector, 1, 256, f)) != 0) {
    if (at + got <= ramsize)
      mg_put(MG_RAM + at, sector, got);
    at += got;
  }
  fclose(f);
  filesize = at;
  if (filesize > ramsize) {
    printf("P bigger than the sample ram\n");
    return 1;
  }
  if (filesize < 1084UL) {
    printf("P too short for a module\n");
    return 1;
  }
  mg_get(hdr, MG_RAM, 1084);
  why = parse_header();
  if (why) {
    printf("P %s\n", why);
    return 1;
  }
  printf("P %u channels, %u patterns, %u positions\n", (unsigned)channels, (unsigned)npatterns,
         (unsigned)songlen);
  song_start();
  while (!song_ended)
    if (!song_poll())
      idle();
  silence();
  return 0;
}

#endif
