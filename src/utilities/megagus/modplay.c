/*
  Module player for MegaGus on the MEGA65 (docs/megagus.md).

  Plays ProTracker-style modules: 31 samples, 4 channels ("M.K.", "M!K!", "FLT4", "4CHN") or
  more ("6CHN", "8CHN", "OCTA", "CD81", "xxCH" up to 32); Scream Tracker 3 modules (S3M) of
  up to 32 channels; FastTracker 2 modules (XM) of up to 32 channels, with their instruments'
  envelopes and vibrato.

  The file goes into the MEGA65's attic RAM as it is; the patterns are read from there a row
  at a time, the samples are copied from there into the MegaGus sample RAM. When they are more
  than that holds (512 KB) the biggest are made smaller on the way: a 16-bit sample is kept
  as 8-bit, then samples are kept at half their rate, then at a quarter. One voice a channel;
  the song's ticks are counted off the engine's frame counter, 48 000 a second. An engine of
  version 1 has none: there voice 31 plays nothing and is the clock (its position moves one
  byte an output frame), and a module can have 31 channels, not 32.

  Built with cc65 for the MEGA65 it is a C64-mode program in parts, since all of it is more
  than fits at once (modplay.cfg): a main part and four overlays (MODPLAY.OV1 to MODPLAY.OV4),
  which it reads when it starts, from the disk it was loaded from (maked81.py puts the five
  on a D81) or from the SD card's directory, and puts in place as it needs them. They are the list of
  the SD card's modules, the screens while one plays, the player of a MOD and an S3M, and the
  player of an XM. Built with a host compiler, or with cc65 for its simulator (-t sim6502),
  it is one program that plays the file named on its command line once through, speaking
  megagustest's line protocol on stdin/stdout: modplay_host.py answers that from the
  engine's model and writes what it plays to a WAV file.

  Not done in a MOD: the E0 (filter), E3 (glissando) and EF (invert loop) commands, the swap
  of a sample without a new note, 15-sample modules. In an S3M: AdLib instruments, stereo
  samples (the left half plays), S1x (glissando), SAx, SFx. In an XM: E3x (glissando).

  An F command from 32 up in a MOD is a tempo (beats a minute), as ProTracker's CIA timing
  has it. Older modules mean a speed by every F: for those there is VBlank timing (the V key;
  -v on the host).
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
typedef signed char s8;
typedef unsigned long u32; /* at least 32 */

#ifdef ON_MEGA65
/* this file's texts as the screen has them, upper and lower case */
#include <cbm_screen_charmap.h>
#pragma charmap (0x00, 0x00) /* but a text still ends with a zero */
#ifndef STAMP
#define STAMP 1 /* the build: the overlays of another are not taken */
#endif
/* Each overlay's first four bytes (modplay.cfg puts these segments first): the address the
   rest of the file loads to, as a program file on a disk begins, and the build's number */
extern u8 _OVL1_START__[], _OVL2_START__[], _OVL3_START__[], _OVL4_START__[];
#pragma rodata-name (push, "OVL1HEAD")
const u8 *const ov1_addr = _OVL1_START__ + 2;
const u16 ov1_stamp = STAMP;
#pragma rodata-name (pop)
#pragma rodata-name (push, "OVL2HEAD")
const u8 *const ov2_addr = _OVL2_START__ + 2;
const u16 ov2_stamp = STAMP;
#pragma rodata-name (pop)
#pragma rodata-name (push, "OVL3HEAD")
const u8 *const ov3_addr = _OVL3_START__ + 2;
const u16 ov3_stamp = STAMP;
#pragma rodata-name (pop)
#pragma rodata-name (push, "OVL4HEAD")
const u8 *const ov4_addr = _OVL4_START__ + 2;
const u16 ov4_stamp = STAMP;
#pragma rodata-name (pop)
#endif

/* MegaGus */
#define MG_RAM 0xE000000UL
#define MG_REG 0xEFFF000UL
#define ATTIC 0x8000000UL /* the MEGA65's 8 MB of slow RAM: the module's file goes there */
#define COLOUR_RAM 0xFF80000UL

#define CTRL 0
#define PAN 1
#define VOL 2
#define FREQ 4
#define POS 7
#define START 10
#define END 13

#define RUN 0x01
#define LOOP 0x02
#define PING 0x04     /* a loop that goes back and forth */
#define WIDE 0x08     /* a 16-bit sample */
#define UNSIGNED 0x20 /* an unsigned one */

#define G_ID 0x200
#define G_VERSION 0x202
#define G_FRAMES 0x214 /* from version 2: the frames played, four bytes */
#define G_RAMSIZE 0x203
#define G_CONTROL 0x204
#define G_MASTER 0x205

#define V(v) ((u16)(v) << 4)

#define MAXCH 32
#define CLOCK 31 /* the voice that keeps time where the engine has no frame counter */

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
/* cc65's: the KERNAL's LOAD, to an address; returns the bytes it loaded */
unsigned int __fastcall__ cbm_load(const char *name, unsigned char device, void *data);

void closeall(void);
unsigned char chdir(char *filename);
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

#ifdef NOGUS
/* No engine, for trying the screens in an emulator (cl65 -DNOGUS): it is there, takes what
   it is told, and its clock goes on whenever it is looked at */
static u16 fake_frames;
static u8 fake(u32 a, u8 *v)
{
  if (a < MG_RAM)
    return 0;
  *v = 0;
  switch ((u16)(a - MG_REG)) {
  case G_ID:
    *v = 0x4D;
    break;
  case G_ID + 1:
    *v = 0x47;
    break;
  case G_VERSION:
    *v = 2;
    break;
  case G_RAMSIZE:
    *v = 8;
    break;
  case G_FRAMES:
    fake_frames += 24;
    *v = (u8)(fake_frames & 0xFF);
    break;
  case G_FRAMES + 1:
    *v = (u8)(fake_frames >> 8);
    break;
  default:
    break;
  }
  return 1;
}
#endif

static u8 mg_peek(u32 a)
{
#ifdef NOGUS
  if (fake(a, &dbyte))
    return dbyte;
#endif
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
#ifdef NOGUS
  if (a >= MG_RAM)
    return;
#endif
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
#ifdef NOGUS
  if (dst >= MG_RAM && dst < COLOUR_RAM)
    return;
#endif
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
#define F_MOD 0
#define F_S3M 1
#define F_XM 2
static u8 fmt;      /* which kind of module */
static u8 channels; /* as many voices play */
/* The engine counts its frames itself (version 2 on): the song's clock. An engine before
   that has none, and its last voice is the clock: silent, a byte a frame. */
static u8 own_clock;
static u8 maxch; /* 32, or 31 when the last voice is the clock */
static u8 protracker; /* a 4-channel ProTracker module: its periods stop at 113 and 856 */
static u8 songlen;
static u8 orders[256];
static u16 npatterns;
static u32 filesize;
static u32 ramsize;

#define MAXSMP 100
static u8 nsamples; /* the highest sample number there is */

/* A sample as the file has it: where, how many frames, its loop in frames (f_le 0: none) */
#define FORM16 1
#define FORMUNS 2
#define FORMDELTA 4 /* each value is the difference from the one before (an XM's) */
#define FORMPING 8  /* its loop goes back and forth */
static u32 f_at[MAXSMP];
static u32 f_len[MAXSMP];
static u32 f_ls[MAXSMP];
static u32 f_le[MAXSMP];
static u8 f_form[MAXSMP];

/* And as it lies in the sample RAM */
static u32 s_pos[MAXSMP];    /* where */
static u32 s_end[MAXSMP];    /* one past its last byte */
static u32 s_lstart[MAXSMP]; /* its loop, if s_lend is not 0 */
static u32 s_lend[MAXSMP];
static u8 s_ctrl[MAXSMP];    /* WIDE, UNSIGNED: for the voice that plays it */
static u8 s_shift[MAXSMP];   /* kept at half (1) or a quarter (2) of its rate, to fit */
static u8 s_narrow[MAXSMP];  /* a 16-bit sample kept as 8-bit, to fit */
static u8 s_vol[MAXSMP];
static u8 s_ft[MAXSMP];      /* a MOD's finetune */
static u32 s_c5[MAXSMP];     /* an S3M's rate at C-4 */
static u8 s_pan[MAXSMP];     /* an XM's: where it sounds, */
static u8 s_rel[MAXSMP];     /* and how many semitones above the note it plays (signed) */
static u8 nreduced;          /* samples that were made smaller to fit */

/* Signatures as numbers: cc65 would turn the letters of a string into PETSCII */
static u8 sig_is(const u8 *s, u8 a, u8 b, u8 c, u8 d)
{
  return s[0] == a && s[1] == b && s[2] == c && s[3] == d;
}

static u8 digit(u8 c)
{
  return (u8)(c >= 0x30 && c <= 0x39);
}

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
/* A MOD's header in hdr[]: 0 if this is a module that can be played, else why not */
static const char *parse_mod(void)
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
    return "not a module this player knows";
  if (channels > maxch)
    return "more channels than the player has";

  songlen = hdr[950];
  if (songlen == 0 || songlen > 128)
    return "no song in it";
  top = 0;
  for (i = 0; i < 128; i++) {
    orders[i] = hdr[952 + i];
    if (hdr[952 + i] > top)
      top = hdr[952 + i];
  }
  npatterns = (u16)(top + 1);

  at = 1084UL + (u32)npatterns * 256UL * channels;
  if (at > filesize)
    return "the file is shorter than its patterns";
  nsamples = 31;
  for (i = 1; i < 32; i++) {
    p = hdr + 20 + (u16)(i - 1) * 30;
    len = (((u32)p[22] << 8) | p[23]) << 1;
    ls = (((u32)p[26] << 8) | p[27]) << 1;
    ll = (((u32)p[28] << 8) | p[29]) << 1;
    s_ft[i] = (u8)(p[24] & 0x0F);
    s_vol[i] = (u8)(p[25] > 64 ? 64 : p[25]);
    if (at + len > filesize) /* cut short: play what there is */
      len = filesize > at ? filesize - at : 0;
    f_at[i] = at;
    f_len[i] = len;
    f_form[i] = 0;
    f_ls[i] = 0;
    f_le[i] = 0;
    if (ll > 2 && ls < len) {
      if (ls + ll > len)
        ll = len - ls;
      f_ls[i] = ls;
      f_le[i] = ls + ll;
    }
    at += len;
  }
  return 0;
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

/* ------------------------------------------------------------------------------------------
   An S3M (Scream Tracker 3)
   ------------------------------------------------------------------------------------------ */
static u16 patptr[256]; /* where each pattern is in the file, in 16 bytes; 0: an empty one */
static u8 chvoice[32];  /* the voice an S3M channel plays on, 0xFF: none */
static u8 s3m_pan[MAXCH];
static u8 s3m_speed, s3m_tempo, s3m_gvol;
static u8 fastslides; /* volume slides move on a row's first tick too (Scream Tracker 3.00) */
/* What made the file decides how it is played, as libopenmpt has it (measured from it,
   modcompare.py): Impulse Tracker's S3Ms are tuned exactly and each command remembers its own
   argument; Scream Tracker's go by its table of periods and most commands share one. A sample
   number with no note changes the sample that is playing in Impulse Tracker's, in Scream
   Tracker 3.00's and in 3.20's made with a Sound Blaster, and nowhere else: not on a GUS. */
static u8 st3_hertz, st3_shared, st3_swap;

static u16 get16(const u8 *p)
{
  return (u16)(p[0] | ((u16)p[1] << 8));
}

static u32 get32(const u8 *p)
{
  return (u32)get16(p) | ((u32)get16(p + 2) << 16);
}

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
/* The file's bytes in order, from wherever: for going through the packed patterns */
static u32 by_at;
static u16 by_i, by_n;

static void bytes_from(u32 at)
{
  by_at = at;
  by_i = by_n = 0;
}

static u8 next_byte(void)
{
  if (by_i >= by_n) {
    if (by_at >= filesize)
      return 0;
    by_n = (u16)(filesize - by_at > 512UL ? 512UL : filesize - by_at);
    mg_get(sector, ATTIC + by_at, by_n);
    by_at += by_n;
    by_i = 0;
  }
  return sector[by_i++];
}

static const char *parse_s3m(void)
{
  static u8 used[32];
  static u8 pans[32];
  u16 ordnum, insnum, patnum, i, flags, cwt, gus = 0;
  u8 ffi, mv, dp, row, what, c, n, t;
  u32 at, len, lb, le;
  const u8 *p = hdr;

  ordnum = get16(p + 0x20);
  insnum = get16(p + 0x22);
  patnum = get16(p + 0x24);
  flags = get16(p + 0x26);
  cwt = get16(p + 0x28);
  ffi = p[0x2A];
  s3m_gvol = p[0x30];
  s3m_speed = p[0x31];
  s3m_tempo = p[0x32];
  mv = p[0x33];
  dp = p[0x35];
  if (s3m_gvol > 64 || (s3m_gvol == 0 && cwt < 0x1320))
    s3m_gvol = 64;
  if (s3m_speed == 0)
    s3m_speed = 6;
  if (s3m_tempo < 33)
    s3m_tempo = 125;
  fastslides = (u8)((flags & 0x40) || cwt == 0x1300);
  if (ordnum > 255)
    ordnum = 255;
  if (insnum >= MAXSMP)
    return "more samples than the player has room for";
  if (patnum > 256)
    patnum = 256;
  at = 0x60UL + get16(p + 0x20) + (u32)insnum * 2 + (u32)patnum * 2;
  if (ordnum == 0 || at > filesize)
    return "no song in it";
  songlen = (u8)ordnum;
  npatterns = patnum;
  nsamples = (u8)insnum;
  mg_get(orders, ATTIC + 0x60UL, ordnum);
  at = ATTIC + 0x60UL + get16(p + 0x20) + (u32)insnum * 2;
  memset(patptr, 0, sizeof patptr);
  mg_get((u8 *)patptr, at, (u16)(patnum * 2)); /* little-endian here and there */
#ifndef __CC65__
  for (i = 0; i < patnum; i++)
    patptr[i] = get16((const u8 *)&patptr[i]);
#endif
  memset(pans, 0, 32);
  if (dp == 252)
    mg_get(pans, at + (u32)patnum * 2, 32);

  /* the samples */
  for (i = 1; i <= insnum; i++) {
    mg_get(sector, ATTIC + 0x60UL + get16(p + 0x20) + (u32)(i - 1) * 2, 2);
    at = (u32)get16(sector) << 4;
    f_len[i] = 0;
    f_le[i] = 0;
    f_ls[i] = 0;
    f_form[i] = 0;
    f_at[i] = 0;
    s_vol[i] = 0;
    s_c5[i] = 8363;
    if (at == 0 || at + 0x50UL > filesize)
      continue;
    mg_get(sector, ATTIC + at, 0x50);
    s_vol[i] = (u8)(sector[0x1C] > 64 ? 64 : sector[0x1C]);
    s_c5[i] = get32(sector + 0x20);
    if (s_c5[i] == 0)
      s_c5[i] = 8363;
    if (sector[0] != 1 || sector[0x1E] != 0) /* not a sample (an AdLib voice), or a packed one */
      continue;
    gus |= get16(sector + 0x28); /* where the sample was in the maker's GUS; 1: a Sound Blaster */
    len = get32(sector + 0x10);
    lb = get32(sector + 0x14);
    le = get32(sector + 0x18);
    at = (((u32)sector[0x0D] << 16) | get16(sector + 0x0E)) << 4;
    if (sector[0x1F] & 4)
      f_form[i] |= FORM16;
    if (ffi != 1)
      f_form[i] |= FORMUNS;
    if (at >= filesize)
      continue;
    if (at + (len << (f_form[i] & FORM16)) > filesize) /* cut short: play what there is */
      len = (filesize - at) >> (f_form[i] & FORM16);
    f_at[i] = at;
    f_len[i] = len;
    if ((sector[0x1F] & 1) && lb < len && le > lb + 1) {
      if (le > len)
        le = len;
      f_ls[i] = lb;
      f_le[i] = le;
      f_len[i] = le; /* nothing after the loop is ever played */
    }
  }

  t = (u8)(cwt >> 12); /* 1: Scream Tracker, 3: Impulse Tracker, 5: OpenMPT */
  st3_hertz = (u8)(t == 3);
  st3_shared = (u8)((t == 1 && !(cwt == 0x1320 && p[0x34] == 0)) || t == 5); /* no ultra-click: not ST3 itself */
  st3_swap = (u8)(t == 3 || cwt == 0x1300 || (cwt == 0x1320 && p[0x34] && gus == 1));

  /* Which channels the patterns use: a voice for each of those. Modules often say they have
     all 32 and play on a few. */
  memset(used, 0, 32);
  for (i = 0; i < patnum; i++) {
    if (patptr[i] == 0 || ((u32)patptr[i] << 4) + 2 > filesize)
      continue;
    bytes_from(((u32)patptr[i] << 4) + 2);
    for (row = 0; row < 64;) {
      what = next_byte();
      if (what == 0) {
        row++;
        continue;
      }
      n = 0;
      if (what & 0x20) {
        next_byte();
        next_byte();
        n = 1;
      }
      if (what & 0x40) {
        next_byte();
        n = 1;
      }
      if (what & 0x80) {
        c = next_byte();
        next_byte();
        if (c)
          n = 1;
      }
      if (n)
        used[what & 31] = 1;
    }
  }
  channels = 0;
  for (i = 0; i < 32; i++) {
    t = p[0x40 + i];
    chvoice[i] = 0xFF;
    if (t >= 16 || !used[i]) /* an AdLib channel, one switched off, one that plays nothing */
      continue;
    if (channels >= maxch)
      return "more channels than the player has";
    chvoice[i] = channels;
    c = (u8)(t & 8 ? 0xCC : 0x33); /* right or left */
    if (!(mv & 0x80))
      c = 0x80; /* a mono module */
    if (pans[i] & 0x20)
      c = (u8)((pans[i] & 0x0F) * 17);
    s3m_pan[channels] = c;
    channels++;
  }
  if (channels == 0)
    return "no channel in it plays anything";
  return 0;
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

/* ------------------------------------------------------------------------------------------
   An XM (FastTracker 2)
   ------------------------------------------------------------------------------------------ */
/* The player's own tables for an XM, in the attic RAM above the file: for each instrument
   where it is in the file, its first sample's number here and how many it has (8 bytes each,
   from XINS); for each pattern where its rows are, how many, and their bytes (8 each, from
   XPAT). */
#define SIDE 0x7F0000UL
#define XINS (ATTIC + SIDE)
#define XPAT (ATTIC + SIDE + 0x800UL)

static u8 xm_linear;  /* notes are steps of a 64th of a semitone, not an Amiga's periods */
static u8 xm_restart; /* the position the song goes on from when it is over */
static u8 xm_nins;
static u8 xm_speed, xm_tempo;
static u8 regs[16];

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY4")
#pragma rodata-name (push, "OVERLAY4")
#endif
static void put32(u8 *p, u32 v)
{
  p[0] = (u8)(v & 0xFF);
  p[1] = (u8)((v >> 8) & 0xFF);
  p[2] = (u8)((v >> 16) & 0xFF);
  p[3] = (u8)(v >> 24);
}

static const char *parse_xm(void)
{
  u32 at, n, len, ls, ll;
  u16 i, k, ns, shs, npat;
  u8 smp = 1, first, t;

  if (get16(hdr + 58) < 0x0104)
    return "an xm older than version 1.04";
  k = get16(hdr + 64);
  if (k == 0)
    return "no song in it";
  songlen = (u8)(k > 255 ? 255 : k);
  k = get16(hdr + 66);
  xm_restart = (u8)(k < songlen ? k : 0);
  k = get16(hdr + 68);
  if (k == 0)
    return "no channel in it";
  if (k > maxch)
    return "more channels than the player has";
  channels = (u8)k;
  npat = get16(hdr + 70);
  if (npat > 256)
    npat = 256;
  npatterns = npat;
  k = get16(hdr + 72);
  xm_nins = (u8)(k > 128 ? 128 : k);
  xm_linear = (u8)(hdr[74] & 1);
  xm_speed = hdr[76];
  if (xm_speed == 0)
    xm_speed = 6;
  xm_tempo = hdr[78];
  if (xm_tempo < 32)
    xm_tempo = 125;
  memcpy(orders, hdr + 80, 256);

  at = 60UL + get32(hdr + 60);
  for (i = 0; i < npat; i++) {
    if (at + 9UL > filesize)
      return "the file is shorter than its patterns";
    mg_get(sector, ATTIC + at, 9);
    n = get32(sector);
    put32(regs, at + n);
    regs[4] = sector[5]; /* rows */
    regs[5] = sector[6];
    regs[6] = sector[7]; /* bytes */
    regs[7] = sector[8];
    mg_put(XPAT + ((u32)i << 3), regs, 8);
    at += n + get16(sector + 7);
  }

  for (i = 1; i <= xm_nins; i++) {
    memset(regs, 0, 8);
    if (at + 29UL > filesize) { /* the file ends here: the instruments it has no more of are empty */
      mg_put(XINS + ((u32)i << 3), regs, 8);
      continue;
    }
    mg_get(sector, ATTIC + at, 33);
    n = get32(sector);
    ns = get16(sector + 27);
    if (ns > 16)
      ns = 16;
    shs = get16(sector + 29);
    put32(regs, at);
    regs[4] = smp;
    regs[5] = (u8)ns;
    mg_put(XINS + ((u32)i << 3), regs, 8);
    at += n;
    if (ns == 0)
      continue;
    first = smp;
    for (k = 0; k < ns; k++) { /* the samples' headers, then their data in the same order */
      if (smp >= MAXSMP)
        return "more samples than the player has room for";
      mg_get(sector, ATTIC + at, 18);
      at += shs;
      len = get32(sector);
      ls = get32(sector + 4);
      ll = get32(sector + 8);
      t = sector[14];
      s_vol[smp] = (u8)(sector[12] > 64 ? 64 : sector[12]);
      s_ft[smp] = sector[13];
      s_pan[smp] = sector[15];
      s_rel[smp] = sector[16];
      f_form[smp] = FORMDELTA;
      if (t & 16)
        f_form[smp] |= FORM16;
      f_len[smp] = len; /* in bytes, until the data is come to */
      f_ls[smp] = 0;
      f_le[smp] = 0;
      if ((t & 3) && ll && ls < len) {
        if (ls + ll > len)
          ll = len - ls;
        f_ls[smp] = ls;
        f_le[smp] = ls + ll;
        if (t & 2)
          f_form[smp] |= FORMPING;
      }
      smp++;
    }
    for (k = first; k < smp; k++) {
      len = f_len[k];
      f_at[k] = at;
      if (at >= filesize)
        f_len[k] = 0;
      else if (at + len > filesize) /* cut short: play what there is */
        f_len[k] = filesize - at;
      at += len;
      if (f_form[k] & FORM16) {
        f_len[k] >>= 1;
        f_ls[k] >>= 1;
        f_le[k] >>= 1;
      }
      if (f_le[k] > f_len[k])
        f_le[k] = f_len[k];
      if (f_le[k] <= f_ls[k] + 1)
        f_le[k] = f_ls[k] = 0;
      if (f_le[k])
        f_len[k] = f_le[k]; /* nothing after the loop is ever played */
    }
  }
  nsamples = (u8)(smp - 1);
  return 0;
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

/* ------------------------------------------------------------------------------------------
   The samples into the sample RAM
   ------------------------------------------------------------------------------------------ */
#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY1")
#pragma rodata-name (push, "OVERLAY1")
#endif
/* The bytes a sample takes there */
static u32 stored(u8 i)
{
  u32 n = f_len[i] >> s_shift[i];
  if ((f_form[i] & FORM16) && !s_narrow[i])
    n = (n << 1) + 1; /* and one, at worst, to begin on an even address */
  return n;
}

/* Make them fit: what is too much for the sample RAM is made smaller, the biggest first. A
   16-bit sample becomes 8-bit; when there are no more of those, a sample is kept at half its
   rate, and when all are, at a quarter. A short loop is left alone: it would go out of tune. */
static const char *fit(void)
{
  u32 total, big, n;
  u8 i, pick, pass;

  nreduced = 0;
  for (i = 1; i <= nsamples; i++)
    s_shift[i] = s_narrow[i] = 0;
  for (;;) {
    total = 0;
    for (i = 1; i <= nsamples; i++)
      total += stored(i);
    if (total <= ramsize)
      return 0;
    pick = 0;
    big = 0;
    for (pass = 0; pass < 3 && !pick; pass++)
      for (i = 1; i <= nsamples; i++) {
        n = stored(i);
        if (n <= big)
          continue;
        if (pass == 0) {
          if (!(f_form[i] & FORM16) || s_narrow[i])
            continue;
        }
        else if (s_shift[i] != pass - 1 || (f_le[i] && ((f_le[i] - f_ls[i]) >> s_shift[i]) < 512))
          continue;
        big = n;
        pick = i;
      }
    if (!pick)
      return "its samples are too big, made smaller too";
    if (!s_shift[pick] && !s_narrow[pick])
      nreduced++;
    if ((f_form[pick] & FORM16) && !s_narrow[pick])
      s_narrow[pick] = 1;
    else
      s_shift[pick]++;
  }
}

/* One sample from the file to the sample RAM at 'to', made smaller on the way if it has to be:
   frames are averaged in twos or fours, a 16-bit frame gives its upper byte */
static void put_sample(u8 i, u32 to)
{
  u32 from = ATTIC + f_at[i];
  u32 left = f_len[i] << (f_form[i] & FORM16);
  u8 sh = s_shift[i];
  u8 wide = (u8)(f_form[i] & FORM16);
  u8 flip = (u8)((f_form[i] & FORMUNS) ? 0 : 0x80); /* signed: as unsigned while it is averaged */
  u8 group = (u8)((wide ? 2 : 1) << sh);
  u16 n, k, o, acc = 0;
  u8 g;
  u32 sum;
  const u8 *p;
  u8 *q;

  while (left) {
    n = (u16)(left > 512UL ? 512UL : left);
    mg_get(sector, from, n);
    from += n;
    left -= n;
    if (f_form[i] & FORMDELTA) { /* each value is the one before and what the file has */
      q = sector;
      if (wide)
        for (k = (u16)(n >> 1); k; k--) {
          acc = (u16)(acc + (q[0] | ((u16)q[1] << 8)));
          q[0] = (u8)(acc & 0xFF);
          q[1] = (u8)(acc >> 8);
          q += 2;
        }
      else
        for (k = n; k; k--) {
          acc = (u8)(acc + *q);
          *q++ = (u8)acc;
        }
    }
    /* the two that come up most, without sums of 32 bits: on the MEGA65 those take half a
       minute for a megabyte */
    if (wide && s_narrow[i] && !sh) { /* 16 bits to 8: the upper bytes */
      p = sector + 1;
      q = sector;
      for (k = (u16)(n >> 1); k; k--) {
        *q++ = *p;
        p += 2;
      }
      n >>= 1;
    }
    else if (!wide && sh == 1) { /* 8 bits at half the rate: the mean of two */
      p = sector;
      q = sector;
      for (k = (u16)(n >> 1); k; k--) {
        o = (u16)((u8)(p[0] ^ flip)) + (u8)(p[1] ^ flip);
        *q++ = (u8)((u8)(o >> 1) ^ flip);
        p += 2;
      }
      n >>= 1;
    }
    else if (sh || s_narrow[i]) {
      o = 0;
      for (k = 0; k + group <= n; k += group) {
        sum = 0;
        if (wide) {
          for (g = 0; g < group; g += 2)
            sum += (u16)(get16(sector + k + g) ^ ((u16)flip << 8));
          sum >>= sh;
          if (s_narrow[i])
            sector[o++] = (u8)((sum >> 8) ^ flip);
          else {
            sector[o++] = (u8)(sum & 0xFF);
            sector[o++] = (u8)((sum >> 8) ^ flip);
          }
        }
        else {
          for (g = 0; g < group; g++)
            sum += (u8)(sector[k + g] ^ flip);
          sector[o++] = (u8)((sum >> sh) ^ flip);
        }
      }
      n = o;
    }
    mg_put(MG_RAM + to, sector, n);
    to += n;
  }
}

static void progress(u8 i); /* the machine's: sample i of nsamples is going in */

/* All of them. 0, or why not. */
static const char *put_samples(void)
{
  u32 at = 0, frames, ll;
  u8 i, wide;
  const char *why = fit();

  if (why)
    return why;
  for (i = 1; i <= nsamples; i++) {
    wide = (u8)((f_form[i] & FORM16) && !s_narrow[i]);
    if (wide && (at & 1))
      at++;
    frames = f_len[i] >> s_shift[i];
    s_pos[i] = at;
    s_end[i] = at + (frames << wide);
    s_lstart[i] = s_lend[i] = 0;
    s_ctrl[i] = (u8)((wide ? WIDE : 0) | ((f_form[i] & FORMUNS) ? UNSIGNED : 0));
    if (f_form[i] & FORMPING)
      s_ctrl[i] |= PING;
    if (f_le[i]) {
      ll = (f_le[i] - f_ls[i]) >> s_shift[i];
      if (ll > 1) {
        s_lstart[i] = at + ((f_ls[i] >> s_shift[i]) << wide);
        s_lend[i] = s_lstart[i] + (ll << wide);
        if (s_lend[i] > s_end[i])
          s_lend[i] = s_end[i];
      }
    }
    if (frames) {
      progress(i);
      put_sample(i, at);
    }
    at = s_end[i];
  }
  return 0;
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

/* The file is in the attic RAM, filesize bytes of it: get it ready to play. 0, or why not. */
#ifdef ON_MEGA65
static void need(u8 n); /* the machine's: this overlay into memory */
#endif

static const char *open_module(void)
{
  const char *why;

  if (filesize < 0x60UL)
    return "too short for a module";
  if (filesize > SIDE)
    return "bigger than the player has room for";
  mg_get(hdr, ATTIC, (u16)(filesize < 1084UL ? filesize : 1084UL));
  protracker = 0;
  fmt = F_MOD;
  if (sig_is(hdr + 0x2C, 0x53, 0x43, 0x52, 0x4D)) /* SCRM */
    fmt = F_S3M;
  else if (filesize > 336UL && sig_is(hdr, 0x45, 0x78, 0x74, 0x65) && sig_is(hdr + 9, 0x4D, 0x6F, 0x64, 0x75))
    fmt = F_XM; /* "Extended Module" */
  else if (filesize < 1084UL)
    return "too short for a module";
#ifdef ON_MEGA65
  need((u8)(fmt == F_XM ? 4 : 3)); /* the part of the program that knows this kind */
#endif
  if (fmt == F_S3M)
    why = parse_s3m();
  else if (fmt == F_XM)
    why = parse_xm();
  else
    why = parse_mod();
  return why ? why : put_samples();
}

/* ------------------------------------------------------------------------------------------
   The player
   ------------------------------------------------------------------------------------------ */
/* Periods are kept in quarters: a vibrato is finer than a whole period, and so are the upper
   octaves of a finetuned sample. The tuning is ProTracker's as libopenmpt plays it (measured
   from it note by note, modcompare.py): ProTracker's table for the octave C-1 to B-1 at each
   finetune (0..7, -8..-1), the other octaves its halves and doubles; at finetune 0
   ProTracker's own two upper octaves, which are not all halves. */
static const u16 tuned[16][12] = {
  { 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453 },
  { 850, 802, 757, 715, 674, 637, 601, 567, 535, 505, 477, 450 },
  { 844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474, 447 },
  { 838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470, 444 },
  { 832, 785, 741, 699, 660, 623, 588, 555, 524, 495, 467, 441 },
  { 826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463, 437 },
  { 820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460, 434 },
  { 814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457, 431 },
  { 907, 856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480 },
  { 900, 850, 802, 757, 715, 675, 636, 601, 567, 535, 505, 477 },
  { 894, 844, 796, 752, 709, 670, 632, 597, 563, 532, 502, 474 },
  { 887, 838, 791, 746, 704, 665, 628, 592, 559, 528, 498, 470 },
  { 881, 832, 785, 741, 699, 660, 623, 588, 555, 524, 494, 467 },
  { 875, 826, 779, 736, 694, 655, 619, 584, 551, 520, 491, 463 },
  { 868, 820, 774, 730, 689, 651, 614, 580, 547, 516, 487, 460 },
  { 862, 814, 768, 725, 684, 646, 610, 575, 543, 513, 484, 457 }
};
static const u16 upper[24] = { 428, 404, 381, 360, 339, 320, 302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113 };
#define NOTES 72 /* six octaves from C-0 (period 1712); a note is its number in them */

static const u8 sine[32] = { 0,   24,  49,  74,  97,  120, 141, 161, 180, 197, 212, 224, 235, 244, 250, 253,
                             255, 253, 250, 244, 235, 224, 212, 197, 180, 161, 141, 120, 97,  74,  49,  24 };

static u8 c_smp[MAXCH];    /* the sample the channel's next note plays, 0: none yet */
static u8 c_play[MAXCH];   /* the sample its voice is playing */
static u8 c_vol[MAXCH];    /* 0..64 */
static u8 c_ft[MAXCH];
static u8 c_pan[MAXCH];
static u16 c_per[MAXCH];   /* its period in quarters, 0: no note yet */
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
static s16 c_vibd[MAXCH];  /* this tick's vibrato: added to the period, in quarters */
static s16 c_trmd[MAXCH];  /* this tick's tremolo: added to the volume */
static u8 c_arp[MAXCH];    /* this tick's arpeggio: semitones up */
static u16 c_dper[MAXCH];  /* a delayed note (EDx) waiting for its tick */
static u8 c_loopstart[MAXCH];
static u8 c_loopcnt[MAXCH];
static u16 o_per[MAXCH];   /* what the voice was last given: period and volume */
static u16 o_vol[MAXCH];
static u8 gvol;            /* the song's own volume, 0..64 */
static u32 freq_k;         /* a voice's FREQ is this over its period */
/* an S3M's */
static u8 c_note[MAXCH];   /* the note: 12 * octave + semitone */
static u32 c_c5[MAXCH];    /* its sample's rate at C-4 */
static u8 c_mem[13][MAXCH]; /* the arguments commands remember for when theirs is 0 */
static u8 c_fine[MAXCH];   /* the vibrato is a fine one (U) */
static u8 c_rtg[MAXCH];    /* ticks until the next retrigger */
static u8 c_tremor[MAXCH]; /* where a tremor is in its on and off time */
static u8 c_troff[MAXCH];  /* and that it is in its off time */
static u8 c_cut[MAXCH];    /* cut off: silent until the next note */
static u8 n_note[MAXCH], n_ins[MAXCH], n_vol[MAXCH]; /* the row as it was read */

/* an XM's */
static u8 x_ins[MAXCH];    /* the channel's instrument number */
static u32 x_iat[MAXCH];   /* where the instrument its note plays is in the file, 0: none */
/* That instrument's settings as the file has them: points in the volume and the panning
   envelope, then for each its sustain point, loop start and loop end, then for each its type
   (1: on, 2: sustain, 4: loop), then the vibrato's type, sweep, depth and rate, the fade */
static u8 x_p[MAXCH][16];
static u8 x_key[MAXCH];    /* the key is up */
static u16 x_fade[MAXCH];  /* what the fade after that has left, of 32768 */
/* The envelopes, volume [0] and panning [1]: the tick each is at, the tick of the point it is
   heading for, its value in 65536ths and what a tick adds; e_hold: it stands (1: at its
   sustain point until the key goes up, 2: at its end) */
static u16 e_pos[2][MAXCH];
static u16 e_next[2][MAXCH];
static long e_val[2][MAXCH];
static long e_delta[2][MAXCH];
static u8 e_hold[2][MAXCH];
static u8 x_avpos[MAXCH];  /* the instrument's own vibrato: where, how deep by now, and growing by */
static u16 x_avamp[MAXCH];
static u16 x_avsweep[MAXCH];
static s16 c_avd[MAXCH];   /* what it adds to the period this tick */
static u8 o_pan[MAXCH];    /* the PAN the voice was last given */
static u8 x_gain;          /* how much louder a voice is for where it is between the sides */
static u16 pat_rows = 64;  /* rows in the pattern that is playing */

static u8 vu[MAXCH];       /* for the screen: how loud a channel's last note began, 0..64 */
static u8 master;          /* the engine's MASTER as the song has it */
static u8 master_own;      /* and as it was before the keys changed it */
/* and a pattern row as the screen shows it: the note (0: none, 1: C-0, 253: key up, 254: cut),
   the instrument, the volume column (0xFF: none), the command as a screen code (0: none), its
   argument */
static u8 d_note[MAXCH], d_ins[MAXCH], d_vol[MAXCH], d_fx[MAXCH], d_arg[MAXCH];

static u8 order, row, tickno, speed, bpm;
static u8 vblank; /* the module is from before tempos: every F command is a speed */
/* The song's F commands, read both ways at once, so that the timing can be changed in the
   middle of a song and is then what it would have been from the start. */
static u8 speed_cia, bpm_cia, speed_vbl;
static u8 patdelay, in_delay;
static u8 begun; /* the song's first tick has been played */
static u8 song_ended; /* the song came to its end, or jumped back: it has been played through */
static u8 rowbuf[208]; /* a row: 4 bytes a channel in a MOD, up to 6 and one more in an S3M */

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
  u8 lo;
  if (own_clock) {
    lo = rd(G_FRAMES); /* reading the low byte holds the others */
    return (u16)(lo | ((u16)rd(G_FRAMES + 1) << 8));
  }
  lo = rd(V(CLOCK) + POS); /* here too */
  return (u16)(lo | ((u16)rd(V(CLOCK) + POS + 1) << 8));
}

/* A note's period, in quarters */
static u16 note_period(u8 note, u8 ft)
{
  u8 o = (u8)(note / 12);
  u8 n = (u8)(note % 12);
  if (ft == 0 && (o == 2 || o == 3))
    return (u16)(upper[(o - 2) * 12 + n] << 2);
  return (u16)((u16)(tuned[ft][n] << 3) >> o);
}

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
/* The note a pattern's period means: the nearest */
static u8 note_of(u16 period)
{
  u16 p4 = (u16)(period << 2);
  u16 d, best = 0xFFFFU;
  u8 k, note = 0;
  for (k = 0; k < NOTES; k++) {
    d = note_period(k, 0);
    d = d > p4 ? (u16)(d - p4) : (u16)(p4 - d);
    if (d > best)
      break; /* the periods only fall from here */
    best = d;
    note = k;
  }
  return note;
}

static u16 tune(u16 period, u8 ft)
{
  u16 p = note_period(note_of(period), ft);
  return p < per_min ? per_min : p; /* B-3 finetuned up, on an Amiga */
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

static void put24(u8 *p, u32 v)
{
  p[0] = (u8)(v & 0xFF);
  p[1] = (u8)((v >> 8) & 0xFF);
  p[2] = (u8)((v >> 16) & 0xFF);
}

/* An S3M's note as a period: Scream Tracker's, four to one of an Amiga's, for a sample that
   plays C-4 at c5 Hz */
static const u16 s3m_tab[12] = { 1712, 1616, 1524, 1440, 1356, 1280, 1208, 1140, 1076, 1016, 960, 907 };
/* and the exact ones, of the octave four below: 27392 / 2^(n/12) */
static const u16 s3m_exact[12] = { 27392, 25855, 24403, 23034, 21741, 20521, 19369, 18282, 17256, 16287, 15373, 14510 };

static u16 s3m_period(u8 note, u32 c5)
{
  u32 p, d;
  u8 o = (u8)(note / 12);
  if (o > 9)
    o = 9;
  if (c5 > 0xFFFFFUL)
    c5 = 0xFFFFFUL;
  if (c5 == 0)
    c5 = 8363;
  d = c5 << o;
  if (st3_hertz)
    p = (8363UL * s3m_exact[note % 12] + (d >> 1)) / d;
  else
    p = (133808UL * s3m_tab[note % 12]) / d; /* 8363 * 16 */
  if (p > 0xFFFFUL)
    p = 0xFFFFUL;
  return p ? (u16)p : 1;
}

/* An XM's note as a period. 'real' is the note counted from C-0 with the sample's own shift
   in it; the finetune moves in sixteenths of a semitone, as FastTracker's does. Linear: 64 to
   a semitone, 7680 at C-0. Else as an S3M's: 1712 at C-4. */
static u16 xm_note_period(u8 real, u8 ft)
{
  s16 f = (s16)(s8)ft;
  u16 p, q;
  u8 o;

  if (f >= 0)
    f >>= 3;
  else
    f = -((7 - f) >> 3);
  if (xm_linear)
    return (u16)(7680 - (s16)real * 64 - f * 4);
  if (f < 0 && real) { /* between this note and the one below */
    real--;
    f += 16;
  }
  o = (u8)(real / 12);
  p = (u16)((u16)(s3m_tab[real % 12] << 4) >> o);
  if (f > 0) {
    real++;
    q = (u16)((u16)(s3m_tab[real % 12] << 4) >> (real / 12));
    p -= (u16)(((p - q) * (u16)f) >> 4);
  }
  return p ? p : 1;
}

/* A linear period as a voice's FREQ: 8363 Hz at 4608, twice that 768 lower. The table is
   twice the FREQ of 8363 Hz times 2^(j/48); between its steps a straight line is as good. */
static const u16 xm_tab[49] = { 22837U, 23169U, 23506U, 23848U, 24194U, 24546U, 24903U, 25266U, 25633U, 26006U,
                                26384U, 26768U, 27157U, 27552U, 27953U, 28360U, 28772U, 29191U, 29615U, 30046U,
                                30483U, 30927U, 31376U, 31833U, 32296U, 32766U, 33242U, 33726U, 34216U, 34714U,
                                35219U, 35731U, 36251U, 36778U, 37313U, 37856U, 38406U, 38965U, 39532U, 40107U,
                                40690U, 41282U, 41882U, 42492U, 43110U, 43737U, 44373U, 45018U, 45673U };

static u32 xm_freq(u16 p)
{
  u16 y, m;
  u8 a, oct, j;

  if (p > 7680)
    p = 7680;
  y = (u16)(7680 - p);
  a = (u8)(y >> 8);
  oct = (u8)(a / 3);
  y = (u16)(((u16)(a % 3) << 8) | (y & 0xFF)); /* 0..767 within the octave */
  j = (u8)(y >> 4);
  m = xm_tab[j];
  m += (u16)(((xm_tab[j + 1] - m) * (y & 15)) >> 4);
  if (oct >= 5)
    return (u32)m << (oct - 5);
  return (u32)((m + (1U << (4 - oct))) >> (5 - oct));
}

/* a * b, its lower 32 bits. The MEGA65 has a multiplier that is there at once ($D770); cc65's
   own takes a millisecond for a tick of twenty channels. hw_mul: it was tried and is right. */
#ifdef ON_MEGA65
static u8 hw_mul;
#endif

static u32 mul(u32 a, u16 b)
{
#ifdef ON_MEGA65
  if (hw_mul) {
    *(u32 *)0xD770U = a;
    *(u16 *)0xD774U = b;
    *(u16 *)0xD776U = 0;
    return *(u32 *)0xD778U;
  }
#endif
  return a * b;
}

/* Where an XM's voice is between the sides, as the engine's PAN, and in x_gain how much louder
   it then has to be. FastTracker gives a side the square root of its share, the engine the
   share itself: PAN is moved towards the middle and the level raised until both sides get
   what FastTracker would give them. The tables are for the left half, 0 to the middle. */
static const u8 xm_pans[129] = {
  0,   15,  21,  25,  29,  32,  34,  37,  39,  41,  43,  45,  46,  48,  49,  51,  52,  54,  55,  56,  57,  59,
  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  73,  74,  75,  76,  77,  78,  78,  79,
  80,  81,  81,  82,  83,  83,  84,  85,  86,  86,  87,  88,  88,  89,  90,  90,  91,  91,  92,  93,  93,  94,
  95,  95,  96,  96,  97,  98,  98,  99,  99,  100, 100, 101, 102, 102, 103, 103, 104, 104, 105, 105, 106, 107,
  107, 108, 108, 109, 109, 110, 110, 111, 111, 112, 112, 113, 113, 114, 114, 115, 115, 116, 116, 117, 117, 118,
  118, 119, 119, 120, 121, 121, 122, 122, 123, 123, 124, 124, 125, 125, 126, 126, 127, 127, 128
};
static const u8 xm_gain[129] = {
  128, 136, 139, 141, 143, 145, 146, 147, 149, 150, 151, 152, 153, 154, 154, 155, 156, 157, 157, 158, 159, 159,
  160, 160, 161, 162, 162, 163, 163, 164, 164, 165, 165, 165, 166, 166, 167, 167, 167, 168, 168, 169, 169, 169,
  170, 170, 170, 170, 171, 171, 171, 172, 172, 172, 172, 173, 173, 173, 173, 174, 174, 174, 174, 175, 175, 175,
  175, 175, 176, 176, 176, 176, 176, 177, 177, 177, 177, 177, 177, 178, 178, 178, 178, 178, 178, 178, 178, 179,
  179, 179, 179, 179, 179, 179, 179, 179, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 181,
  181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181, 181
};

/* The voice's PAN: a MOD's and an S3M's channel has it as it is */
static u8 pan_now(u8 ch)
{
  s16 p, m, e;
  u8 q;

  if (fmt != F_XM)
    return c_pan[ch];
  p = (s16)c_pan[ch];
  if (x_p[ch][9] & 1) { /* the panning envelope moves it, the more the nearer the middle it is */
    e = (s16)(e_val[1][ch] >> 14) - 128;
    m = p;
    if (m > 128)
      m = (s16)(256 - m);
    m *= e;
    if (m >= 0)
      p += m >> 7;
    else
      p -= (-m) >> 7;
    if (p < 0)
      p = 0;
    if (p > 255)
      p = 255;
  }
  if (p <= 128) {
    q = (u8)p;
    x_gain = xm_gain[q];
    return xm_pans[q];
  }
  q = (u8)(256 - p);
  x_gain = xm_gain[q];
  return (u8)(255 - xm_pans[q]);
}

/* What the channel sounds like this tick: its period with arpeggio and vibrato, its volume
   with tremolo */
static u16 period_now(u8 ch)
{
  u16 p = c_per[ch];
  long q;
  u8 k;
  if (fmt == F_XM) {
    k = c_arp[ch];
    if (k && xm_linear)
      p = (u16)(p > (u16)k * 64 ? p - (u16)k * 64 : 1);
    else if (k)
      p = xm_note_period((u8)(c_note[ch] - 1 + (s8)s_rel[c_play[ch]] + k), c_ft[ch]);
    q = (long)p + c_vibd[ch] + c_avd[ch];
    if (q < 1)
      q = 1;
    if (q > 31999L)
      q = 31999L;
    return (u16)q;
  }
  if (c_arp[ch] && fmt == F_S3M)
    p = s3m_period((u8)(c_note[ch] + c_arp[ch]), c_c5[ch]);
  else if (c_arp[ch]) { /* the note the period is at or just above, and so many semitones up */
    for (k = 0; k < NOTES - 1 && note_period(k, c_ft[ch]) > p; k++)
      ;
    k = (u8)(k + c_arp[ch]);
    if (protracker && k > 47) {
      /* Past B-3 ProTracker reads on beyond its table. What comes out, as libopenmpt has it:
         the table's first note for the first two steps past the end, then on up from there. */
      k = (u8)(k - 48);
      k = (u8)(k ? k + 11 : 12);
    }
    else if (k > NOTES - 1)
      k = NOTES - 1;
    p = note_period(k, c_ft[ch]);
    if (p < per_min)
      p = per_min;
  }
  q = (long)p + c_vibd[ch];
  if (q < 1)
    q = 1;
  if (q > 0xFFFFL)
    q = 0xFFFFL;
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

/* That as the voice's VOL, with the song's volume in it: 64 at 64 is 64 * 1023 */
static u16 level_now(u8 ch)
{
  u16 x;
  u32 w;
  if (c_cut[ch] || c_troff[ch])
    return 0;
  if (fmt == F_XM) {
    /* The channel's volume, its instrument's envelope, the fade after the key went up and the
       song's volume, and x_gain for where it is: all of it at the middle is 46336, since a
       voice in the middle is on both sides. pan_now() comes first. */
    x = volume_now(ch);
    if (x_p[ch][8] & 1)
      w = mul(x, (u16)(e_val[0][ch] >> 8)); /* the envelope: 0..16384 */
    else
      w = (u32)x << 14;
    if (x_fade[ch] < 32768U)
      w = mul((u16)(w >> 5), (u16)(x_fade[ch] >> 2)) >> 8; /* w is at most 2^20 */
    if (gvol < 64)
      w = mul(w, gvol) >> 6;
    return (u16)(mul(w, x_gain) >> 12); /* w is at most 2^20 */
  }
  x = (u16)volume_now(ch) * gvol;
  return x >= 4096U ? 65472U : (u16)((x << 4) - (x >> 6));
}

/* The voice's FREQ for a period: less for a sample that is kept at a lower rate */
static u32 freq_of(u8 ch, u16 p)
{
  if (fmt == F_XM && xm_linear)
    return xm_freq(p) >> s_shift[c_play[ch]];
  return (freq_k / p) >> s_shift[c_play[ch]];
}

/* Give the voice what changed since it was last told */
static void flush(u8 ch)
{
  u16 p, v;
  u8 pn;
  if (c_per[ch] == 0)
    return;
  if (fmt == F_XM) {
    pn = pan_now(ch);
    if (pn != o_pan[ch]) {
      wr(V(ch) + PAN, pn);
      o_pan[ch] = pn;
    }
  }
  p = period_now(ch);
  v = level_now(ch);
  if (v != o_vol[ch]) {
    regs[0] = (u8)(v & 0xFF);
    regs[1] = (u8)(v >> 8);
    mg_put(MG_REG + V(ch) + VOL, regs, 2);
    o_vol[ch] = v;
  }
  if (p != o_per[ch]) {
    put24(regs, freq_of(ch, p));
    mg_put(MG_REG + V(ch) + FREQ, regs, 3);
    o_per[ch] = p;
  }
}

/* Start the channel's sample, offs frames in. The voice is stopped while its registers
   change (one copy, PAN to END), so the engine never sees a new position with an old end.
   play_at(.., 1): past the end of a loop is as far round it. */
static void play_at(u8 ch, u32 offs, u8 round);

static void play(u8 ch, u32 offs)
{
  play_at(ch, offs, 0);
}

static void play_at(u8 ch, u32 offs, u8 round)
{
  u8 smp = c_smp[ch];
  u8 ctrl = (u8)(RUN | s_ctrl[smp]);
  u16 p, vol16;
  u32 pos, st, en;

  c_play[ch] = smp;
  if (smp == 0 || s_end[smp] <= s_pos[smp] || c_per[ch] == 0) {
    wr(V(ch) + CTRL, 0);
    return;
  }
  offs >>= s_shift[smp];
  if (ctrl & WIDE)
    offs <<= 1;
  pos = s_pos[smp] + offs;
  st = s_pos[smp];
  en = s_end[smp];
  if (s_lend[smp]) {
    ctrl |= LOOP;
    st = s_lstart[smp];
    en = s_lend[smp];
    if (pos >= en) {
      if (fmt == F_XM) { /* FastTracker plays nothing from past the end, loop or not */
        wr(V(ch) + CTRL, 0);
        return;
      }
      pos = round ? st + (pos - st) % (en - st) : st;
    }
    if (ctrl & WIDE)
      pos &= ~1UL;
  }
  else if (pos >= en) { /* an offset past the end: nothing to play */
    wr(V(ch) + CTRL, 0);
    return;
  }
  regs[0] = o_pan[ch] = pan_now(ch);
  p = period_now(ch);
  o_per[ch] = p;
  vol16 = level_now(ch);
  o_vol[ch] = vol16;
  vu[ch] = (u8)(vol16 >> 10);
  if (fmt == F_XM)
    vu[ch] += (u8)(vu[ch] >> 2); /* an XM's level stops at 0.71 of the others' */
  regs[1] = (u8)(vol16 & 0xFF);
  regs[2] = (u8)(vol16 >> 8);
  put24(regs + 3, freq_of(ch, p));
  put24(regs + 6, pos);
  put24(regs + 9, st);
  put24(regs + 12, en);
  wr(V(ch) + CTRL, 0);
  mg_put(MG_REG + V(ch) + PAN, regs, 15);
  wr(V(ch) + CTRL, ctrl);
}

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
static void slide_period(u8 ch, s16 by)
{
  s16 p = (s16)c_per[ch] + by * 4;
  if (p < (s16)per_min)
    p = (s16)per_min;
  if (p > (s16)per_max)
    p = (s16)per_max;
  c_per[ch] = (u16)p;
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

static void slide_volume(u8 ch, s16 by)
{
  s16 v = (s16)c_vol[ch] + by;
  if (v < 0)
    v = 0;
  if (v > 64)
    v = 64;
  c_vol[ch] = (u8)v;
}

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
static void volume_slide(u8 ch)
{
  u8 a = c_arg[ch];
  if (a >> 4)
    slide_volume(ch, (s16)(a >> 4));
  else
    slide_volume(ch, -(s16)(a & 0x0F));
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

static void tone_portamento(u8 ch)
{
  u16 t = c_target[ch];
  u16 s = (u16)(c_tspd[ch] << 2);
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
  if (c_per[ch] == t)
    c_target[ch] = 0; /* there: a later 3xx with no note has nowhere to go */
}

static u16 wave(u8 form, u8 pos)
{
  u8 i = (u8)(pos & 31);
  switch (form & 3) {
  case 1: /* ramp down (the caller turns it over for an S3M, where it goes the other way) */
    return (pos & 32) ? (u16)(255 - (i << 3)) : (u16)(i << 3);
  case 2: /* square */
    return 255;
  default:
    return sine[i];
  }
}

/* The vibrato where it stands; go: and move it on (not on a row's first tick) */
static void vibrato(u8 ch, u8 go)
{
  u8 pos = c_vibpos[ch];
  s16 d = (s16)((wave(c_wave[ch], pos) * (u16)(c_vib[ch] & 0x0F)) >> 5); /* in quarters */
  if (c_fine[ch])
    d >>= 2;
  if (fmt == F_S3M && (c_wave[ch] & 3) == 1)
    d = -d;
  c_vibd[ch] = (pos & 32) ? -d : d;
  if (go)
    c_vibpos[ch] = (u8)((pos + (c_vib[ch] >> 4)) & 63);
}

/* The tremolo where it stands, and on. An S3M's is half as deep, and on a row's first tick
   it sounds where it stands (go 0). */
static void tremolo_at(u8 ch, u8 go)
{
  u8 pos = c_trmpos[ch];
  u16 w = wave((u8)(c_wave[ch] >> 4), pos);
  s16 d;
  /* FastTracker's ramp turns over where the vibrato is in its second half, not the tremolo:
     its mistake, and modules are heard with it */
  if (fmt == F_XM && ((c_wave[ch] >> 4) & 3) == 1) {
    w = (u16)((pos & 31) << 3);
    if (c_vibpos[ch] & 32)
      w = (u16)(255 - w);
  }
  d = (s16)((w * (u16)(c_trm[ch] & 0x0F)) >> 6);
  if (fmt == F_S3M) {
    d >>= 1;
    if (((c_wave[ch] >> 4) & 3) == 1)
      d = -d;
  }
  c_trmd[ch] = (pos & 32) ? -d : d;
  if (go)
    c_trmpos[ch] = (u8)((pos + (c_trm[ch] >> 4)) & 63);
}

static void tremolo(u8 ch)
{
  tremolo_at(ch, 1);
}

/* where the song goes after this row */
static u8 jump_order, break_row, do_jump, do_break, do_loop, loop_row;
/* FastTracker keeps the row a loop went back to, and begins the next pattern there unless a
   break names another row: its mistake, and modules count on it */
static u8 xm_bp;

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
/* A new row: its notes, and what its commands do at once */
static void do_row(void)
{
  u8 ch, smp, fx, arg, x, start;
  u16 per;
  u32 offs;
  const u8 *p;

  mg_get(rowbuf, ATTIC + 1084UL + ((u32)orders[order] * 64UL + row) * (u32)(channels * 4),
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
    /* ProTracker plays a row's first tick at the plain period, in a vibrato too; the trackers
       that came after it (and their modules with more channels) let a vibrato run on */
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
      if ((fx == 3 || fx == 5) && c_per[ch]) { /* slide to it */
        c_target[ch] = tune(per, c_ft[ch]);
        /* with a sample named and the voice silent (its sample was empty, or played out),
           that sample starts, at the pitch the slide begins from */
        if (smp && smp < 32 && !(rd(V(ch) + CTRL) & RUN))
          start = 1;
      }
      else if (fx == 0x0E && (arg >> 4) == 0x0D && x) /* later in the row */
        c_dper[ch] = per;
      else {
        c_per[ch] = tune(per, c_ft[ch]);
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

    /* outside ProTracker a vibrato sounds on a row's first tick too, where it stands */
    if (!protracker && (fx == 4 || fx == 6))
      vibrato(ch, 0);
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
      vibrato(ch, 1);
      break;
    case 0x05:
      tone_portamento(ch);
      volume_slide(ch);
      break;
    case 0x06:
      vibrato(ch, 1);
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

/* ------------------------------------------------------------------------------------------
   An S3M's rows and commands
   ------------------------------------------------------------------------------------------ */
static const u16 s3m_fine[16] = { 7895, 7941, 7985, 8046, 8107, 8169, 8232, 8280,
                                  8363, 8413, 8463, 8529, 8581, 8651, 8723, 8757 };

/* The pattern being played: its packed rows in the file (0: an empty pattern), and where the
   rows that have been come to begin */
static u32 pat_at;
static u16 rowofs[256];
static u16 rows_known;

static void s3m_pattern(void)
{
  u16 pp = orders[order] < npatterns ? patptr[orders[order]] : 0;
  pat_at = pp ? ((u32)pp << 4) + 2 : 0;
  if (pat_at >= filesize)
    pat_at = 0;
  rowofs[0] = 0;
  rows_known = 1;
}

/* Go through row r; keep: and take it into the channels */
static void s3m_parse(u8 r, u8 keep)
{
  u32 at = pat_at + rowofs[r];
  u16 n, i = 0;
  u8 what, v;
  u8 *pn = n_note, *pi = n_ins, *pv = n_vol, *pf = c_fx, *pa = c_arg;

  if (keep == 2) { /* for the screen */
    pn = d_note;
    pi = d_ins;
    pv = d_vol;
    pf = d_fx;
    pa = d_arg;
  }

  if (at >= filesize)
    return;
  n = (u16)(filesize - at > 200UL ? 200UL : filesize - at);
  mg_get(rowbuf, ATTIC + at, n);
  while (i < n) {
    what = rowbuf[i++];
    if (what == 0)
      break;
    v = keep ? chvoice[what & 31] : 0xFF;
    if (what & 0x20) {
      if (v != 0xFF) {
        pn[v] = rowbuf[i];
        pi[v] = rowbuf[i + 1];
      }
      i += 2;
    }
    if (what & 0x40) {
      if (v != 0xFF)
        pv[v] = rowbuf[i];
      i++;
    }
    if (what & 0x80) {
      if (v != 0xFF) {
        pf[v] = rowbuf[i];
        pa[v] = rowbuf[i + 1];
      }
      i += 2;
    }
  }
  if (r < 63) {
    rowofs[r + 1] = (u16)(rowofs[r] + i);
    if (rows_known < r + 2)
      rows_known = (u16)(r + 2);
  }
}

static void s3m_read(u8 r)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++) {
    n_note[ch] = 255;
    n_ins[ch] = 0;
    n_vol[ch] = 255;
    c_fx[ch] = 0;
    c_arg[ch] = 0;
  }
  if (!pat_at)
    return;
  while (rows_known <= r) /* jumped past rows not yet seen: find where they begin */
    s3m_parse((u8)(rows_known - 1), 0);
  s3m_parse(r, 1);
}

static void s3m_slide(u8 ch, long by)
{
  long p;
  if (c_per[ch] == 0)
    return;
  p = (long)c_per[ch] + by;
  if (p < 64)
    p = 64;
  if (p > 0xFFFFL)
    p = 0xFFFFL;
  c_per[ch] = (u16)p;
}

/* Dxy: x0 up, 0y down, on every tick but a row's first (in some modules on that too); xF and
   Fy once, on the first */
static void s3m_volslide(u8 ch, u8 arg, u8 first, u8 fast)
{
  u8 x = (u8)(arg >> 4), y = (u8)(arg & 0x0F);
  if (y == 0x0F && x) {
    if (first)
      slide_volume(ch, (s16)x);
  }
  else if (x == 0x0F && y) {
    if (first)
      slide_volume(ch, -(s16)y);
  }
  else if (first && !fast) {
    if (arg == 0x0F)
      slide_volume(ch, -15);
    else if (arg == 0xF0)
      slide_volume(ch, 15);
  }
  else if (y)
    slide_volume(ch, -(s16)y);
  else
    slide_volume(ch, (s16)x);
}

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

/* Qxy: the volume a retrigger leaves */
static void s3m_retrig_volume(u8 ch, u8 x)
{
  s16 v = (s16)c_vol[ch];
  switch (x) {
  case 1: v -= 1; break;
  case 2: v -= 2; break;
  case 3: v -= 4; break;
  case 4: v -= 8; break;
  case 5: v -= 16; break;
  case 6:
    if (fmt == F_XM)
      v = (s16)((v >> 1) + (v >> 3) + (v >> 4));
    else
      v = (s16)((v << 1) / 3);
    break;
  case 7: v >>= 1; break;
  case 9: v += 1; break;
  case 10: v += 2; break;
  case 11: v += 4; break;
  case 12: v += 8; break;
  case 13: v += 16; break;
  case 14: v = (s16)((v * 3) >> 1); break;
  case 15: v <<= 1; break;
  default: break;
  }
  if (v < 0)
    v = 0;
  if (v > 64)
    v = 64;
  c_vol[ch] = (u8)v;
}

#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY3")
#pragma rodata-name (push, "OVERLAY3")
#endif
/* A row's note, sample number and volume for a channel. Returns 1 if the sample is to start. */
static u8 s3m_note(u8 ch, u8 porta)
{
  u8 note = n_note[ch], ins = n_ins[ch], vol = n_vol[ch];
  u8 start = 0;

  if (ins && ins <= nsamples) {
    c_smp[ch] = ins;
    c_vol[ch] = s_vol[ins];
    /* With no note, where the file's maker would have it so: another sample than the one
       playing starts at once, from its beginning and at the pitch the channel is at; so does
       any sample after a cut. */
    if (st3_swap && note == 255 && c_per[ch] && (ins != c_play[ch] || c_cut[ch])) {
      c_cut[ch] = 0;
      start = 1;
    }
  }
  if (note == 254)
    c_cut[ch] = 1;
  else if (note != 255) {
    note = (u8)((note >> 4) * 12 + (note & 0x0F));
    c_c5[ch] = s_c5[c_smp[ch]];
    if (porta && c_per[ch] && !c_cut[ch] && (rd(V(ch) + CTRL) & RUN))
      c_target[ch] = s3m_period(note, c_c5[ch]);
    else {
      c_note[ch] = note;
      c_per[ch] = s3m_period(note, c_c5[ch]);
      c_target[ch] = 0;
      if (!(c_wave[ch] & 0x04))
        c_vibpos[ch] = 0;
      if (!(c_wave[ch] & 0x40))
        c_trmpos[ch] = 0;
      c_cut[ch] = 0;
      start = 1;
    }
  }
  if (vol <= 64)
    c_vol[ch] = vol;
  else if (vol >= 128 && vol <= 192) {
    vol = (u8)(vol - 128); /* 0 left to 64 right */
    c_pan[ch] = (u8)(vol << 2);
    if (vol == 64)
      c_pan[ch] = 255;
    wr(V(ch) + PAN, c_pan[ch]);
  }
  return start;
}

static void all_levels(void); /* the song's volume changed */

/* Which of a channel's remembered arguments a command uses, 0xFF: none. In Scream Tracker's
   own modules all of these share one; elsewhere the volume slides (D, K, L) have one, the
   pitch slides (E, F) one, and tremor, arpeggio, retrigger and the S commands one each. */
static u8 s3m_slot(u8 fx)
{
  u8 k;
  switch (fx) {
  case 4: case 11: case 12:
    k = 0;
    break;
  case 5: case 6:
    k = 1;
    break;
  case 9:
    k = 2;
    break;
  case 10:
    k = 3;
    break;
  case 17:
    k = 4;
    break;
  case 19:
    k = 5;
    break;
  case 18: /* tremolo: with the others in Scream Tracker's, its own two halves elsewhere */
    return (u8)(st3_shared ? 0 : 0xFF);
  default:
    return 0xFF;
  }
  return (u8)(st3_shared ? 0 : k);
}

/* Commands are numbered as their letters: A is 1 */
static void s3m_row(void)
{
  u8 ch, fx, arg, x, y, start, levels = 0;
  u32 offs;

  s3m_read(row);
  do_jump = do_break = do_loop = 0;
  break_row = 0;

  for (ch = 0; ch < channels; ch++) {
    fx = c_fx[ch];
    arg = c_arg[ch];
    c_arp[ch] = 0;
    c_vibd[ch] = 0;
    if (fx != 18)
      c_trmd[ch] = 0;
    if (fx != 9)
      c_troff[ch] = 0;
    x = s3m_slot(fx);
    if (x != 0xFF) { /* a command that remembers its argument */
      if (arg)
        c_mem[x][ch] = arg;
      else
        arg = c_mem[x][ch];
      c_arg[ch] = arg;
    }
    x = (u8)(arg >> 4);
    y = (u8)(arg & 0x0F);
    offs = 0;
    if (fx == 15) { /* Oxx */
      if (arg)
        c_offs[ch] = arg;
      offs = (u32)c_offs[ch] << 8;
    }
    start = 0;
    if (!(fx == 19 && x == 0x0D && y)) /* SDx: the note comes later in the row */
      start = s3m_note(ch, (u8)(fx == 7 || fx == 12));

    switch (fx) {
    case 1: /* Axx: speed */
      if (arg)
        speed = arg;
      break;
    case 2: /* Bxx */
      jump_order = arg;
      do_jump = 1;
      break;
    case 3: /* Cxx */
      break_row = (u8)(x * 10 + y);
      if (break_row > 63)
        break_row = 0;
      do_break = 1;
      break;
    case 4: /* Dxy */
      s3m_volslide(ch, arg, 1, fastslides);
      break;
    case 11: case 12: /* with a vibrato (K) or a slide to a note (L): never on the first tick */
      s3m_volslide(ch, arg, 1, 0);
      break;
    case 5: /* Exx: down; EFx fine, EEx extra fine */
      if (x == 0x0F)
        s3m_slide(ch, (long)y << 2);
      else if (x == 0x0E)
        s3m_slide(ch, (long)y);
      break;
    case 6: /* Fxx: up */
      if (x == 0x0F)
        s3m_slide(ch, -((long)y << 2));
      else if (x == 0x0E)
        s3m_slide(ch, -(long)y);
      break;
    case 7: /* Gxx */
      if (arg)
        c_tspd[ch] = arg;
      break;
    case 8: case 21: /* Hxy, Uxy: vibrato, fine vibrato */
      if (y)
        c_vib[ch] = (u8)((c_vib[ch] & 0xF0) | y);
      if (x)
        c_vib[ch] = (u8)((c_vib[ch] & 0x0F) | (arg & 0xF0));
      c_fine[ch] = (u8)(fx == 21);
      break;
    case 9: /* Ixy: tremor */
      if (c_tremor[ch] >= x + y + 2)
        c_tremor[ch] = 0;
      c_troff[ch] = (u8)(c_tremor[ch] > x);
      c_tremor[ch]++;
      break;
    case 17: /* Qxy: a row with a note starts counting from it */
      if (n_note[ch] < 254)
        c_rtg[ch] = y;
      else if (c_rtg[ch] == 0 || --c_rtg[ch] == 0) {
        c_rtg[ch] = y;
        s3m_retrig_volume(ch, x);
        start = 1;
      }
      break;
    case 18: /* Rxy: tremolo */
      if (y)
        c_trm[ch] = (u8)((c_trm[ch] & 0xF0) | y);
      if (x)
        c_trm[ch] = (u8)((c_trm[ch] & 0x0F) | (arg & 0xF0));
      tremolo_at(ch, 0);
      break;
    case 19:
      switch (x) {
      case 0x2: /* a rate from the finetune table */
        c_c5[ch] = s3m_fine[y];
        if (start)
          c_per[ch] = s3m_period(c_note[ch], c_c5[ch]);
        break;
      case 0x3:
        c_wave[ch] = (u8)((c_wave[ch] & 0xF0) | y);
        break;
      case 0x4:
        c_wave[ch] = (u8)((c_wave[ch] & 0x0F) | (y << 4));
        break;
      case 0x8:
        c_pan[ch] = (u8)(y * 17);
        wr(V(ch) + PAN, c_pan[ch]);
        break;
      case 0xB:
        if (y == 0)
          c_loopstart[ch] = row;
        else if (c_loopcnt[ch] == 0) {
          c_loopcnt[ch] = y;
          do_loop = 1;
          loop_row = c_loopstart[ch];
        }
        else if (--c_loopcnt[ch]) {
          do_loop = 1;
          loop_row = c_loopstart[ch];
        }
        break;
      case 0xE:
        patdelay = y;
        break;
      default:
        break;
      }
      break;
    case 20: /* Txx */
      if (arg >= 0x20)
        set_bpm(arg);
      break;
    case 22: /* Vxx */
      if (arg <= 64) {
        gvol = arg;
        levels = 1;
      }
      break;
    case 24: /* Xxx: 00 left to 80 right */
      if (arg <= 0x80) {
        c_pan[ch] = (u8)(arg << 1);
        if (arg == 0x80)
          c_pan[ch] = 255;
        wr(V(ch) + PAN, c_pan[ch]);
      }
      break;
    default:
      break;
    }

    if (fx == 8 || fx == 21 || fx == 11)
      vibrato(ch, 0); /* on a row's first tick it sounds where it stands */
    if (start)
      play(ch, offs);
    else
      flush(ch);
  }
  if (levels)
    all_levels();
}

static void s3m_effects(void)
{
  u8 ch, arg, x, y;

  for (ch = 0; ch < channels; ch++) {
    arg = c_arg[ch];
    x = (u8)(arg >> 4);
    y = (u8)(arg & 0x0F);
    switch (c_fx[ch]) {
    case 4:
      s3m_volslide(ch, arg, 0, 0);
      break;
    case 5:
      if (x < 0x0E)
        s3m_slide(ch, (long)arg << 2);
      break;
    case 6:
      if (x < 0x0E)
        s3m_slide(ch, -((long)arg << 2));
      break;
    case 7:
      tone_portamento(ch);
      break;
    case 8: case 21:
      vibrato(ch, 1);
      break;
    case 9:
      if (c_tremor[ch] >= x + y + 2)
        c_tremor[ch] = 0;
      c_troff[ch] = (u8)(c_tremor[ch] > x);
      c_tremor[ch]++;
      break;
    case 10: /* Jxy */
      switch (tickno % 3) {
      case 1:
        c_arp[ch] = x;
        break;
      case 2:
        c_arp[ch] = y;
        break;
      default:
        c_arp[ch] = 0;
        break;
      }
      break;
    case 11:
      vibrato(ch, 1);
      s3m_volslide(ch, arg, 0, 0);
      break;
    case 12:
      tone_portamento(ch);
      s3m_volslide(ch, arg, 0, 0);
      break;
    case 17:
      if (c_rtg[ch] == 0 || --c_rtg[ch] == 0) {
        c_rtg[ch] = y;
        s3m_retrig_volume(ch, x);
        play(ch, 0);
        continue;
      }
      break;
    case 18:
      tremolo(ch);
      break;
    case 19:
      if (x == 0x0C && y && tickno == y) /* SCx: silent from tick x */
        c_vol[ch] = 0;
      else if (x == 0x0D && y && tickno == y) { /* SDx: the row's note, now */
        if (s3m_note(ch, 0)) {
          play(ch, 0);
          continue;
        }
      }
      break;
    default:
      break;
    }
    flush(ch);
  }
}

#ifdef ON_MEGA65
/* A row of a MOD or an S3M for the pattern screen, into d_note[] and the rest */
static void ms_cells(u8 r)
{
  u8 ch, n, fx;
  const u8 *p;
  u16 per;

  if (fmt == F_MOD) {
    mg_get(rowbuf, ATTIC + 1084UL + ((u32)orders[order] * 64UL + r) * (u32)(channels * 4), (u16)(channels * 4));
    for (ch = 0; ch < channels; ch++) {
      p = rowbuf + ch * 4;
      per = (u16)(((u16)(p[0] & 0x0F) << 8) | p[1]);
      d_note[ch] = 0;
      if (per)
        d_note[ch] = (u8)(note_of(per) + 1);
      d_ins[ch] = (u8)((p[0] & 0xF0) | (p[2] >> 4));
      d_vol[ch] = 0xFF;
      fx = (u8)(p[2] & 0x0F);
      d_arg[ch] = p[3];
      d_fx[ch] = 0;
      if (fx >= 10)
        d_fx[ch] = (u8)(0x41 - 10 + fx);
      else if (fx || p[3])
        d_fx[ch] = (u8)(0x30 + fx);
    }
    return;
  }
  memset(d_note, 255, sizeof d_note);
  memset(d_ins, 0, sizeof d_ins);
  memset(d_vol, 255, sizeof d_vol);
  memset(d_fx, 0, sizeof d_fx);
  if (pat_at) {
    while (rows_known <= r)
      s3m_parse((u8)(rows_known - 1), 0);
    s3m_parse(r, 2);
  }
  for (ch = 0; ch < channels; ch++) {
    n = d_note[ch];
    if (n == 255)
      n = 0;
    else if (n != 254)
      n = (u8)((n >> 4) * 12 + (n & 0x0F) + 1);
    d_note[ch] = n;
    if (d_fx[ch])
      d_fx[ch] += 0x40; /* A is 1 */
  }
}
#endif

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

static void all_levels(void)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++)
    flush(ch);
}

/* ------------------------------------------------------------------------------------------
   An XM's rows and commands, as FastTracker 2 plays them
   ------------------------------------------------------------------------------------------ */
#ifdef ON_MEGA65
#pragma code-name (push, "OVERLAY4")
#pragma rodata-name (push, "OVERLAY4")
#endif
/* which of c_mem[] a command's argument is remembered in */
#define M_UP 0
#define M_DOWN 1
#define M_VOL 2
#define M_FUP 3
#define M_FDOWN 4
#define M_FVUP 5
#define M_FVDOWN 6
#define M_GVOL 7
#define M_PAN 8
#define M_RTG 9
#define M_TREMOR 10
#define M_XUP 11
#define M_XDOWN 12

static u8 envbuf[48];
static u8 xm_gl; /* the song's volume changed in this tick */

static void xm_pattern(void)
{
  u16 n;
  pat_at = 0;
  if (orders[order] < npatterns) {
    mg_get(regs, XPAT + ((u32)orders[order] << 3), 8);
    n = get16(regs + 4);
    if (n && n <= 256)
      pat_rows = n;
    if (get16(regs + 6) && get32(regs) < filesize)
      pat_at = get32(regs);
  }
  rowofs[0] = 0;
  rows_known = 1;
}

/* Go through row r; keep: and take it into the channels */
static void xm_parse(u8 r, u8 keep)
{
  u32 at = pat_at + rowofs[r];
  u16 n, i = 0;
  u8 ch, b, k;
  static u8 cell[5];

  if (at >= filesize)
    return;
  n = (u16)(filesize - at > 200UL ? 200UL : filesize - at);
  mg_get(rowbuf, ATTIC + at, n);
  for (ch = 0; ch < channels && i < n; ch++) {
    b = rowbuf[i++];
    if (b & 0x80) {
      for (k = 0; k < 5; k++) {
        cell[k] = 0;
        if (b & (1 << k))
          cell[k] = rowbuf[i++];
      }
    }
    else {
      cell[0] = b;
      memcpy(cell + 1, rowbuf + i, 4);
      i += 4;
    }
    if (keep == 2) { /* for the screen */
      d_note[ch] = cell[0];
      d_ins[ch] = cell[1];
      d_vol[ch] = cell[2];
      d_fx[ch] = cell[3];
      d_arg[ch] = cell[4];
    }
    else if (keep) {
      n_note[ch] = cell[0];
      n_ins[ch] = cell[1];
      n_vol[ch] = cell[2];
      c_fx[ch] = cell[3];
      c_arg[ch] = cell[4];
    }
  }
  if ((u16)r + 1 < pat_rows) {
    rowofs[r + 1] = (u16)(rowofs[r] + i);
    if (rows_known < (u16)r + 2)
      rows_known = (u16)(r + 2);
  }
}

static void xm_read(u8 r)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++) {
    n_note[ch] = 0;
    n_ins[ch] = 0;
    n_vol[ch] = 0;
    c_fx[ch] = 0;
    c_arg[ch] = 0;
  }
  if (!pat_at)
    return;
  while (rows_known <= r) /* jumped past rows not yet seen: find where they begin */
    xm_parse((u8)(rows_known - 1), 0);
  xm_parse(r, 1);
}

/* An envelope has come to the tick e_pos: to a point of it, when it got there by itself. What
   it does from here. */
static void env_arrive(u8 ch, u8 w)
{
  const u8 *q = x_p[ch];
  u8 n = q[w], type = q[8 + w];
  u8 sus = q[2 + 3 * w], ls = q[3 + 3 * w], le = q[4 + 3 * w];
  u16 pos = e_pos[w][ch], x0, x1;
  u8 k, y0, y1, looped = 0;

  if (n > 12)
    n = 12;
  e_delta[w][ch] = 0;
  if (n == 0 || x_iat[ch] == 0) {
    e_hold[w][ch] = 2;
    return;
  }
  mg_get(envbuf, ATTIC + x_iat[ch] + (w ? 177UL : 129UL), 48);
  for (k = (u8)(n - 1); k && get16(envbuf + (k << 2)) > pos; k--) /* the point at or before pos */
    ;
  for (;;) {
    x0 = get16(envbuf + (k << 2));
    y0 = envbuf[(k << 2) + 2];
    if (y0 > 64)
      y0 = 64;
    e_val[w][ch] = (long)y0 << 16;
    if (x0 == pos || k == n - 1) { /* at a point */
      /* The end of the loop: back to its start. Where that is the sustain point too, the
         loop goes round while the key is down, and the envelope goes on when it is up. */
      if ((type & 4) && k == le && !looped && !((type & 2) && x_key[ch] && le == sus)) {
        if (ls >= le || ls >= n) { /* a loop of one point: it stays */
          e_hold[w][ch] = 2;
          return;
        }
        k = ls;
        pos = get16(envbuf + (k << 2));
        e_pos[w][ch] = pos;
        looped = 1;
        continue;
      }
      if ((type & 2) && !x_key[ch] && k == sus) { /* held here while the key is down */
        e_hold[w][ch] = 1;
        e_pos[w][ch] = x0;
        return;
      }
      if (k == n - 1) {
        e_hold[w][ch] = 2;
        return;
      }
    }
    break;
  }
  x1 = get16(envbuf + (k << 2) + 4);
  y1 = envbuf[(k << 2) + 6];
  if (y1 > 64)
    y1 = 64;
  if (x1 <= x0) {
    e_hold[w][ch] = 2;
    return;
  }
  e_delta[w][ch] = (((long)y1 - (long)y0) << 16) / (long)(x1 - x0);
  if (pos > x0)
    e_val[w][ch] += e_delta[w][ch] * (long)(pos - x0);
  e_next[w][ch] = x1;
}

static void env_tick(u8 ch, u8 w)
{
  if (!(x_p[ch][8 + w] & 1) || e_hold[w][ch])
    return;
  if (++e_pos[w][ch] != e_next[w][ch])
    e_val[w][ch] += e_delta[w][ch];
  else
    env_arrive(ch, w);
}

/* a quarter of a sine, 64 high */
static const u8 qsine[65] = { 0,  2,  3,  5,  6,  8,  9,  11, 12, 14, 16, 17, 19, 20, 22, 23, 24, 26, 27, 29, 30, 32,
                              33, 34, 36, 37, 38, 39, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56,
                              56, 57, 58, 59, 59, 60, 60, 61, 61, 62, 62, 62, 63, 63, 63, 64, 64, 64, 64, 64, 64 };

/* The channel's tick, after its row or its commands: the fade, the envelopes and the
   instrument's own vibrato move on, and the voice is told. start: its sample begins. */
static void xm_update(u8 ch, u8 start, u32 offs)
{
  const u8 *q = x_p[ch];
  u16 f, amp;
  s16 v, t;
  u8 pos;

  if (x_key[ch]) {
    f = get16(q + 14);
    x_fade[ch] = (u16)(x_fade[ch] > f ? x_fade[ch] - f : 0);
  }
  env_tick(ch, 0);
  env_tick(ch, 1);
  if (q[12]) {
    /* While it is still growing, FastTracker's vibrato is as deep as it has grown only with
       the key down: let go before it is full, it all but stops. */
    amp = x_avamp[ch];
    if (x_avsweep[ch]) {
      amp = x_avsweep[ch];
      if (!x_key[ch]) {
        amp += x_avamp[ch];
        if ((amp >> 8) > q[12]) {
          amp = (u16)q[12] << 8;
          x_avsweep[ch] = 0;
        }
        x_avamp[ch] = amp;
      }
    }
    pos = x_avpos[ch] = (u8)(x_avpos[ch] + q[13]);
    switch (q[10] & 3) {
    case 1: /* square */
      v = 64;
      if (pos < 128)
        v = -64;
      break;
    case 2: /* ramp up */
      v = (s16)(((pos >> 1) + 64) & 127) - 64;
      break;
    case 3: /* ramp down */
      v = (s16)((64 - (pos >> 1)) & 127) - 64;
      break;
    default: /* a sine that begins by going up in pitch */
      if (pos < 64)
        v = -(s16)qsine[pos];
      else if (pos < 128)
        v = -(s16)qsine[128 - pos];
      else if (pos < 192)
        v = (s16)qsine[pos - 128];
      else
        v = (s16)qsine[(u8)(0 - pos)];
      break;
    }
    t = (s16)(v * (s16)(amp >> 4));
    if (t >= 0)
      c_avd[ch] = t >> 10;
    else
      c_avd[ch] = -((1023 - t) >> 10);
  }
  else
    c_avd[ch] = 0;
  if (start)
    play(ch, offs);
  else
    flush(ch);
}

static void xm_setvol(u8 ch, u8 v)
{
  c_vol[ch] = (u8)(v > 64 ? 64 : v);
  c_trmd[ch] = 0;
  c_troff[ch] = 0;
}

static void xm_volslide(u8 ch, s16 by)
{
  slide_volume(ch, by);
  c_trmd[ch] = 0;
  c_troff[ch] = 0;
}

static void xm_panslide(u8 ch, s16 by)
{
  s16 p = (s16)c_pan[ch] + by;
  if (p < 0)
    p = 0;
  if (p > 255)
    p = 255;
  c_pan[ch] = (u8)p;
}

static void xm_slide(u8 ch, s16 by)
{
  long p;
  if (c_per[ch] == 0)
    return;
  p = (long)c_per[ch] + by;
  if (p < 1)
    p = 1;
  if (p > 31999L)
    p = 31999L;
  c_per[ch] = (u16)p;
  c_vibd[ch] = 0;
}

/* The key goes up: with no volume envelope the note is over; with one, envelopes that stood at
   their sustain point go on, and the fade begins */
static void xm_keyoff(u8 ch)
{
  u8 w;
  x_key[ch] = 1;
  if (!(x_p[ch][8] & 1))
    xm_setvol(ch, 0);
  for (w = 0; w < 2; w++)
    if (e_hold[w][ch] == 1) { /* the tick that comes now is the sustain point's again */
      e_hold[w][ch] = 0;
      e_next[w][ch] = e_pos[w][ch];
      e_pos[w][ch]--;
    }
}

/* An instrument number: the volume and the place of the sample that is playing, as new */
static void xm_revol(u8 ch)
{
  u8 smp = c_smp[ch];
  if (!smp)
    return;
  xm_setvol(ch, s_vol[smp]);
  c_pan[ch] = s_pan[smp];
}

/* and the envelopes, the vibratos and the fade from their beginning, the key down */
static void xm_retrig(u8 ch)
{
  const u8 *q = x_p[ch];
  u8 w;
  if (!(c_wave[ch] & 0x04))
    c_vibpos[ch] = 0;
  if (!(c_wave[ch] & 0x40))
    c_trmpos[ch] = 0;
  c_tremor[ch] = 0;
  x_key[ch] = 0;
  x_fade[ch] = 32768U;
  for (w = 0; w < 2; w++) {
    e_hold[w][ch] = 0;
    e_pos[w][ch] = 0xFFFFU;
    e_next[w][ch] = 0;
  }
  if (q[12]) {
    x_avpos[ch] = 0;
    x_avamp[ch] = (u16)q[12] << 8;
    x_avsweep[ch] = 0;
    if (q[11]) {
      x_avsweep[ch] = (u16)(x_avamp[ch] / q[11]);
      x_avamp[ch] = 0;
    }
  }
}

/* A note for the channel's instrument (0: the note it had): which sample that is, and its
   period. ft: a finetune from the row, or 0xFF. Returns 1 if the sample is to start. */
static u8 xm_tone(u8 ch, u8 note, u8 ft)
{
  u8 ins = x_ins[ch], smp = 0, k;
  s16 real;

  if (note == 0) {
    note = c_note[ch];
    if (note == 0)
      return 0;
  }
  c_note[ch] = note;
  x_iat[ch] = 0;
  memset(x_p[ch], 0, 16);
  if (ins && ins <= xm_nins) {
    mg_get(regs, XINS + ((u32)ins << 3), 8);
    if (regs[5]) {
      x_iat[ch] = get32(regs);
      mg_get(x_p[ch], ATTIC + x_iat[ch] + 225UL, 16);
      k = 0;
      if (regs[5] > 1) /* more than one sample: its table says which plays this note */
        k = (u8)(mg_peek(ATTIC + x_iat[ch] + 32UL + note) & 15);
      if (k < regs[5])
        smp = (u8)(regs[4] + k);
    }
  }
  c_smp[ch] = smp;
  if (smp == 0) { /* an instrument with nothing to play: the voice stops */
    c_play[ch] = 0;
    return 1;
  }
  real = (s16)note - 1 + (s8)s_rel[smp];
  if (real < 0 || real > 118)
    return 0;
  c_ft[ch] = (u8)(ft == 0xFF ? s_ft[smp] : ft);
  c_per[ch] = xm_note_period((u8)real, c_ft[ch]);
  c_vibd[ch] = 0;
  return 1;
}

/* Rxy: every y ticks the note again, at a volume changed by x. The ticks are counted while
   the command is there, over rows and past notes; a row with an instrument number begins at
   one. With a volume in the volume column a row's first tick is passed over, and a row's own
   note is not played twice on it. (As libopenmpt has it, measured.) */
static u8 xm_multi(u8 ch, u8 first)
{
  u8 a = c_arg[ch], vol = n_vol[ch], start = 0;

  if (first) {
    if (vol >= 0x10 && vol <= 0x50)
      return 0;
    if (n_ins[ch] && n_note[ch] != 97)
      c_rtg[ch] = 1;
  }
  if (c_rtg[ch] >= (a & 0x0F) && !(first && n_note[ch])) {
    c_rtg[ch] = 0;
    s3m_retrig_volume(ch, (u8)(a >> 4));
    c_trmd[ch] = 0;
    if (vol >= 0x10 && vol <= 0x50)
      xm_setvol(ch, (u8)(vol - 0x10));
    else if ((vol >> 4) == 0x0C)
      c_pan[ch] = (u8)((vol & 15) << 4);
    start = xm_tone(ch, 0, 0xFF);
  }
  c_rtg[ch]++;
  return start;
}

/* the command's argument, or the one it remembers */
static u8 xm_arg(u8 ch, u8 slot)
{
  u8 a = c_arg[ch];
  if (a)
    c_mem[slot][ch] = a;
  else
    a = c_mem[slot][ch];
  c_arg[ch] = a;
  return a;
}

/* the same for the commands whose argument is the lower half: Exy, Xxy */
static u8 xm_low(u8 ch, u8 slot)
{
  u8 a = (u8)(c_arg[ch] & 0x0F);
  if (a)
    c_mem[slot][ch] = a;
  else
    a = c_mem[slot][ch];
  return a;
}

/* What a row's volume column and command do on its first tick */
static void xm_first(u8 ch)
{
  u8 fx = c_fx[ch], arg = c_arg[ch], vol = n_vol[ch];
  u8 x = (u8)(arg >> 4), y = (u8)(arg & 0x0F), vy = (u8)(vol & 0x0F);

  if (vol >= 0x10 && vol <= 0x50)
    xm_setvol(ch, (u8)(vol - 0x10));
  else
    switch (vol >> 4) {
    case 0x8:
      xm_volslide(ch, -(s16)vy);
      break;
    case 0x9:
      xm_volslide(ch, (s16)vy);
      break;
    case 0xA:
      if (vy)
        c_vib[ch] = (u8)((c_vib[ch] & 0x0F) | (vy << 4));
      break;
    case 0xB:
      if (vy)
        c_vib[ch] = (u8)((c_vib[ch] & 0xF0) | vy);
      break;
    case 0xC:
      c_pan[ch] = (u8)(vy << 4);
      break;
    default:
      break;
    }

  switch (fx) {
  case 0x04:
    if (y)
      c_vib[ch] = (u8)((c_vib[ch] & 0xF0) | y);
    if (x)
      c_vib[ch] = (u8)((c_vib[ch] & 0x0F) | (arg & 0xF0));
    break;
  case 0x07:
    if (y)
      c_trm[ch] = (u8)((c_trm[ch] & 0xF0) | y);
    if (x)
      c_trm[ch] = (u8)((c_trm[ch] & 0x0F) | (arg & 0xF0));
    break;
  case 0x08:
    c_pan[ch] = arg;
    break;
  case 0x0B:
    jump_order = arg;
    do_jump = 1;
    break;
  case 0x0C:
    xm_setvol(ch, arg);
    break;
  case 0x0D:
    break_row = (u8)(x * 10 + y);
    do_break = 1;
    break;
  case 0x0E:
    switch (x) {
    case 0x1:
      xm_slide(ch, -(s16)xm_low(ch, M_FUP) * 4);
      break;
    case 0x2:
      xm_slide(ch, (s16)xm_low(ch, M_FDOWN) * 4);
      break;
    case 0x4:
      c_wave[ch] = (u8)((c_wave[ch] & 0xF0) | y);
      break;
    case 0x6:
      if (y == 0)
        c_loopstart[ch] = row;
      else if (c_loopcnt[ch] == 0) {
        c_loopcnt[ch] = y;
        do_loop = 1;
        loop_row = c_loopstart[ch];
      }
      else if (--c_loopcnt[ch]) {
        do_loop = 1;
        loop_row = c_loopstart[ch];
      }
      break;
    case 0x7:
      c_wave[ch] = (u8)((c_wave[ch] & 0x0F) | (y << 4));
      break;
    case 0xA:
      xm_volslide(ch, (s16)xm_low(ch, M_FVUP));
      break;
    case 0xC:
      if (y == 0)
        xm_setvol(ch, 0);
      break;
    case 0xB:
      xm_volslide(ch, -(s16)xm_low(ch, M_FVDOWN));
      break;
    case 0xE:
      patdelay = y;
      break;
    default:
      break;
    }
    break;
  case 0x0F:
    if (arg >= 32)
      set_bpm(arg);
    else if (arg)
      speed = arg;
    break;
  case 0x10: /* Gxx */
    gvol = (u8)(arg > 64 ? 64 : arg);
    xm_gl = 1;
    break;
  case 0x15: /* Lxx: the volume envelope goes on from tick xx; the panning envelope too, but
                only where the volume envelope has a sustain point (FastTracker's mistake) */
    e_hold[0][ch] = 0;
    e_next[0][ch] = arg;
    e_pos[0][ch] = (u16)(arg - 1);
    if (x_p[ch][8] & 2) {
      e_hold[1][ch] = 0;
      e_next[1][ch] = arg;
      e_pos[1][ch] = (u16)(arg - 1);
    }
    break;
  case 0x21: /* X1x, X2x: the finest slides */
    if (x == 1)
      xm_slide(ch, -(s16)xm_low(ch, M_XUP));
    else if (x == 2)
      xm_slide(ch, (s16)xm_low(ch, M_XDOWN));
    break;
  default:
    break;
  }
}

static void xm_row(void)
{
  u8 ch, fx, arg, x, y, note, ins, vol, start, a;
  u32 offs;

  xm_read(row);
  do_jump = do_break = do_loop = 0;
  break_row = 0;
  xm_gl = 0;

  for (ch = 0; ch < channels; ch++) {
    fx = c_fx[ch];
    arg = c_arg[ch];
    note = n_note[ch];
    ins = n_ins[ch];
    vol = n_vol[ch];
    x = (u8)(arg >> 4);
    y = (u8)(arg & 0x0F);
    start = 0;
    offs = 0;
    c_arp[ch] = 0;
    c_vibd[ch] = 0;
    if (fx != 7)
      c_trmd[ch] = 0;
    if (fx != 0x1D)
      c_troff[ch] = 0;

    switch (fx) { /* the commands that remember their argument */
    case 0x01:
      xm_arg(ch, M_UP);
      break;
    case 0x02:
      xm_arg(ch, M_DOWN);
      break;
    case 0x05: case 0x06: case 0x0A:
      xm_arg(ch, M_VOL);
      break;
    case 0x11:
      xm_arg(ch, M_GVOL);
      break;
    case 0x19:
      xm_arg(ch, M_PAN);
      break;
    case 0x1D:
      xm_arg(ch, M_TREMOR);
      break;
    case 0x1B: /* Rxy remembers its halves apart */
      a = c_mem[M_RTG][ch];
      if (y)
        a = (u8)((a & 0xF0) | y);
      if (x)
        a = (u8)((a & 0x0F) | (arg & 0xF0));
      c_mem[M_RTG][ch] = a;
      c_arg[ch] = a;
      break;
    default:
      break;
    }

    if (ins > 128)
      ins = 0;
    if (ins)
      x_ins[ch] = ins;
    if (!(fx == 0x0E && x == 0x0D && y)) { /* EDx: the note, and all the row does, comes later */
      if ((vol >> 4) == 0x0F || fx == 3 || fx == 5) {
        /* a slide to the note: of the playing sample, which goes on */
        if ((vol >> 4) == 0x0F) {
          if (vol & 0x0F)
            c_tspd[ch] = (u8)((vol & 0x0F) << 4);
        }
        else if (fx == 3 && arg)
          c_tspd[ch] = arg;
        if (note == 97)
          xm_keyoff(ch);
        else if (note && c_play[ch]) {
          a = (u8)(note - 1 + (s8)s_rel[c_play[ch]]);
          if (a < 119) {
            c_target[ch] = xm_note_period(a, c_ft[ch]);
            c_note[ch] = note; /* the one a retrigger then plays */
            /* a voice that has stopped (its sample played out) starts again, at the pitch
               the slide begins from: as libopenmpt has it, not FastTracker */
            if (!(rd(V(ch) + CTRL) & RUN))
              start = 1;
          }
        }
        if (ins) {
          xm_revol(ch);
          if (note != 97)
            xm_retrig(ch);
        }
      }
      else if (fx == 0x14 && arg == 0) { /* K00 */
        xm_keyoff(ch);
        if (ins)
          xm_revol(ch);
      }
      else if (note == 0 && !(fx == 0x0E && arg == 0x90)) {
        if (ins) {
          xm_revol(ch);
          xm_retrig(ch);
        }
      }
      else {
        if (note == 97)
          xm_keyoff(ch);
        else {
          a = 0xFF;
          if (fx == 0x0E && x == 5) /* E5x: its own finetune for this note */
            a = (u8)((y << 4) - 128);
          start = xm_tone(ch, note, a);
          if (fx == 9) {
            if (arg)
              c_offs[ch] = arg;
            offs = (u32)c_offs[ch] << 8;
          }
        }
        if (ins) {
          xm_revol(ch);
          if (note != 97)
            xm_retrig(ch);
        }
      }
    }
    if (!(fx == 0x0E && x == 0x0D && y)) {
      xm_first(ch);
      /* a vibrato and a tremolo sound on a row's first tick where they stand */
      if (fx == 4 || fx == 6 || (vol >> 4) == 0x0B)
        vibrato(ch, 0);
      if (fx == 7)
        tremolo_at(ch, 0);
      if (fx == 0x1B && xm_multi(ch, 1)) {
        start = 1;
        offs = 0;
      }
    }
    xm_update(ch, start, offs);
  }
  if (xm_gl)
    all_levels();
}

/* The ticks of a row after its first */
static void xm_effects(void)
{
  u8 ch, fx, arg, x, y, vol, vy, start, k;

  xm_gl = 0;
  for (ch = 0; ch < channels; ch++) {
    fx = c_fx[ch];
    arg = c_arg[ch];
    x = (u8)(arg >> 4);
    y = (u8)(arg & 0x0F);
    vol = n_vol[ch];
    vy = (u8)(vol & 0x0F);
    start = 0;

    switch (vol >> 4) { /* the volume column */
    case 0x6:
      xm_volslide(ch, -(s16)vy);
      break;
    case 0x7:
      xm_volslide(ch, (s16)vy);
      break;
    case 0xB:
      vibrato(ch, 1);
      break;
    case 0xD:
      xm_panslide(ch, -(s16)vy);
      break;
    case 0xE:
      xm_panslide(ch, (s16)vy);
      break;
    case 0xF:
      tone_portamento(ch);
      c_vibd[ch] = 0;
      break;
    default:
      break;
    }

    switch (fx) {
    case 0x00:
      if (arg) { /* FastTracker counts the ticks down: which step sounds depends on the speed */
        k = (u8)(speed - tickno);
        if (k > 16)
          k = 2;
        else if (k == 16)
          k = 0;
        else
          k %= 3;
        c_arp[ch] = 0;
        if (k == 1)
          c_arp[ch] = x;
        else if (k == 2)
          c_arp[ch] = y;
      }
      break;
    case 0x01:
      xm_slide(ch, -(s16)arg * 4);
      break;
    case 0x02:
      xm_slide(ch, (s16)arg * 4);
      break;
    case 0x03:
      tone_portamento(ch);
      c_vibd[ch] = 0;
      break;
    case 0x04:
      vibrato(ch, 1);
      break;
    case 0x05:
      tone_portamento(ch);
      c_vibd[ch] = 0;
      break;
    case 0x06:
      vibrato(ch, 1);
      break;
    case 0x07:
      tremolo(ch);
      break;
    case 0x0E:
      if (x == 0x9) { /* the note again every y ticks */
        if (y && tickno % y == 0) {
          start = xm_tone(ch, 0, 0xFF);
          xm_retrig(ch);
        }
      }
      else if (x == 0xC) {
        if (tickno == y)
          xm_setvol(ch, 0);
      }
      else if (x == 0xD && tickno == y) { /* the row's note, now */
        if (n_note[ch] == 97)
          xm_keyoff(ch);
        else {
          start = xm_tone(ch, n_note[ch], 0xFF);
          if (n_ins[ch])
            xm_revol(ch);
          xm_retrig(ch);
        }
        if (vol >= 0x10 && vol <= 0x50)
          xm_setvol(ch, (u8)(vol - 0x10));
        else if ((vol >> 4) == 0x0C)
          c_pan[ch] = (u8)(vy << 4);
      }
      break;
    case 0x11: /* Hxy: the song's volume slides */
      if (x) {
        gvol = (u8)(gvol + x);
        if (gvol > 64)
          gvol = 64;
      }
      else
        gvol = (u8)(gvol > y ? gvol - y : 0);
      xm_gl = 1;
      break;
    case 0x14: /* Kxx: the key goes up on tick xx */
      if (tickno == (arg & 31))
        xm_keyoff(ch);
      break;
    case 0x19: /* Pxy */
      if (x)
        xm_panslide(ch, (s16)x);
      else
        xm_panslide(ch, -(s16)y);
      break;
    case 0x1B:
      start = xm_multi(ch, 0);
      break;
    case 0x1D: /* Txy: on for x + 1 ticks, off for y + 1 */
      k = c_tremor[ch];
      if ((k & 0x7F) == 0) {
        if (k & 0x80)
          k = y;
        else
          k = (u8)(0x80 | x);
      }
      else
        k--;
      c_tremor[ch] = k;
      c_troff[ch] = (u8)!(k & 0x80);
      break;
    default:
      break;
    }
    if (fx == 5 || fx == 6 || fx == 0x0A) { /* a volume slide: up when it has both */
      if (x)
        xm_volslide(ch, (s16)x);
      else
        xm_volslide(ch, -(s16)y);
    }
    xm_update(ch, start, 0);
  }
  if (xm_gl)
    all_levels();
}

#ifdef ON_MEGA65
/* A row of an XM for the pattern screen, into d_note[] and the rest */
static void xm_cells(u8 r)
{
  u8 ch, fx;

  memset(d_note, 0, sizeof d_note);
  memset(d_ins, 0, sizeof d_ins);
  memset(d_vol, 0, sizeof d_vol);
  memset(d_fx, 0, sizeof d_fx);
  memset(d_arg, 0, sizeof d_arg);
  if (pat_at) {
    while (rows_known <= r)
      xm_parse((u8)(rows_known - 1), 0);
    xm_parse(r, 2);
  }
  for (ch = 0; ch < channels; ch++) {
    if (d_note[ch] == 97)
      d_note[ch] = 253;
    if (d_vol[ch] == 0)
      d_vol[ch] = 0xFF;
    fx = d_fx[ch];
    if (fx >= 10)
      d_fx[ch] = (u8)(0x41 - 10 + fx);
    else if (fx || d_arg[ch])
      d_fx[ch] = (u8)(0x30 + fx);
  }
}
#endif

#ifdef ON_MEGA65
#pragma rodata-name (pop)
#pragma code-name (pop)
#endif

static void new_pattern(void)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++) {
    c_loopstart[ch] = 0;
    c_loopcnt[ch] = 0;
  }
  pat_rows = 64;
  if (fmt == F_S3M)
    s3m_pattern();
  else if (fmt == F_XM)
    xm_pattern();
  if (row >= pat_rows) /* a break to a row the pattern does not have */
    row = 0;
}

/* The position that plays when the song comes to o: an S3M has markers in its list, one to
   pass over (254) and one that ends the song (255) */
static u8 settle(u8 o)
{
  u16 tries;
  for (tries = 0; tries < 600; tries++) {
    if (o >= songlen || (fmt == F_S3M && orders[o] == 255)) {
      o = (u8)(fmt == F_XM ? xm_restart : 0);
      song_ended = 1;
    }
    else if (fmt == F_S3M && orders[o] == 254)
      o++;
    else
      break;
  }
  return o;
}

/* After a row's last tick: the next row */
static void advance(void)
{
  u8 next = order;
  if (do_loop) {
    row = loop_row;
    if (fmt == F_XM)
      xm_bp = loop_row;
    return;
  }
  if (do_jump || do_break) {
    next = do_jump ? jump_order : (u8)(order + 1);
    if (do_jump && jump_order <= order)
      song_ended = 1;
    row = do_break ? break_row : xm_bp;
    xm_bp = 0;
  }
  else if ((u16)row + 1 >= pat_rows) {
    row = xm_bp;
    xm_bp = 0;
    next = (u8)(order + 1);
  }
  else
    row++;
  if (next != order || do_jump || do_break) {
    order = settle(next);
    new_pattern();
  }
}

static void row_mark(void); /* the machine's: a new row is about to play */

/* One tick of the song */
static void tick(void)
{
  if (tickno == 0 && !in_delay) {
    row_mark();
    if (fmt == F_S3M)
      s3m_row();
    else if (fmt == F_XM)
      xm_row();
    else
      do_row();
  }
  else if (fmt == F_S3M)
    s3m_effects();
  else if (fmt == F_XM)
    xm_effects();
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

/* Which clock this engine gives the player */
static void find_clock(u8 want_voice)
{
  own_clock = (u8)(rd(G_VERSION) >= 2 && !want_voice);
  maxch = (u8)(own_clock ? 32 : 31);
}

/* The engine and the player ready for the song whose samples are in the sample RAM */
static void song_start(void)
{
  u8 ch, m;

  wr(G_CONTROL, 0x80);
  m = (u8)(128 / channels); /* all channels at full level on one side don't clip */
  if (fmt == F_S3M) { /* which they never are when there are many: louder than that */
    m = (u8)(96 / channels + 8);
    if (m > 64)
      m = 64;
  }
  if (fmt == F_XM) {
    /* An XM's voice in the middle is at 0.71 of full on each side: up to four channels can't
       clip at this. More of them get what the loudest of the modules tried could take. */
    m = (u8)(181 / channels);
    if (m < 34)
      m = 34;
    if (m > 64)
      m = 64;
  }
  if (m < 2)
    m = 2;
  master = master_own = m;
  wr(G_MASTER, m);

  memset(x_ins, 0, sizeof x_ins);
  memset(x_iat, 0, sizeof x_iat);
  memset(x_p, 0, sizeof x_p);
  memset(x_key, 0, sizeof x_key);
  memset(e_hold, 0, sizeof e_hold);
  memset(e_val, 0, sizeof e_val);
  memset(c_avd, 0, sizeof c_avd);
  memset(o_pan, 0, sizeof o_pan);

  memset(c_note, 0, sizeof c_note);
  memset(c_c5, 0, sizeof c_c5);
  memset(c_mem, 0, sizeof c_mem);
  memset(c_fine, 0, sizeof c_fine);
  memset(c_rtg, 0, sizeof c_rtg);
  memset(c_tremor, 0, sizeof c_tremor);
  memset(c_troff, 0, sizeof c_troff);
  memset(c_cut, 0, sizeof c_cut);

  memset(c_smp, 0, sizeof c_smp);
  memset(c_play, 0, sizeof c_play);
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
  if (fmt == F_S3M)
    memcpy(c_pan, s3m_pan, channels);
  if (fmt == F_XM)
    memset(c_pan, 128, sizeof c_pan);

  per_min = protracker ? 113 * 4 : 28 * 4;
  per_max = protracker ? 856 * 4 : 3424 * 4;
  row = tickno = 0;
  xm_bp = 0;
  order = settle(0);
  new_pattern();
  patdelay = in_delay = 0;
  song_ended = 0;
  begun = 0;
  gvol = 64;
  freq_k = 19370776UL; /* an Amiga's clock (PAL), for periods in quarters */
  speed_cia = speed_vbl = 6;
  bpm_cia = 125;
  bpm = 0;
  use_timing();
  if (fmt == F_S3M) {
    gvol = s3m_gvol;
    freq_k = 19548100UL; /* 8363 Hz at period 1712 */
    speed = s3m_speed;
    set_bpm(s3m_tempo);
  }
  if (fmt == F_XM) {
    freq_k = 19548100UL;
    speed = xm_speed;
    set_bpm(xm_tempo);
  }

  if (!own_clock) {
    /* the clock: silent, a byte a frame round the first 64 KB */
    memset(regs, 0, 16);
    regs[6] = 1;  /* FREQ $010000 */
    regs[15] = 1; /* END $010000 */
    mg_put(MG_REG + V(CLOCK) + PAN, regs + 1, 15);
    wr(V(CLOCK) + CTRL, RUN | LOOP);
  }
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
  /* In a MOD a new tempo counts from the tick after the one that sets it, as on an Amiga,
     whose timer takes its new period when the running one is over; but the song's first row
     sets the tempo the song begins with. In an S3M the tick that sets it is the first of the
     new length. */
  if (begun && fmt == F_MOD) {
    next_tick_length();
    tick();
  }
  else {
    tick();
    next_tick_length();
    begun = 1;
  }
  return 1;
}

/* ------------------------------------------------------------------------------------------
   The program
   ------------------------------------------------------------------------------------------ */
#ifdef ON_MEGA65

/* ------------------------------------------------------------------------------------------
   The screen: the VIC-IV's 80 columns, its memory in the MEGA65's bank 4. A row is put
   together in line[] as screen codes and copied there; this file's own texts are screen
   codes already (cbm_screen_charmap.h, for the set with both cases), a module's are ASCII.
   ------------------------------------------------------------------------------------------ */
#define SCREEN 0x40000UL
#define COLOURS 0xFF80000UL
#define STORE 0x50000UL /* bank 5: the overlays as they were read at the start */
#define FILES 0x41000UL /* the directory: ENTRY bytes a file */
#define ENTRY 72
#define E_ATTR 64
#define E_TYPE 65 /* 0: a directory, else F_MOD + 1 and so on */
#define E_SIZE 66
#define MAXFILES 250

#define ROWS_TOP 4  /* the rows a list or a view has: 4 to 22 */
#define ROWS 19
#define ROW_MSG 23
#define ROW_KEYS 24

#define BLACK 0
#define WHITE 1
#define RED 2
#define CYAN 3
#define GREEN 5
#define YELLOW 7
#define ORANGE 8
#define DGREY 11
#define GREY 12
#define LGREEN 13
#define LBLUE 14
#define LGREY 15

/* keys as the MEGA65's keyboard register has them: ASCII */
#define K_UP 0x91
#define K_DOWN 0x11
#define K_LEFT 0x9D
#define K_RIGHT 0x1D
#define K_RETURN 0x0D
#define K_ESC 0x1B
#define K_STOP 0x03
#define K_F1 0xF1
#define K_F3 0xF3
#define K_F5 0xF5
#define K_F7 0xF7

static u8 line[80];
static u8 lx;
static u8 ent[ENTRY];

static void mg_fill(u32 dst, u8 v, u16 n)
{
  far_dst(dst);
  dl.src_mb = 0;
  dl.src_bank = 0;
  dl.src = v;
  dl.count = n;
  dl.command = 3;
  dma();
  dl.command = 0;
}

#if defined(NOGUS) && defined(KEYS)
/* Keys an emulator presses by itself, one every so often (cl65 -DKEYS=0x11,0x0D,...), and at
   their end Xemu is told to stop (its -testing), which is when it takes its screenshot */
static const u8 script[] = { KEYS, 0 };
static u8 script_at;
static u16 script_wait;
#endif

static u8 key(void)
{
  u8 k = PEEK(0xD610U);
  if (k)
    POKE(0xD610U, 0);
#if defined(NOGUS) && defined(KEYS)
  if (++script_wait < 2500)
    return 0;
  script_wait = 0;
  k = script[script_at];
#ifdef RERUN
  if (k == 0x1B && PEEK(0x02A7U) == 0x55) /* the second time round: the end is here */
    k = 0;
#endif
  if (k == 0)
    POKE(0xD6CFU, 0x42);
  else
    script_at++;
#endif
  return k;
}

static u8 wait_key(void)
{
  u8 k;
  while ((k = key()) == 0)
    ;
  return k;
}

/* ASCII as a screen code */
static u8 sc(u8 c)
{
  if (c >= 0x61 && c <= 0x7A)
    return (u8)(c - 0x60);
  if (c == 0x40)
    return 0;
  if (c >= 0x5B && c <= 0x5F)
    return (u8)(c - 0x40);
  if (c < 0x20 || c > 0x7A)
    return 0x20;
  if (c == 0x60)
    return 0x27;
  return c;
}

static void l_new(void)
{
  memset(line, 0x20, 80);
  lx = 0;
}

/* one of the program's own texts */
static void l_text(const char *s)
{
  while (*s && lx < 80)
    line[lx++] = (u8)*s++;
}

/* n characters of a file's text: where it ends, blanks */
static void l_file(const u8 *s, u8 n)
{
  u8 c = 1;
  for (; n && lx < 80; n--) {
    if (c)
      c = *s++;
    line[lx++] = sc(c);
  }
}

/* a number, to the right in w places; fill: what goes before it */
static void l_num(u16 v, u8 w, u8 fill)
{
  u8 i = w;
  do {
    line[lx + --i] = (u8)(0x30 + v % 10);
    v /= 10;
  } while (v && i);
  while (i)
    line[lx + --i] = fill;
  lx += w;
}

static void l_hex1(u8 v)
{
  v &= 0x0F;
  line[lx++] = (u8)(v < 10 ? 0x30 + v : 0x41 - 10 + v);
}

static void l_hex(u8 v)
{
  l_hex1((u8)(v >> 4));
  l_hex1(v);
}

static void l_reverse(u8 x, u8 n)
{
  for (; n && x < 80; n--)
    line[x++] |= 0x80;
}

static void l_out(u8 y)
{
  mg_put(SCREEN + (u16)y * 80, line, 80);
}

static void colour(u8 y, u8 x, u8 n, u8 c)
{
  mg_fill(COLOURS + (u16)y * 80 + x, c, n);
}

/* a row of one text, in a colour */
static void say(u8 y, const char *s, u8 c)
{
  l_new();
  lx = 1;
  l_text(s);
  l_out(y);
  colour(y, 0, 80, c);
}

static void screen_on(void)
{
  POKE(0xD018U, 0x16);                           /* both cases; before the rest, which it resets */
  POKE(0xD031U, (u8)(PEEK(0xD031U) | 0x80));     /* 80 columns */
  POKE(0xD060U, 0x00);                           /* the screen's memory: $40000 */
  POKE(0xD061U, 0x00);
  POKE(0xD062U, 0x04);
  POKE(0xD063U, (u8)(PEEK(0xD063U) & 0xF0));
  POKE(0xD020U, BLACK);
  POKE(0xD021U, BLACK);
  mg_fill(SCREEN, 0x20, 2000);
  mg_fill(COLOURS, LGREY, 2000);
}

static void screen_off(void)
{
  POKE(0xD031U, (u8)(PEEK(0xD031U) & 0x7F));
  POKE(0xD018U, 0x14);
  POKE(0xD060U, 0x00);
  POKE(0xD061U, 0x04);
  POKE(0xD062U, 0x00);
  POKE(0xD020U, 14);
  POKE(0xD021U, 6);
  memset((u8 *)0x0400U, 0x20, 1000);
  memset((u8 *)0xD800U, 14, 1000);
  POKE(0x00C6U, 0); /* and no keys left over for BASIC */
}

/* the row at the top, the program's and something of the screen's */
static void title(const char *s)
{
  l_new();
  lx = 1;
  l_text("MegaGus Player");
  lx = 20;
  l_text(s);
  l_reverse(0, 80);
  l_out(0);
  colour(0, 0, 80, LBLUE);
}

static void keys_row(const char *s)
{
  say(ROW_KEYS, s, GREY);
}

static void message(const char *s)
{
  say(ROW_MSG, s, YELLOW);
}

/* ------------------------------------------------------------------------------------------
   The overlays: parts of the program that take turns in the same memory. Two places: one for
   the file list or the playing screens, one for the player of the module's kind. All four are
   read from the SD card when the program starts and kept in bank 5, so that a change of
   directory does not lose them.
   ------------------------------------------------------------------------------------------ */
#define OV_FILES 1
#define OV_SCREENS 2
#define OV_MOD 3
#define OV_XM 4

/* where the linker has each, and how far it goes */
extern u8 _OVL1_LAST__[], _OVL2_LAST__[], _OVL3_LAST__[], _OVL4_LAST__[];

static u8 ov_in[2]; /* which is in each place */
static char ov_name[12] = { 0x4D, 0x4F, 0x44, 0x50, 0x4C, 0x41, 0x59, 0x2E, 0x4F, 0x56, 0x31, 0 }; /* MODPLAY.OV1 */

static u8 *ov_at(u8 n)
{
  switch (n) {
  case 1:
    return _OVL1_START__;
  case 2:
    return _OVL2_START__;
  case 3:
    return _OVL3_START__;
  default:
    return _OVL4_START__;
  }
}

static u16 ov_size(u8 n)
{
  switch (n) {
  case 1:
    return (u16)(_OVL1_LAST__ - _OVL1_START__);
  case 2:
    return (u16)(_OVL2_LAST__ - _OVL2_START__);
  case 3:
    return (u16)(_OVL3_LAST__ - _OVL3_START__);
  default:
    return (u16)(_OVL4_LAST__ - _OVL4_START__);
  }
}

/* All four into bank 5, 16 KB apart: from the disk in the drive the program came from (a
   D81 with the five files on it), or, when the first is not there, from the SD card's
   directory. 0, or the one that is not there or is another build's: each begins with the
   address it loads to and then the build's number. */
static u8 fetch_overlays(void)
{
  u8 n, dev, disk = 1;
  unsigned char fd;
  u16 left, k;
  u32 to;

  dev = PEEK(0x00BAU);
  if (dev < 8)
    dev = 8;
  for (n = 1; n <= 4; n++) {
    ov_name[10] = (char)(0x30 + n);
    to = STORE + ((u32)(n - 1) << 14);
    if (disk) {
      /* the KERNAL loads it to where it runs, less the two bytes of its address */
      k = cbm_load(ov_name, dev, ov_at(n) + 2);
      POKE(0xD02FU, 0x47); /* the MEGA65's registers again, in case the drive's code hid them */
      POKE(0xD02FU, 0x53);
      if (k == ov_size(n) - 2)
        mg_put(to, ov_at(n), ov_size(n));
      else if (k == 0 && n == 1)
        disk = 0;
      else
        return n;
    }
    if (!disk) {
      closeall();
      fd = open(ov_name);
      if (fd == 0xFF)
        return n;
      for (left = ov_size(n); left; left -= k) {
        read512(sector);
        k = (u16)(left > 512 ? 512 : left);
        mg_put(to, sector, k);
        to += k;
      }
      close(fd);
      to = STORE + ((u32)(n - 1) << 14);
    }
    mg_get(sector, to + 2, 2);
    if (sector[0] != (u8)(STAMP & 0xFF) || sector[1] != (u8)(STAMP >> 8))
      return n;
  }
  ov_in[0] = ov_in[1] = 0;
  return 0;
}

static void need(u8 n)
{
  u8 place = (u8)(n > 2);
  if (ov_in[place] == n)
    return;
  mg_get(ov_at(n), STORE + ((u32)(n - 1) << 14), ov_size(n));
  ov_in[place] = n;
}

/* ------------------------------------------------------------------------------------------
   What both screens use
   ------------------------------------------------------------------------------------------ */
static u8 nfiles, sel, top;
static u8 row_new;      /* the song has come to a new row */

static void get_entry(u8 n)
{
  mg_get(ent, FILES + (u32)n * ENTRY, ENTRY);
}

static void progress(u8 i)
{
  l_new();
  lx = 1;
  l_text("Sample ");
  l_num(i, 2, 0x20);
  l_text(" of ");
  l_num(nsamples, 2, 0x20);
  l_out(ROW_MSG);
}

static void row_mark(void)
{
  row_new = 1;
}

/* ------------------------------------------------------------------------------------------
   Overlay 1: the modules on the SD card, and one of them into memory
   ------------------------------------------------------------------------------------------ */
#pragma code-name (push, "OVERLAY1")
#pragma rodata-name (push, "OVERLAY1")

static u8 lower(u8 c)
{
  return (u8)((c >= 0x41 && c <= 0x5A) ? c + 0x20 : c);
}

/* name.mod, name.s3m, name.xm or mod.name: which kind, 0 for none */
static u8 kind_of(const char *n)
{
  u8 l = (u8)strlen(n);
  u8 a, b, c;
  if (l < 4)
    return 0;
  a = lower((u8)n[l - 3]);
  b = lower((u8)n[l - 2]);
  c = lower((u8)n[l - 1]);
  if (a == 0x2E && b == 0x78 && c == 0x6D)
    return F_XM + 1;
  if (l > 4 && n[l - 4] == 0x2E) {
    if (a == 0x6D && b == 0x6F && c == 0x64)
      return F_MOD + 1;
    if (a == 0x73 && b == 0x33 && c == 0x6D)
      return F_S3M + 1;
    if (a == 0x2E)
      return 0;
  }
  if (l > 4 && lower((u8)n[0]) == 0x6D && lower((u8)n[1]) == 0x6F && lower((u8)n[2]) == 0x64 && n[3] == 0x2E)
    return F_MOD + 1;
  return 0;
}

static void find_files(void)
{
  unsigned char d;
  struct m65_dirent *e;
  u8 attr, l, kind;

  nfiles = 0;
  closeall();
  d = opendir();
  /* The name is taken from mega65-libc's dirent; the length and the attributes from the
     Hypervisor's own, which readdir leaves at $0400: the library reads those two a byte too
     early (the Hypervisor's short name is 13 bytes, not 12). */
  while (nfiles < MAXFILES && (e = readdir(d)) != 0) {
    attr = PEEK(0x0400U + 86);
    l = (u8)strlen(e->d_name);
    if ((attr & 0x08) || l == 0 || l > 63)
      continue;
    kind = 0;
    if (attr & 0x10) {
      if (l == 1 && e->d_name[0] == 0x2E) /* "." */
        continue;
    }
    else {
      kind = kind_of(e->d_name);
      if (!kind)
        continue;
    }
    memset(ent, 0, ENTRY);
    strcpy((char *)ent, e->d_name);
    ent[E_ATTR] = attr;
    ent[E_TYPE] = kind;
    memcpy(ent + E_SIZE, (u8 *)(0x0400U + 82), 4);
    mg_put(FILES + (u32)nfiles * ENTRY, ent, ENTRY);
    nfiles++;
  }
  closedir(d);
  sel = top = 0;
}

static void show_files(void)
{
  u8 i, n;
  u32 size;

  for (i = 0; i < ROWS; i++) {
    n = (u8)(top + i);
    l_new();
    if (n < nfiles) {
      get_entry(n);
      lx = 2;
      l_file(ent, 52);
      lx = 57;
      switch (ent[E_TYPE]) {
      case 0:
        l_text("folder");
        break;
      case F_MOD + 1:
        l_text("MOD");
        break;
      case F_S3M + 1:
        l_text("S3M");
        break;
      default:
        l_text("XM");
        break;
      }
      if (ent[E_TYPE]) {
        size = (get32(ent + E_SIZE) + 1023UL) >> 10;
        lx = 66;
        l_num((u16)size, 5, 0x20);
        l_text(" K");
      }
      if (n == sel)
        l_reverse(1, 78);
    }
    else if (nfiles == 0 && i == 1) {
      lx = 2;
      l_text("No .mod, .s3m or .xm here.");
    }
    l_out((u8)(ROWS_TOP + i));
  }
}

static void files_screen(void)
{
  mg_fill(SCREEN + 80, 0x20, 1920);
  mg_fill(COLOURS + 80, LGREY, 1920);
  title("the modules on the SD card");
  mg_fill(SCREEN + 3 * 80, 0x40, 80);
  colour(3, 0, 80, DGREY);
  keys_row("Cursor keys: choose    Return: play, or open the folder    Esc: leave");
  show_files();
}

/* The file into the attic RAM, and its samples from there into the sample RAM. 0, or why not. */
static const char *load(void)
{
  unsigned char fd;
  u16 got;
  u32 at = 0;

  filesize = get32(ent + E_SIZE);
  if (filesize > SIDE)
    return "It is bigger than the player has room for.";
  closeall();
  fd = open((char *)ent);
  if (fd == 0xFF)
    return "It can't be opened.";
  /* A sector at a time, as many as the directory says the file has: read512's count is
     of no use (this mega65-libc loses its low byte). */
  while (at < filesize) {
    read512(sector);
    got = (u16)(filesize - at > 512UL ? 512UL : filesize - at);
    mg_put(ATTIC + at, sector, got);
    at += got;
    if ((at & 0x7FFFUL) == 0) {
      l_new();
      lx = 1;
      l_text("Reading ");
      l_num((u16)(at >> 10), 5, 0x20);
      l_text(" K of ");
      l_num((u16)((filesize + 1023UL) >> 10), 5, 0x20);
      l_out(ROW_MSG);
    }
  }
  close(fd);
  return open_module();
}

/* The list, until a module is chosen and ready to play (1) or the user has had enough (0).
   The chosen file's entry stays in ent[]. */
static u8 browse(void)
{
  u8 k;
  const char *why;

  files_screen();
  for (;;) {
    k = wait_key();
    if (k == K_ESC || k == K_STOP)
      return 0;
    if (nfiles) {
      if (k == K_DOWN && sel + 1 < nfiles)
        sel++;
      else if (k == K_UP && sel)
        sel--;
      else if (k == K_RIGHT)
        sel = (u8)(sel + ROWS < nfiles ? sel + ROWS : nfiles - 1);
      else if (k == K_LEFT)
        sel = (u8)(sel > ROWS ? sel - ROWS : 0);
      else if (k == K_RETURN) {
        get_entry(sel);
        if (ent[E_TYPE] == 0) { /* into the folder */
          chdir((char *)ent);
          find_files();
          say(ROW_MSG, "", LGREY);
        }
        else {
          colour(ROW_MSG, 0, 80, YELLOW);
          why = load();
          if (!why)
            return 1;
          message(why);
          get_entry(sel);
        }
      }
    }
    if (sel < top)
      top = sel;
    if (sel >= top + ROWS)
      top = (u8)(sel - ROWS + 1);
    show_files();
  }
}

#pragma rodata-name (pop)
#pragma code-name (pop)

/* ------------------------------------------------------------------------------------------
   Overlay 2: the screens while a module plays
   ------------------------------------------------------------------------------------------ */
#pragma code-name (push, "OVERLAY2")
#pragma rodata-name (push, "OVERLAY2")

#define V_CHANNELS 0
#define V_SAMPLES 1
#define V_PATTERN 2
#define V_HELP 3

static u8 view;
static u8 first_ch;  /* the channel at the top of the channels, at the left of the pattern */
static u8 first_smp;
static u8 next_ch;   /* the channel whose row is drawn next */
static u8 paused;
static u8 page_row = 0xFF, page_order, mark_row; /* the pattern rows on the screen, and the one marked */
static u16 vu_clock;
static u32 played;   /* frames since the song began */

static const char notes[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
static const u8 tab_at[4] = { 0, 14, 27, 40 };
static const u8 tab_wide[4] = { 13, 13, 13, 9 };

/* a note as three characters: 0 none, 1 C-0 */
static void l_note(u8 n)
{
  if (n == 0)
    l_text("...");
  else if (n == 254)
    l_text("^^^");
  else if (n == 253)
    l_text("===");
  else {
    n--;
    line[lx++] = (u8)notes[(n % 12) << 1];
    line[lx++] = (u8)notes[((n % 12) << 1) + 1];
    line[lx++] = (u8)(0x30 + n / 12);
  }
}

/* How many things the samples screen lists, and one's name (n characters) into sector[] */
static u8 names_count(void)
{
  return (u8)(fmt == F_XM ? xm_nins : nsamples);
}

static void sample_name(u8 i, u8 n)
{
  u32 at = 0;
  memset(sector, 0, 32);
  if (fmt == F_S3M) {
    mg_get(regs, ATTIC + 0x60UL + get16(hdr + 0x20) + (u32)(i - 1) * 2, 2);
    at = (u32)get16(regs) << 4;
    if (at)
      at += 0x30UL;
  }
  else if (fmt == F_XM) {
    mg_get(regs, XINS + ((u32)i << 3), 8);
    at = get32(regs);
    if (at)
      at += 4;
  }
  else
    at = 20UL + (u32)(i - 1) * 30;
  if (at && at + n <= filesize)
    mg_get(sector, ATTIC + at, n);
}

/* the number a channel's instrument has on the samples screen */
static u8 channel_ins(u8 ch)
{
  return (u8)(fmt == F_XM ? x_ins[ch] : c_smp[ch]);
}

#define VOL_AT 54
#define VOL_WIDE 12
#define VOL_OWN 8

static void show_tabs(void)
{
  u8 i, n;
  u16 w;

  l_new();
  lx = 1;
  l_text("F1 Channels   F3 Samples   F5 Pattern   F7 Help");
  /* the volume against the song's own (the engine's MASTER as the song began with it): a
     bar in half columns, of which VOL_OWN columns are the song's own and green, and the
     rest, to half as much again, yellow: there it may be too much. And in percent. */
  lx = 50;
  l_text("Vol ");
  w = ((u16)master * (2 * VOL_OWN) + (master_own >> 1)) / master_own;
  n = (u8)(w > 2 * VOL_WIDE ? 2 * VOL_WIDE : w);
  for (i = 0; i < VOL_WIDE; i++)
    line[lx++] = (u8)(2 * i + 1 < n ? 0xA0 : 2 * i < n ? 0x61 : 0x2D);
  lx++;
  w = ((u16)master * 100 + (master_own >> 1)) / master_own;
  l_num((u16)(w > 999 ? 999 : w), 3, 0x20);
  line[lx++] = 0x25;
  if (paused) {
    lx = 72;
    l_text("paused");
  }
  l_reverse(tab_at[view], tab_wide[view]);
  l_out(3);
  colour(3, VOL_AT, VOL_OWN, GREEN);
  colour(3, VOL_AT + VOL_OWN, VOL_WIDE - VOL_OWN, YELLOW);
}

static void show_position(void)
{
  u16 s = (u16)(played / 48000UL);
  l_new();
  lx = 1;
  l_text("Position ");
  l_num((u16)(order + 1), 3, 0x20);
  l_text(" of ");
  l_num(songlen, 3, 0x20);
  l_text("   Pattern ");
  l_num(orders[order], 3, 0x20);
  l_text("   Row ");
  l_num(row, 3, 0x20);
  l_text("   Speed ");
  l_num(speed, 2, 0x20);
  l_text("   Tempo ");
  l_num(bpm, 3, 0x20);
  l_text("   ");
  l_num((u16)(s / 60), 2, 0x20);
  line[lx++] = 0x3A;
  l_num((u16)(s % 60), 2, 0x30);
  l_out(2);
}

/* A channel's row: its note and instrument, how loud, where between the sides */
static void show_channel(u8 ch, u8 y)
{
  u8 n = 0, i, v, p;

  l_new();
  if (ch < channels) {
    lx = 1;
    l_num((u16)(ch + 1), 2, 0x20);
    lx = 4;
    if (c_per[ch]) {
      if (fmt == F_XM)
        n = c_note[ch];
      else if (fmt == F_S3M)
        n = (u8)(c_note[ch] + 1);
      else
        n = (u8)(note_of((u16)(c_per[ch] >> 2)) + 1);
    }
    l_note(n);
    i = channel_ins(ch);
    lx = 8;
    if (i) {
      l_num(i, 2, 0x30);
      lx = 11;
      sample_name(i, 22);
      l_file(sector, 22);
    }
    v = vu[ch]; /* 0..64: a column for every two */
    lx = 35;
    for (i = (u8)(v >> 1); i; i--)
      line[lx++] = 0xA0;
    if (v & 1)
      line[lx++] = 0x61;
    p = c_pan[ch];
    for (i = 69; i < 78; i++) /* a line from left to right, and a block where the voice is */
      line[i] = 0x2D;
    line[69 + (u8)((p * 9U) >> 8)] = 0xA0;
  }
  l_out(y);
}

/* The bars fall back, and a voice that is quieter than its bar pulls it down */
static void bars_fall(void)
{
  u8 ch, lv;
  u16 now = clock_now();
  if ((u16)(now - vu_clock) < 1400)
    return;
  vu_clock = now;
  for (ch = 0; ch < channels; ch++) {
    lv = (u8)(o_vol[ch] >> 10);
    if (fmt == F_XM)
      lv = (u8)(lv + (lv >> 2)); /* an XM's level stops at 0.71 of the others' */
    if (o_vol[ch] == 0xFFFFU)
      lv = 0;
    if (vu[ch] > lv)
      vu[ch] = lv;
    vu[ch] = (u8)(vu[ch] > 3 ? vu[ch] - 3 : 0);
  }
}

static void show_sample(u8 i, u8 y)
{
  u8 ch, s = i;
  u32 n;

  l_new();
  if (i && i <= names_count()) {
    for (ch = 0; ch < channels; ch++)
      if (channel_ins(ch) == i && vu[ch])
        line[0] = 0x3E; /* it is sounding */
    lx = 1;
    l_num(i, 2, 0x30);
    lx = 4;
    sample_name(i, 28);
    l_file(sector, 28);
    if (fmt == F_XM) { /* an instrument: its first sample */
      mg_get(regs, XINS + ((u32)i << 3), 8);
      s = (u8)(regs[5] ? regs[4] : 0);
    }
    if (s && s_end[s] > s_pos[s]) {
      n = s_end[s] - s_pos[s];
      lx = 34;
      l_num((u16)((n + 1023UL) >> 10), 4, 0x20);
      l_text(" K");
      lx = 42;
      l_text(s_ctrl[s] & WIDE ? "16-bit" : "8-bit");
      lx = 50;
      if (s_lend[s])
        l_text(s_ctrl[s] & PING ? "back and forth" : "loop");
      lx = 66;
      if (s_narrow[s])
        l_text("as 8-bit ");
      if (s_shift[s] == 1)
        l_text("half");
      else if (s_shift[s] == 2)
        l_text("quarter");
    }
  }
  l_out(y);
}

static void cells(u8 r)
{
  if (fmt == F_XM)
    xm_cells(r);
  else
    ms_cells(r);
}

/* A pattern row: five channels of it from first_ch */
static void show_pattern_row(u8 r, u8 y)
{
  u8 k, ch;

  l_new();
  if ((u16)r < pat_rows) {
    cells(r);
    lx = 1;
    l_num(r, 3, 0x30);
    for (k = 0; k < 5; k++) {
      ch = (u8)(first_ch + k);
      if (ch >= channels)
        break;
      lx = (u8)(6 + k * 15);
      l_note(d_note[ch]);
      lx++;
      if (d_ins[ch])
        l_num(d_ins[ch], 2, 0x30);
      else
        l_text("..");
      lx++;
      if (d_vol[ch] == 0xFF)
        l_text("..");
      else if (fmt == F_S3M && d_vol[ch] < 100)
        l_num(d_vol[ch], 2, 0x30);
      else
        l_hex(d_vol[ch]);
      lx++;
      if (d_fx[ch]) {
        line[lx++] = d_fx[ch];
        l_hex(d_arg[ch]);
      }
      else
        l_text("...");
    }
    if (r == row)
      l_reverse(0, 80);
  }
  l_out(y);
}

static void show_pattern(void)
{
  u8 i, k;
  page_row = (u8)(row & 0xF0);
  page_order = order;
  mark_row = row;
  l_new();
  for (k = 0; k < 5 && first_ch + k < channels; k++) {
    lx = (u8)(6 + k * 15);
    l_text("Channel ");
    l_num((u16)(first_ch + k + 1), 2, 0x20);
  }
  l_out(ROWS_TOP);
  colour(ROWS_TOP, 0, 80, CYAN);
  for (i = 0; i < 16; i++)
    show_pattern_row((u8)(page_row + i), (u8)(ROWS_TOP + 2 + i));
  colour((u8)(ROWS_TOP + 2), 0, 5, GREY);
}

static const char *const help[] = {
  "MegaGus Player: MOD, S3M and XM modules on the MEGA65's MegaGus,",
  "32 voices at 48 kHz.",
  "",
  "F1  the channels: note, instrument, level, and where between the sides",
  "F3  the samples or instruments; > marks the ones that sound",
  "F5  the pattern as it plays",
  "",
  "Space         pause, and on",
  "+ and -       the next position of the song, the one before",
  ", and .       quieter, louder: the Vol bar; yellow is above the song's own",
  "Cursor keys   more channels, more samples",
  "V             a MOD's timing: tempo commands, or all speeds (old modules)",
  "Esc           back to the list",
  0
};

static void show_view(void)
{
  u8 i;
  mg_fill(SCREEN + ROWS_TOP * 80, 0x20, ROWS * 80);
  mg_fill(COLOURS + ROWS_TOP * 80, LGREY, ROWS * 80);
  show_tabs();
  switch (view) {
  case V_CHANNELS:
    for (i = 0; i < ROWS; i++) { /* the bars: green, then yellow, red at the top */
      colour((u8)(ROWS_TOP + i), 35, 22, GREEN);
      colour((u8)(ROWS_TOP + i), 57, 6, YELLOW);
      colour((u8)(ROWS_TOP + i), 63, 4, RED);
      colour((u8)(ROWS_TOP + i), 4, 3, WHITE);
      colour((u8)(ROWS_TOP + i), 69, 9, GREY);
      show_channel((u8)(first_ch + i), (u8)(ROWS_TOP + i));
    }
    break;
  case V_SAMPLES:
    for (i = 0; i < ROWS; i++)
      show_sample((u8)(first_smp + 1 + i), (u8)(ROWS_TOP + i));
    break;
  case V_PATTERN:
    show_pattern();
    break;
  default:
    for (i = 0; help[i]; i++) {
      l_new();
      lx = 2;
      l_text(help[i]);
      l_out((u8)(ROWS_TOP + 1 + i));
    }
    break;
  }
}

static void show_timing(void)
{
  if (fmt == F_MOD)
    message(vblank ? "Timing: every F command is a speed (VBlank)." : "Timing: an F command from 32 up is a tempo.");
}

static void play_screen(void)
{
  mg_fill(SCREEN + 80, 0x20, 1920);
  mg_fill(COLOURS + 80, LGREY, 1920);
  /* the file's name in the title row */
  l_new();
  lx = 1;
  l_text("MegaGus Player");
  lx = 20;
  l_file(ent, 58);
  l_reverse(0, 80);
  l_out(0);
  /* the song's own name, and what it is */
  l_new();
  lx = 1;
  if (fmt == F_XM)
    mg_get(sector, ATTIC + 17, 20);
  else
    memcpy(sector, hdr, 28);
  sector[fmt == F_S3M ? 28 : 20] = 0;
  l_file(sector, 28);
  lx = 32;
  l_text(fmt == F_XM ? "FastTracker 2" : fmt == F_S3M ? "Scream Tracker 3" : "MOD");
  lx = 50;
  l_num(channels, 2, 0x20);
  l_text(" channels");
  if (nreduced) {
    lx = 64;
    l_num(nreduced, 2, 0x20);
    l_text(" made smaller");
  }
  l_out(1);
  colour(1, 0, 31, WHITE);
  colour(2, 0, 80, CYAN);
  keys_row("Space: pause   + -: position   , .: volume   Cursor keys   Esc: stop");
  show_view();
}

static void skip_to(u8 o)
{
  silence();
  order = settle(o);
  row = 0;
  tickno = 0;
  patdelay = in_delay = 0;
  new_pattern();
}

#ifdef TICKTIME
/* For an emulator (cl65 -DNOGUS -DTICKTIME): how long the ticks took, by CIA 1's timer A,
   which counts down a million times a second from 16421 or so: the longest, and all of them
   together with how many, to be read out of the emulator's memory afterwards */
u16 tick_longest;
u32 tick_sum;
u16 tick_count;
u16 tick_top;   /* the highest the timer was seen at: where it begins again */
u32 tick_lines; /* and the same time in raster lines, 64 microseconds each, to check the timer by */

static u16 cia_now(void)
{
  u8 hi = PEEK(0xDC05U), lo = PEEK(0xDC04U);
  u16 v;
  if (PEEK(0xDC05U) != hi) { /* it went on between the two */
    hi = PEEK(0xDC05U);
    lo = PEEK(0xDC04U);
  }
  v = (u16)(((u16)hi << 8) | lo);
  if (v > tick_top)
    tick_top = v;
  return v;
}

static u16 line_now(void)
{
  u8 hi = PEEK(0xD011U), lo = PEEK(0xD012U);
  if ((PEEK(0xD011U) ^ hi) & 0x80) {
    hi = PEEK(0xD011U);
    lo = PEEK(0xD012U);
  }
  return (u16)(((u16)(hi & 0x80) << 1) | lo);
}
#endif

static void play_song(void)
{
  u8 k, i, old;
  u16 before;
#ifdef TICKTIME
  u16 t0, t1, l0, l1;
#endif

  vblank = 0;
  view = V_CHANNELS;
  first_ch = first_smp = next_ch = paused = 0;
  played = 0;
  memset(vu, 0, sizeof vu);
  song_start();
  play_screen();
  vu_clock = clock_now();
  for (;;) {
    before = clock_last;
#ifdef TICKTIME
    t0 = cia_now();
    l0 = line_now();
#endif
    if (song_poll()) {
      played += (u16)(clock_last - before);
#ifdef TICKTIME
      t1 = cia_now();
      l1 = line_now();
      if (l1 < l0)
        l1 += 312;
      tick_lines += (u16)(l1 - l0);
      if (t1 > t0) /* the timer began again */
        t0 += tick_top;
      t0 -= t1;
      if (t0 > tick_longest)
        tick_longest = t0;
      tick_sum += t0;
      tick_count++;
#endif
    }
    if (row_new) {
      row_new = 0;
      show_position();
      if (view == V_PATTERN) {
        if (order != page_order || (row & 0xF0) != page_row)
          show_pattern();
        else if (row != mark_row) { /* the mark moves */
          old = mark_row;
          mark_row = row;
          show_pattern_row(old, (u8)(ROWS_TOP + 2 + (old & 15)));
          show_pattern_row(row, (u8)(ROWS_TOP + 2 + (row & 15)));
        }
      }
    }
    else if (view == V_CHANNELS) { /* a channel's row at a time, so that no tick waits long */
      bars_fall();
      if (next_ch >= ROWS)
        next_ch = 0;
      show_channel((u8)(first_ch + next_ch), (u8)(ROWS_TOP + next_ch));
      next_ch++;
    }
    else if (view == V_SAMPLES) {
      bars_fall();
      if (next_ch >= ROWS)
        next_ch = 0;
      show_sample((u8)(first_smp + 1 + next_ch), (u8)(ROWS_TOP + next_ch));
      next_ch++;
    }
    else
      bars_fall();

    k = key();
    if (!k)
      continue;
    if (k == K_ESC || k == K_STOP || k == 0x71)
      break;
    old = view;
    i = first_ch;
    switch (k) {
    case 0x20:
      paused = (u8)!paused;
      wr(G_CONTROL, (u8)(paused ? 0 : 1)); /* the clock stops with the engine */
      show_tabs();
      break;
    case K_F1:
      view = V_CHANNELS;
      break;
    case K_F3:
      view = V_SAMPLES;
      break;
    case K_F5:
      view = V_PATTERN;
      break;
    case K_F7:
      view = V_HELP;
      break;
    case 0x2B: /* + */
      skip_to((u8)(order + 1));
      break;
    case 0x2D: /* - */
      skip_to((u8)(order ? order - 1 : 0));
      break;
    case 0x2C: /* , */
      master = (u8)(master > 2 ? master - 2 : 1);
      wr(G_MASTER, master);
      show_tabs();
      break;
    case 0x2E: /* . */
      master = (u8)(master < 126 ? master + 2 : 127);
      wr(G_MASTER, master);
      show_tabs();
      break;
    case 0x76: /* v */
      if (fmt == F_MOD) {
        vblank = (u8)!vblank;
        use_timing();
        show_timing();
      }
      break;
    case K_DOWN:
      if (view == V_SAMPLES) {
        if (first_smp + ROWS < names_count())
          first_smp++;
      }
      else if (first_ch + (view == V_PATTERN ? 5 : ROWS) < channels)
        first_ch++;
      break;
    case K_UP:
      if (view == V_SAMPLES) {
        if (first_smp)
          first_smp--;
      }
      else if (first_ch)
        first_ch--;
      break;
    case K_RIGHT:
      if (view == V_PATTERN && first_ch + 5 < channels)
        first_ch++;
      break;
    case K_LEFT:
      if (view == V_PATTERN && first_ch)
        first_ch--;
      break;
    default:
      break;
    }
    if (view != old || first_ch != i || (view == V_SAMPLES && (k == K_UP || k == K_DOWN)))
      show_view();
  }
  wr(G_CONTROL, 0x80);
}

#pragma rodata-name (pop)
#pragma code-name (pop)

/* ------------------------------------------------------------------------------------------
   The program
   ------------------------------------------------------------------------------------------ */
int main(void)
{
  u8 n;

  u8 missing;

  POKE(0xD02FU, 0x47); /* MEGA65 registers on */
  POKE(0xD02FU, 0x53);
  /* the overlays first, while the machine is as the KERNAL and the drive know it */
  missing = fetch_overlays();
  POKE(0x0000U, 65); /* 40 MHz */
  screen_on();
  title("");

  n = 0;
  if (rd(G_ID) != 0x4D || rd(G_ID + 1) != 0x47)
    message("This core has no MegaGus.");
  else {
    mg_poke(ATTIC, 0x5A); /* the file goes to the attic RAM: is there any? */
    mg_poke(ATTIC + 1, 0xA5);
    if (mg_peek(ATTIC) != 0x5A || mg_peek(ATTIC + 1) != 0xA5)
      message("This machine has no attic RAM.");
    else if (missing)
      message("MODPLAY.OV1 to MODPLAY.OV4 belong next to the program, from the same build.");
    else
      n = 1;
  }
  if (!n) {
    keys_row("A key: leave");
    wait_key();
    screen_off();
    return 1;
  }
  ramsize = (u32)rd(G_RAMSIZE) << 16;
  find_clock(0);
  hw_mul = 1;
  if (mul(1234567UL, 3210) != 3962960070UL)
    hw_mul = 0;
  need(OV_FILES);
  find_files();
  while (PEEK(0xD610U)) /* what was typed to load and run this is not meant for the list */
    POKE(0xD610U, 0);
  for (;;) {
    need(OV_FILES);
    if (!browse())
      break;
    need(OV_SCREENS);
    play_song();
  }
  wr(G_CONTROL, 0x80);
  screen_off();
  while (PEEK(0x0091U) == 0x7F) /* RUN/STOP still down would be BASIC's BREAK */
    ;
#if defined(NOGUS) && defined(KEYS) && defined(RERUN)
  /* for an emulator: RUN typed for BASIC, once, to see the program start a second time */
  POKE(0x02A7U, 0x55);
  POKE(0x0277U, 0x52);
  POKE(0x0278U, 0x55);
  POKE(0x0279U, 0x4E);
  POKE(0x027AU, 0x0D);
  POKE(0x00C6U, 4);
#endif
  return 0;
}

#else /* the host: play the file named on the command line once through */

static u16 rows_marked, switch_at; /* -vN: change the timing over before row N (from 1) */
static u8 want_voice;             /* -k: keep time with the last voice, as on an engine of version 1 */

static void progress(u8 i)
{
  (void)i;
}

/* a mark for the harness: position, pattern, row, and the tempo as the row begins */
static void row_mark(void)
{
  if (++rows_marked == switch_at) { /* as the V key does on the machine */
    vblank = (u8)!vblank;
    use_timing();
  }
  printf("M %u %u %u %u %u\n", (unsigned)order, (unsigned)orders[order], (unsigned)row, (unsigned)speed,
         (unsigned)bpm);
}

int main(int argc, char **argv)
{
  FILE *f;
  u16 got;
  u32 at = 0;
  const char *why;

  while (argc > 2 && argv[1][0] == '-' && (argv[1][1] == 'v' || argv[1][1] == 'k')) {
    if (argv[1][1] == 'k') /* -k: the clock voice */
      want_voice = 1;
    else if (argv[1][2]) /* -vN */
      switch_at = (u16)atoi(argv[1] + 2);
    else /* -v: VBlank timing */
      vblank = 1;
    argv++;
    argc--;
  }
  if (argc < 2) {
    fprintf(stderr, "usage: modplay [-v] [-vN] [-k] file\n");
    return 2;
  }
  f = fopen(argv[1], "rb");
  if (!f) {
    fprintf(stderr, "can't open %s\n", argv[1]);
    return 2;
  }
  ramsize = (u32)rd(G_RAMSIZE) << 16;
  find_clock(want_voice);
  while ((got = (u16)fread(sector, 1, 256, f)) != 0) {
    mg_put(ATTIC + at, sector, got);
    at += got;
  }
  fclose(f);
  filesize = at;
  why = open_module();
  if (why) {
    printf("P %s\n", why);
    return 1;
  }
  printf("P %u channels, %u patterns, %u positions, %u samples made smaller\n", (unsigned)channels,
         (unsigned)npatterns, (unsigned)songlen, (unsigned)nreduced);
  song_start();
  while (!song_ended)
    if (!song_poll())
      idle();
  silence();
  return 0;
}

#endif
