/*
  Module player for MegaGus on the MEGA65 (docs/megagus.md).

  Plays ProTracker-style modules: 31 samples, 4 channels ("M.K.", "M!K!", "FLT4", "4CHN") or
  more ("6CHN", "8CHN", "OCTA", "CD81", "xxCH" up to 32), and Scream Tracker 3 modules (S3M)
  of up to 32 channels.

  The file goes into the MEGA65's attic RAM as it is; the patterns are read from there a row
  at a time, the samples are copied from there into the MegaGus sample RAM. When they are more
  than that holds (512 KB) the biggest are made smaller on the way: a 16-bit sample is kept
  as 8-bit, then samples are kept at half their rate, then at a quarter. One voice a channel;
  the song's ticks are counted off the engine's frame counter, 48 000 a second. An engine of
  version 1 has none: there voice 31 plays nothing and is the clock (its position moves one
  byte an output frame), and a module can have 31 channels, not 32.

  Built with cc65 it is a C64-mode program (load to $0801, RUN) that lists the modules in
  the SD card's current directory. Built with a host compiler, or with cc65 for its
  simulator (-t sim6502), it plays the file named on its command line once through, speaking
  megagustest's line protocol on stdin/stdout: modplay_host.py answers that from the
  engine's model and writes what it plays to a WAV file.

  Not done in a MOD: the E0 (filter), E3 (glissando) and EF (invert loop) commands, the swap
  of a sample without a new note, 15-sample modules. In an S3M: AdLib instruments, stereo
  samples (the left half plays), S1x (glissando), SAx, SFx.

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
typedef unsigned long u32; /* at least 32 */

/* MegaGus */
#define MG_RAM 0xE000000UL
#define MG_REG 0xEFFF000UL
#define ATTIC 0x8000000UL /* the MEGA65's 8 MB of slow RAM: the module's file goes there */

#define CTRL 0
#define PAN 1
#define VOL 2
#define FREQ 4
#define POS 7
#define START 10
#define END 13

#define RUN 0x01
#define LOOP 0x02
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
#define F_MOD 0
#define F_S3M 1
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

/* ------------------------------------------------------------------------------------------
   The samples into the sample RAM
   ------------------------------------------------------------------------------------------ */
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
  u16 n, k, o;
  u8 g;
  u32 sum;
  const u8 *p;
  u8 *q;

  while (left) {
    n = (u16)(left > 512UL ? 512UL : left);
    mg_get(sector, from, n);
    from += n;
    left -= n;
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

/* The file is in the attic RAM, filesize bytes of it: get it ready to play. 0, or why not. */
static const char *open_module(void)
{
  const char *why;

  if (filesize < 0x60UL)
    return "too short for a module";
  mg_get(hdr, ATTIC, (u16)(filesize < 1084UL ? filesize : 1084UL));
  if (sig_is(hdr + 0x2C, 0x53, 0x43, 0x52, 0x4D)) { /* SCRM */
    fmt = F_S3M;
    protracker = 0;
    why = parse_s3m();
  }
  else {
    fmt = F_MOD;
    if (filesize < 1084UL)
      return "too short for a module";
    why = parse_mod();
  }
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
static u8 c_mem[7][MAXCH]; /* the arguments commands remember for when theirs is 0 */
static u8 c_fine[MAXCH];   /* the vibrato is a fine one (U) */
static u8 c_rtg[MAXCH];    /* ticks until the next retrigger */
static u8 c_tremor[MAXCH]; /* where a tremor is in its on and off time */
static u8 c_troff[MAXCH];  /* and that it is in its off time */
static u8 c_cut[MAXCH];    /* cut off: silent until the next note */
static u8 n_note[MAXCH], n_ins[MAXCH], n_vol[MAXCH]; /* the row as it was read */

static u8 order, row, tickno, speed, bpm;
static u8 vblank; /* the module is from before tempos: every F command is a speed */
/* The song's F commands, read both ways at once, so that the timing can be changed in the
   middle of a song and is then what it would have been from the start. */
static u8 speed_cia, bpm_cia, speed_vbl;
static u8 patdelay, in_delay;
static u8 begun; /* the song's first tick has been played */
static u8 song_ended; /* the song came to its end, or jumped back: it has been played through */
static u8 rowbuf[208]; /* a row: 4 bytes a channel in a MOD, up to 6 and one more in an S3M */
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

/* What the channel sounds like this tick: its period with arpeggio and vibrato, its volume
   with tremolo */
static u16 period_now(u8 ch)
{
  u16 p = c_per[ch];
  long q;
  u8 k;
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
  if (c_cut[ch] || c_troff[ch])
    return 0;
  x = (u16)volume_now(ch) * gvol;
  return x >= 4096U ? 65472U : (u16)((x << 4) - (x >> 6));
}

/* The voice's FREQ for a period: less for a sample that is kept at a lower rate */
static u32 freq_of(u8 ch, u16 p)
{
  return (freq_k / p) >> s_shift[c_play[ch]];
}

/* Give the voice what changed since it was last told */
static void flush(u8 ch)
{
  u16 p, v;
  if (c_per[ch] == 0)
    return;
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
    if (pos >= en)
      pos = round ? st + (pos - st) % (en - st) : st;
    if (ctrl & WIDE)
      pos &= ~1UL;
  }
  else if (pos >= en) { /* an offset past the end: nothing to play */
    wr(V(ch) + CTRL, 0);
    return;
  }
  p = period_now(ch);
  o_per[ch] = p;
  vol16 = level_now(ch);
  o_vol[ch] = vol16;
  regs[0] = c_pan[ch];
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

static void slide_period(u8 ch, s16 by)
{
  s16 p = (s16)c_per[ch] + by * 4;
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
  s16 d = (s16)((wave((u8)(c_wave[ch] >> 4), pos) * (u16)(c_trm[ch] & 0x0F)) >> 6);
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
static u16 rowofs[64];
static u8 rows_known;

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
        n_note[v] = rowbuf[i];
        n_ins[v] = rowbuf[i + 1];
      }
      i += 2;
    }
    if (what & 0x40) {
      if (v != 0xFF)
        n_vol[v] = rowbuf[i];
      i++;
    }
    if (what & 0x80) {
      if (v != 0xFF) {
        c_fx[v] = rowbuf[i];
        c_arg[v] = rowbuf[i + 1];
      }
      i += 2;
    }
  }
  if (r < 63) {
    rowofs[r + 1] = (u16)(rowofs[r] + i);
    if (rows_known < r + 2)
      rows_known = (u8)(r + 2);
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
  case 6: v = (s16)((v << 1) / 3); break;
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

static void all_levels(void)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++)
    flush(ch);
}

static void new_pattern(void)
{
  u8 ch;
  for (ch = 0; ch < channels; ch++) {
    c_loopstart[ch] = 0;
    c_loopcnt[ch] = 0;
  }
  if (fmt == F_S3M)
    s3m_pattern();
}

/* The position that plays when the song comes to o: an S3M has markers in its list, one to
   pass over (254) and one that ends the song (255) */
static u8 settle(u8 o)
{
  u16 tries;
  for (tries = 0; tries < 600; tries++) {
    if (o >= songlen || (fmt == F_S3M && orders[o] == 255)) {
      o = 0;
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
    else
      do_row();
  }
  else if (fmt == F_S3M)
    s3m_effects();
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
  if (m < 2)
    m = 2;
  wr(G_MASTER, m);

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

  per_min = protracker ? 113 * 4 : 28 * 4;
  per_max = protracker ? 856 * 4 : 3424 * 4;
  row = tickno = 0;
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

/* name.mod, name.s3m or mod.name */
static u8 is_module(const char *n)
{
  u8 l = (u8)strlen(n);
  u8 a, b, c;
  if (l < 5)
    return 0;
  a = lower((u8)n[l - 3]);
  b = lower((u8)n[l - 2]);
  c = lower((u8)n[l - 1]);
  if (n[l - 4] == 0x2E && ((a == 0x6D && b == 0x6F && c == 0x64) || (a == 0x73 && b == 0x33 && c == 0x6D)))
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
    if ((PEEK(0x0400U + 86) & 0x18) || !is_module(e->d_name) || strlen(e->d_name) >= NAMELEN)
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
  cputs("megagus module player\r\n\r\n");
  if (nfiles == 0) {
    cputs("no .mod or .s3m in this directory\r\nof the sd card. q: leave");
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

static void progress(u8 i)
{
  gotoxy(0, 24);
  cprintf("sample %2u of %2u                 ", i, nsamples);
}

/* The file into the attic RAM, and its samples from there into the sample RAM. 0, or why not. */
static const char *load(u8 n)
{
  unsigned char fd;
  u16 got;
  u32 at = 0;

  filesize = sizes[n];
  if (filesize > 0x800000UL)
    return "bigger than the attic ram";
  closeall();
  fd = open(names[n]);
  if (fd == 0xFF)
    return "can't open it";
  /* A sector at a time, as many as the directory says the file has: read512's count is
     of no use (this mega65-libc loses its low byte). */
  while (at < filesize) {
    read512(sector);
    got = (u16)(filesize - at > 512UL ? 512UL : filesize - at);
    mg_put(ATTIC + at, sector, got);
    at += got;
    if ((at & 0x3FFFUL) == 0) {
      gotoxy(0, 24);
      cprintf("loading %4lu k of %lu      ", at >> 10, (filesize + 1023UL) >> 10);
    }
  }
  close(fd);
  return open_module();
}

/* A sample's name into the sector buffer, n characters at most; returns how many */
static u8 sample_name(u8 i, u8 n)
{
  u32 at;
  if (fmt == F_S3M) {
    mg_get(sector, ATTIC + 0x60UL + get16(hdr + 0x20) + (u32)(i - 1) * 2, 2);
    at = (u32)get16(sector) << 4;
    if (n > 28)
      n = 28;
    memset(sector, 0, n);
    if (at && at + 0x50UL <= filesize)
      mg_get(sector, ATTIC + at + 0x30UL, n);
  }
  else {
    if (n > 22)
      n = 22;
    memcpy(sector, hdr + 20 + (u16)(i - 1) * 30, n);
  }
  return n;
}

static void show_song(u8 n)
{
  u8 i, k;
  clrscr();
  cputs("megagus module player\r\n\r\n");
  put_text((const u8 *)names[n], 39);
  cputs("\r\n");
  put_text(hdr, (u8)(fmt == F_S3M ? 28 : 20));
  cprintf("\r\n%u channels, %u patterns, %lu k", channels, npatterns, (filesize + 1023UL) >> 10);
  if (nreduced)
    cprintf(", %u smaller", nreduced);
  /* the sample names: often the module's text */
  /* the first 17 whole, one a line as their writer saw them; the rest as far as they fit */
  for (i = 1; i < 32 && i <= nsamples; i++) {
    if (i < 18) {
      gotoxy(0, (unsigned char)(6 + i));
      k = sample_name(i, 22);
    }
    else {
      gotoxy(23, (unsigned char)(i - 11));
      k = sample_name(i, 17);
    }
    put_text(sector, k);
  }
  gotoxy(0, 24);
  cputs(fmt == F_MOD ? "space:pause +/-:skip v:timing q:stop" : "space:pause +/-:skip q:stop");
}

static void show_timing(void)
{
  if (fmt != F_MOD)
    return;
  gotoxy(0, 6);
  cputs(vblank ? "timing: vblank (every f is a speed)" : "timing: cia (f from 32 is a tempo) ");
}

static void row_mark(void)
{
  gotoxy(0, 5);
  cprintf("position %3u/%-3u pattern %3u row %2u ", order + 1, songlen, orders[order], row);
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
    else if (k == 'v' && fmt == F_MOD) {
      vblank = (u8)!vblank;
      use_timing();
      show_timing();
    }
    else if (k == '+' || k == '-') {
      silence();
      if (k == '+')
        order = settle((u8)(order + 1));
      else
        order = settle((u8)(order ? order - 1 : 0));
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
  find_clock(0);
  mg_poke(ATTIC, 0x5A); /* the file goes to the attic RAM: is there any? */
  mg_poke(ATTIC + 1, 0xA5);
  if (mg_peek(ATTIC) != 0x5A || mg_peek(ATTIC + 1) != 0xA5) {
    cputs("this machine has no attic ram\r\n");
    return 1;
  }
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
