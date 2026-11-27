--
-- MegaGus: a 32-voice wavetable sound engine for the MEGA65.
--
-- A new design in the spirit of the Gravis Ultrasound's GF1: sample RAM, 32 hardware voices
-- with their own pitch, volume, panning and looping, linear interpolation and a stereo mix,
-- none of it costing the CPU a cycle. It is not a model of the GF1 and shares no code with
-- one: the registers are laid out for the MEGA65 (every voice directly addressable, linear
-- volumes as trackers use them), and all 32 voices always play at the full output rate.
--
-- docs/megagus.md has the register map and the programming model. In short:
--
--   Voice v (0-31) at register offset v*16:
--     +0      CTRL   0 run, 1 loop, 2 ping-pong, 3 16-bit, 4 backward, 5 unsigned, 7 ended
--     +1      PAN    0 left .. 255 right
--     +2,3    VOL    0 .. 65535, linear
--     +4-6    FREQ   8.16 fixed point: samples per output frame
--     +7-9    POS    byte address in the sample RAM (writing +9 commits it)
--     +10-12  START  loop start (byte address)
--     +13-15  END    loop/sample end, exclusive (byte address)
--   Globals from offset $200: ID, version, RAM size, control, master volume, ramp rate,
--   the running and ended bitmaps, the output rate, and from $214 a count of the frames
--   played: a clock for whatever plays music on it, 48 000 to the second.
--   From $218 the queue: register writes with the frame each is to take effect at, which
--   the engine then makes itself, on that frame. A program hands it the next half second
--   of a tune whenever it has the time, and needs no interrupt to keep the tune's time.
--
-- One frame (an output sample) is 17 clocks a sounding voice, one clock an idle one, and four
-- for the master volume: 548 of the 843 that 40.5 MHz gives a 48 kHz frame. Before the
-- voices the frame's queued writes go in, two clocks each, for up to 280 clocks.
--
-- The CPU owns the voice registers, the engine owns each voice's state (position, current
-- volume, running, direction). A write to CTRL or to POS's high byte is handed over by a
-- flag the engine takes when it next processes that voice, so a write can never race the
-- engine's own update of the same voice.
--
-- The bus is a toggle handshake (req_toggle changes: a request; ack_toggle follows it: done),
-- so the requester's clock only has to be synchronous to this one, not the same.
--
-- Released under the GNU Lesser General Public License, as the MEGA65 core's VHDL is.
--

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity megagus_ram is
  generic (
    addr_bits : integer := 19
    );
  port (
    clock   : in  std_logic;
    -- port A: the CPU's
    a_addr  : in  unsigned(addr_bits-1 downto 0);
    a_we    : in  std_logic;
    a_wdata : in  unsigned(7 downto 0);
    a_rdata : out unsigned(7 downto 0) := x"00";
    -- port B: the engine's
    b_addr  : in  unsigned(addr_bits-1 downto 0);
    b_rdata : out unsigned(7 downto 0) := x"00"
    );
end entity;

architecture rtl of megagus_ram is
  type ram_t is array(0 to 2**addr_bits-1) of unsigned(7 downto 0);
  signal ram : ram_t := (others => x"00");
  signal a_q : unsigned(7 downto 0) := x"00";
  signal b_q : unsigned(7 downto 0) := x"00";
  attribute ram_style : string;
  attribute ram_style of ram : signal is "block";
begin
  -- Data two clocks after the address: the block RAM's own register, then one more behind
  -- the multiplexer that picks among the block RAMs (512 KB is 128 of them).
  process (clock)
  begin
    if rising_edge(clock) then
      if a_we = '1' then
        ram(to_integer(a_addr)) <= a_wdata;
      end if;
      a_q <= ram(to_integer(a_addr));
      b_q <= ram(to_integer(b_addr));
      a_rdata <= a_q;
      b_rdata <= b_q;
    end if;
  end process;
end architecture;


-- The queue's memory: a write's frame (24 bits), register (10) and value (8)
library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity megagus_queue_ram is
  generic (
    addr_bits : integer := 12
    );
  port (
    clock : in  std_logic;
    waddr : in  unsigned(addr_bits-1 downto 0);
    we    : in  std_logic;
    wdata : in  unsigned(41 downto 0);
    raddr : in  unsigned(addr_bits-1 downto 0);
    rdata : out unsigned(41 downto 0) := (others => '0')
    );
end entity;

architecture rtl of megagus_queue_ram is
  type ram_t is array(0 to 2**addr_bits-1) of unsigned(41 downto 0);
  signal ram : ram_t := (others => (others => '0'));
  attribute ram_style : string;
  attribute ram_style of ram : signal is "block";
begin
  -- Data one clock after the address; a write shows to a read of the clock after it
  process (clock)
  begin
    if rising_edge(clock) then
      if we = '1' then
        ram(to_integer(waddr)) <= wdata;
      end if;
      rdata <= ram(to_integer(raddr));
    end if;
  end process;
end architecture;


library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity megagus is
  generic (
    clock_frequency : integer := 40500000;
    sample_rate     : integer := 48000;
    ram_addr_bits   : integer := 19;          -- 19: 512 KB of sample RAM
    queue_addr_bits : integer := 12           -- 12: a queue of 4096 writes
    );
  port (
    clock : in std_logic;
    reset : in std_logic;                     -- active high: every voice stops

    -- The bus: registers (req_ram = 0, req_addr 9..0) and sample RAM (req_ram = 1)
    req_toggle : in  std_logic;
    req_write  : in  std_logic;
    req_ram    : in  std_logic;
    req_addr   : in  unsigned(23 downto 0);
    req_wdata  : in  unsigned(7 downto 0);
    ack_toggle : out std_logic := '0';
    ack_rdata  : out unsigned(7 downto 0) := x"00";

    frame_tick  : out std_logic := '0';       -- one clock at the start of every frame
    -- For a testbench: one clock as the frame's voices are let go (the queued writes that
    -- were due are in), and one clock for each queued write as it is made
    voices_go    : out std_logic := '0';
    queue_wrote  : out std_logic := '0';
    audio_left  : out signed(15 downto 0) := (others => '0');
    audio_right : out signed(15 downto 0) := (others => '0')
    );
end entity;

architecture rtl of megagus is

  constant NVOICES : integer := 32;
  -- 2: the frame counter; 3: a stopped voice fades the last value it put out; 4: the queue;
  -- 5: what was written to CTRL and POS reads back at once, also while the voice is in work
  constant VERSION : unsigned(7 downto 0) := x"05";
  -- How long a frame's voices wait for the queued writes that are due: two clocks a write,
  -- so 140 of them are in before the first voice is looked at
  constant QWAIT : integer := 280;

  -- Voice registers (the CPU writes, the engine and the CPU read): 16 bytes a voice
  type par_t is array(0 to NVOICES-1) of unsigned(127 downto 0);
  signal par : par_t := (others => (others => '0'));

  -- Voice state (the engine writes, the engine and the CPU read)
  type pos_t is array(0 to NVOICES-1) of unsigned(39 downto 0);    -- byte address, 16-bit fraction
  type vol_t is array(0 to NVOICES-1) of unsigned(15 downto 0);
  signal st_pos : pos_t := (others => (others => '0'));
  signal st_vol : vol_t := (others => (others => '0'));
  -- The last interpolated sample each voice put out while it ran. Once the voice has
  -- stopped, that is what fades: the CPU may then write the voice's registers and its
  -- sample for the next note (another kind of sample in CTRL, other ends) without any of
  -- it reaching the output before RUN is set again.
  type hold_t is array(0 to NVOICES-1) of signed(15 downto 0);
  signal st_hold : hold_t := (others => (others => '0'));

  attribute ram_style : string;
  attribute ram_style of par    : signal is "distributed";
  attribute ram_style of st_pos : signal is "distributed";
  attribute ram_style of st_vol : signal is "distributed";
  attribute ram_style of st_hold : signal is "distributed";

  signal running   : std_logic_vector(NVOICES-1 downto 0) := (others => '0');
  signal dirv      : std_logic_vector(NVOICES-1 downto 0) := (others => '0');
  signal live      : std_logic_vector(NVOICES-1 downto 0) := (others => '0');  -- running or fading
  signal endflag   : std_logic_vector(NVOICES-1 downto 0) := (others => '0');
  signal run_req   : std_logic_vector(NVOICES-1 downto 0) := (others => '0');  -- CTRL as written
  signal dir_req   : std_logic_vector(NVOICES-1 downto 0) := (others => '0');
  signal ctrl_load : std_logic_vector(NVOICES-1 downto 0) := (others => '0');  -- CTRL waits
  signal pos_load  : std_logic_vector(NVOICES-1 downto 0) := (others => '0');  -- POS waits

  signal g_enable : std_logic := '1';
  signal g_master : unsigned(7 downto 0) := x"20";
  signal g_ramp   : unsigned(7 downto 0) := x"40";
  signal soft_reset : std_logic := '0';
  -- The frames played since the reset, or since $214 was last written: it stands still
  -- while the engine is switched off, as the voices do
  signal g_frames : unsigned(31 downto 0) := (others => '0');
  signal frames_snap : unsigned(23 downto 0) := (others => '0');

  -- The queue: writes the CPU has handed over, each to be made when the frame count is at
  -- its time or past it. They are made in the order they came, through the bus's own way
  -- into the registers, when the CPU is not on it.
  signal q_time  : unsigned(23 downto 0) := (others => '0');   -- $218-$21A
  signal q_reg   : unsigned(9 downto 0) := (others => '0');    -- $21B, $21C
  signal q_wptr  : unsigned(queue_addr_bits-1 downto 0) := (others => '0');
  signal q_rptr  : unsigned(queue_addr_bits-1 downto 0) := (others => '0');
  signal q_count : unsigned(queue_addr_bits downto 0) := (others => '0');
  signal q_over  : std_logic := '0';                           -- a write found it full
  signal q_we    : std_logic := '0';
  signal q_waddr : unsigned(queue_addr_bits-1 downto 0) := (others => '0');
  signal q_wdata : unsigned(41 downto 0) := (others => '0');
  signal q_rdata : unsigned(41 downto 0);
  -- 0: nothing queued, or the first of it not yet read; 1: reading; 3: q_rdata is the
  -- write at the head
  signal q_st    : integer range 0 to 3 := 0;
  signal q_wait  : integer range 0 to QWAIT := 0;
  signal b_queue : std_logic := '0';                           -- the bus is making a queued write

  -- The bus
  signal req_seen  : std_logic := '0';
  signal bus_state : integer range 0 to 3 := 0;
  signal b_write   : std_logic := '0';
  signal b_ram     : std_logic := '0';
  signal b_addr    : unsigned(23 downto 0) := (others => '0');
  signal b_wdata   : unsigned(7 downto 0) := x"00";
  signal pos_snap  : unsigned(15 downto 0) := (others => '0');

  -- The sample RAM
  signal ram_a_we    : std_logic := '0';
  signal ram_a_rdata : unsigned(7 downto 0);
  signal ram_b_addr  : unsigned(ram_addr_bits-1 downto 0) := (others => '0');
  signal ram_b_rdata : unsigned(7 downto 0);

  -- The frame clock
  signal tick_acc : integer range 0 to clock_frequency + sample_rate := 0;

  -- The engine
  signal step : integer range 0 to 22 := 0;
  signal vidx : unsigned(4 downto 0) := (others => '0');

  signal w_ctrl  : unsigned(7 downto 0) := x"00";
  signal w_pan   : unsigned(7 downto 0) := x"00";
  signal w_vol   : unsigned(15 downto 0) := (others => '0');
  signal w_freq  : unsigned(23 downto 0) := (others => '0');
  signal w_start : unsigned(23 downto 0) := (others => '0');
  signal w_end   : unsigned(23 downto 0) := (others => '0');
  signal w_posi  : unsigned(23 downto 0) := (others => '0');
  signal w_frac  : unsigned(15 downto 0) := (others => '0');
  signal w_cv    : unsigned(15 downto 0) := (others => '0');
  signal w_ncv   : unsigned(15 downto 0) := (others => '0');
  signal w_run   : std_logic := '0';
  signal w_dir   : std_logic := '0';

  -- in samples (a 16-bit voice's byte addresses halved)
  signal w_p  : unsigned(23 downto 0) := (others => '0');
  signal w_i1 : unsigned(23 downto 0) := (others => '0');
  signal w_st : unsigned(23 downto 0) := (others => '0');
  signal w_en : unsigned(23 downto 0) := (others => '0');

  signal w_n       : signed(43 downto 0) := (others => '0');
  signal w_wrapped : std_logic := '0';
  signal w_run_n   : std_logic := '0';
  signal w_dir_n   : std_logic := '0';
  signal w_ended   : std_logic := '0';

  signal d0, d1, d2, d3 : unsigned(7 downto 0) := x"00";
  signal w_s0 : signed(15 downto 0) := (others => '0');
  signal w_vv : signed(15 downto 0) := (others => '0');

  signal m_a : signed(23 downto 0) := (others => '0');
  signal m_b : signed(17 downto 0) := (others => '0');
  signal m_p : signed(41 downto 0) := (others => '0');

  signal accl, accr : signed(23 downto 0) := (others => '0');
  signal w_outl : signed(15 downto 0) := (others => '0');

  function sat16(x : signed) return signed is
  begin
    if x > to_signed(32767, x'length) then
      return to_signed(32767, 16);
    elsif x < to_signed(-32768, x'length) then
      return to_signed(-32768, 16);
    else
      return resize(x, 16);
    end if;
  end function;

  -- a position (in samples, no fraction) as the 28.16 the update works in
  function pos44(x : unsigned(23 downto 0)) return signed is
  begin
    return signed(shift_left(resize(x, 44), 16));
  end function;

begin

  assert clock_frequency / sample_rate >= NVOICES * 17 + 8 + QWAIT + 2
    report "MegaGus: a frame is too short for 32 voices at this clock and sample rate"
    severity failure;

  queue0: entity work.megagus_queue_ram
    generic map (
      addr_bits => queue_addr_bits
      )
    port map (
      clock => clock,
      waddr => q_waddr,
      we    => q_we,
      wdata => q_wdata,
      raddr => q_rptr,
      rdata => q_rdata
      );

  ram0: entity work.megagus_ram
    generic map (
      addr_bits => ram_addr_bits
      )
    port map (
      clock   => clock,
      a_addr  => b_addr(ram_addr_bits-1 downto 0),
      a_we    => ram_a_we,
      a_wdata => b_wdata,
      a_rdata => ram_a_rdata,
      b_addr  => ram_b_addr,
      b_rdata => ram_b_rdata
      );

  process (clock)
    variable tick     : boolean;
    variable vi       : integer range 0 to NVOICES-1;
    variable pv       : unsigned(127 downto 0);
    variable vrun     : std_logic;
    variable vdir     : std_logic;
    variable vposi    : unsigned(23 downto 0);
    variable vfrac    : unsigned(15 downto 0);
    variable vcv      : unsigned(15 downto 0);
    variable vp       : unsigned(23 downto 0);
    variable vst      : unsigned(23 downto 0);
    variable ven      : unsigned(23 downto 0);
    variable vi1      : unsigned(24 downto 0);
    variable vtgt     : unsigned(15 downto 0);
    variable vdiff    : signed(16 downto 0);
    variable vstp     : signed(16 downto 0);
    variable p44      : signed(43 downto 0);
    variable f44      : signed(43 downto 0);
    variable st44     : signed(43 downto 0);
    variable en44     : signed(43 downto 0);
    variable n2       : signed(43 downto 0);
    variable a0       : unsigned(24 downto 0);
    variable s0       : signed(15 downto 0);
    variable s1       : signed(15 downto 0);
    variable sdiff    : signed(16 downto 0);
    variable interp   : signed(17 downto 0);
    variable vv       : signed(15 downto 0);
    variable next_v   : boolean;

    -- the one write to the state RAMs
    variable st_we    : boolean;
    variable st_wpos  : unsigned(39 downto 0);
    variable st_wvol  : unsigned(15 downto 0);

    -- the bus
    variable rv       : integer range 0 to NVOICES-1;
    variable rr       : integer range 0 to 15;
    variable cpv      : unsigned(127 downto 0);
    variable cpos     : unsigned(23 downto 0);
    variable rd       : unsigned(7 downto 0);
    variable active   : std_logic_vector(NVOICES-1 downto 0);
    variable curdir   : std_logic_vector(NVOICES-1 downto 0);
    variable ramsize  : unsigned(7 downto 0);
    variable rate16   : unsigned(15 downto 0);

    -- the queue
    variable q_ahead  : unsigned(23 downto 0);
    variable q_due    : boolean;
    variable q_free   : unsigned(queue_addr_bits downto 0);
  begin
    if rising_edge(clock) then

      -----------------------------------------------------------------------------------------
      -- The frame clock: sample_rate ticks a second, whatever the clock
      -----------------------------------------------------------------------------------------
      tick := false;
      if tick_acc + sample_rate >= clock_frequency then
        tick_acc <= tick_acc + sample_rate - clock_frequency;
        tick := true;
      else
        tick_acc <= tick_acc + sample_rate;
      end if;
      frame_tick <= '0';
      if tick then
        frame_tick <= '1';
      end if;
      voices_go <= '0';
      queue_wrote <= '0';

      -- The write at the head of the queue is due when the frame count is at its time or
      -- past it: up to 8 388 607 frames past it, which is nearly three minutes
      q_ahead := q_rdata(41 downto 18) - g_frames(23 downto 0);
      q_due := q_st = 3 and (q_ahead = 0 or q_ahead(23) = '1');

      -- The one multiplier, shared by a voice's four products and the master volume's two:
      -- operands set in step n are multiplied in n+1, the product is read in n+2.
      m_p <= m_a * m_b;

      -----------------------------------------------------------------------------------------
      -- The engine
      -----------------------------------------------------------------------------------------
      vi := to_integer(vidx);
      st_we := false;
      st_wpos := (others => '0');
      st_wvol := (others => '0');
      next_v := false;

      case step is
        when 0 =>
          -- Between frames
          if tick then
            if g_enable = '1' then
              g_frames <= g_frames + 1;
              vidx <= (others => '0');
              accl <= (others => '0');
              accr <= (others => '0');
              q_wait <= 0;
              step <= 22;
            else
              -- stopped: the voices hold where they are, the output is silent
              audio_left <= (others => '0');
              audio_right <= (others => '0');
            end if;
          end if;

        when 1 =>
          -- The voice's registers and state; what the CPU wrote to CTRL and POS since
          pv := par(vi);
          if ctrl_load(vi) = '1' then
            vrun := run_req(vi);
            vdir := dir_req(vi);
          else
            vrun := running(vi);
            vdir := dirv(vi);
          end if;
          if pos_load(vi) = '1' then
            vposi := pv(79 downto 56);
            vfrac := (others => '0');
          else
            vposi := st_pos(vi)(39 downto 16);
            vfrac := st_pos(vi)(15 downto 0);
          end if;
          if live(vi) = '1' then
            vcv := st_vol(vi);
          else
            vcv := (others => '0');
          end if;
          ctrl_load(vi) <= '0';
          pos_load(vi) <= '0';

          w_ctrl  <= pv(7 downto 0);
          w_pan   <= pv(15 downto 8);
          w_vol   <= pv(31 downto 16);
          w_freq  <= pv(55 downto 32);
          w_start <= pv(103 downto 80);
          w_end   <= pv(127 downto 104);
          w_posi  <= vposi;
          w_frac  <= vfrac;
          w_cv    <= vcv;
          w_run   <= vrun;
          w_dir   <= vdir;

          -- What the CPU wrote is the voice's state from this clock on. (It used to become
          -- that only with the voice's last step, 16 clocks later, while the flags that say
          -- "the CPU wrote" were already cleared here: a read of CTRL, POS or RUNNING in
          -- between showed the voice as it was before the write.)
          st_we := true;
          st_wpos := vposi & vfrac;
          st_wvol := vcv;
          running(vi) <= vrun;
          dirv(vi) <= vdir;
          if vrun = '0' and live(vi) = '0' then
            -- Silent: nothing to mix
            next_v := true;
          else
            step <= 2;
          end if;

        when 2 =>
          -- In samples: a 16-bit voice's addresses are bytes too, its samples on even ones
          if w_ctrl(3) = '1' then
            vp  := '0' & w_posi(23 downto 1);
            vst := '0' & w_start(23 downto 1);
            ven := '0' & w_end(23 downto 1);
          else
            vp  := w_posi;
            vst := w_start;
            ven := w_end;
          end if;
          -- The sample to interpolate toward: the next one, at the end of a forward loop its
          -- first, at any other end this one again
          vi1 := ('0' & vp) + 1;
          if vi1 >= ('0' & ven) then
            if w_ctrl(1) = '1' and w_ctrl(2) = '0' then
              vi1 := '0' & vst;
            else
              vi1 := '0' & vp;
            end if;
          end if;
          w_p  <= vp;
          w_st <= vst;
          w_en <= ven;
          w_i1 <= vi1(23 downto 0);

          -- The volume moves toward its target (0 once the voice has stopped) by the ramp
          -- rate a frame: no clicks from a jump in level
          if w_run = '1' then
            vtgt := w_vol;
          else
            vtgt := (others => '0');
          end if;
          vdiff := signed(resize(vtgt, 17)) - signed(resize(w_cv, 17));
          vstp := signed(shift_left(resize(g_ramp, 17), 4));
          if g_ramp = x"00" then
            w_ncv <= vtgt;
          elsif vdiff > vstp then
            w_ncv <= w_cv + unsigned(vstp(15 downto 0));
          elsif vdiff < -vstp then
            w_ncv <= w_cv - unsigned(vstp(15 downto 0));
          else
            w_ncv <= vtgt;
          end if;
          step <= 3;

        when 3 | 4 | 5 | 6 =>
          -- Four bytes from the sample RAM, an address a clock: this sample and the next,
          -- low byte then high (an 8-bit voice uses the first of each pair)
          if step = 3 or step = 4 then
            a0 := '0' & w_p;
          else
            a0 := '0' & w_i1;
          end if;
          if w_ctrl(3) = '1' then
            a0 := a0(23 downto 0) & '0';
            if step = 4 or step = 6 then
              a0(0) := '1';
            end if;
          end if;
          ram_b_addr <= a0(ram_addr_bits-1 downto 0);
          if step = 6 then
            d0 <= ram_b_rdata;
          end if;

          -- Meanwhile, the position moves on by FREQ
          p44  := pos44(w_p);
          p44(15 downto 0) := signed(w_frac);
          f44  := signed(resize(w_freq, 44));
          st44 := pos44(w_st);
          en44 := pos44(w_en);
          if step = 3 then
            w_wrapped <= '0';
            w_ended <= '0';
            w_run_n <= w_run;
            w_dir_n <= w_dir;
            if w_run = '0' then
              w_n <= p44;
            elsif w_dir = '0' then
              w_n <= p44 + f44;
            else
              w_n <= p44 - f44;
            end if;
          elsif step = 4 and w_run = '1' then
            -- At the end (forward) or the start (backward): loop, turn round or stop
            if w_dir = '0' then
              if w_n >= en44 then
                if w_ctrl(1) = '1' and w_ctrl(2) = '0' then
                  w_n <= w_n - (en44 - st44);
                  w_wrapped <= '1';
                elsif w_ctrl(1) = '1' then
                  w_n <= shift_left(en44, 1) - 1 - w_n;
                  w_dir_n <= '1';
                  w_wrapped <= '1';
                else
                  w_n <= en44 - 65536;
                  w_run_n <= '0';
                  w_ended <= '1';
                end if;
              end if;
            else
              if w_n < st44 then
                if w_ctrl(1) = '1' and w_ctrl(2) = '1' then
                  w_n <= shift_left(st44, 1) - w_n;
                  w_dir_n <= '0';
                  w_wrapped <= '1';
                elsif w_ctrl(1) = '1' then
                  w_n <= w_n + (en44 - st44);
                  w_wrapped <= '1';
                else
                  w_n <= st44;
                  w_run_n <= '0';
                  w_ended <= '1';
                end if;
              end if;
            end if;
          elsif step = 5 then
            -- A loop shorter than one step (or none at all, END at or below START)
            if w_wrapped = '1' and (w_n < st44 or w_n >= en44) then
              w_n <= st44;
            end if;
          end if;
          step <= step + 1;

        when 7 =>
          d1 <= ram_b_rdata;
          step <= 8;

        when 8 =>
          d2 <= ram_b_rdata;
          step <= 9;

        when 9 =>
          d3 <= ram_b_rdata;
          step <= 10;

        when 10 =>
          -- The two samples as signed 16 bits; their difference times the fraction
          if w_ctrl(3) = '1' then
            s0(15 downto 8) := signed(d1);
            s0(7 downto 0)  := signed(d0);
            s1(15 downto 8) := signed(d3);
            s1(7 downto 0)  := signed(d2);
          else
            s0(15 downto 8) := signed(d0);
            s0(7 downto 0)  := (others => '0');
            s1(15 downto 8) := signed(d2);
            s1(7 downto 0)  := (others => '0');
          end if;
          if w_ctrl(5) = '1' then               -- unsigned samples: the top bit the other way
            s0(15) := not s0(15);
            s1(15) := not s1(15);
          end if;
          sdiff := resize(s1, 17) - resize(s0, 17);
          if w_run = '0' then
            -- stopped: not the sample RAM's bytes, the value it last put out
            s0 := st_hold(vi);
            sdiff := (others => '0');
          end if;
          w_s0 <= s0;
          m_a <= resize(sdiff, 24);
          m_b <= signed(resize(w_frac, 18));
          step <= 11;

        when 11 =>
          step <= 12;

        when 12 =>
          -- The interpolated sample times the voice's volume
          interp := resize(w_s0, 18) + resize(shift_right(m_p, 16), 18);
          m_a <= resize(interp, 24);
          m_b <= signed(resize(w_cv, 18));
          if w_run = '1' then
            st_hold(vi) <= interp(15 downto 0);
          end if;
          step <= 13;

        when 13 =>
          step <= 14;

        when 14 =>
          -- Left and right: PAN's share of it each
          vv := resize(shift_right(m_p, 16), 16);
          w_vv <= vv;
          m_a <= resize(vv, 24);
          m_b <= signed(resize(x"FF" - w_pan, 18));
          step <= 15;

        when 15 =>
          m_a <= resize(w_vv, 24);
          m_b <= signed(resize(w_pan, 18));
          step <= 16;

        when 16 =>
          accl <= accl + resize(shift_right(m_p, 8), 24);
          step <= 17;

        when 17 =>
          accr <= accr + resize(shift_right(m_p, 8), 24);
          -- The voice's state for the next frame
          st_we := true;
          if w_ctrl(3) = '1' then
            st_wpos := unsigned(w_n(38 downto 16)) & '0' & unsigned(w_n(15 downto 0));
          else
            st_wpos := unsigned(w_n(39 downto 0));
          end if;
          st_wvol := w_ncv;
          running(vi) <= w_run_n;
          dirv(vi) <= w_dir_n;
          if w_ended = '1' and ctrl_load(vi) = '0' then
            endflag(vi) <= '1';
          end if;
          next_v := true;

        when 18 =>
          -- The sum times the master volume (40h: as it is)
          m_a <= accl;
          m_b <= signed(resize(g_master, 18));
          step <= 19;

        when 19 =>
          m_a <= accr;
          step <= 20;

        when 20 =>
          w_outl <= sat16(shift_right(m_p, 6));
          step <= 21;

        when 21 =>
          audio_left <= w_outl;
          audio_right <= sat16(shift_right(m_p, 6));
          step <= 0;

        when 22 =>
          -- The frame's queued writes first, so that all of a tick's writes are in before
          -- any voice is looked at: until nothing is queued or the head is not yet due.
          -- But not for longer than leaves the voices their time.
          if q_count = 0 or (q_st = 3 and not q_due) or q_wait = QWAIT then
            voices_go <= '1';
            step <= 1;
          else
            q_wait <= q_wait + 1;
          end if;
      end case;

      if st_we then
        st_pos(vi) <= st_wpos;
        st_vol(vi) <= st_wvol;
        if st_wvol /= x"0000" or (step = 17 and w_run_n = '1') or (step = 1 and vrun = '1') then
          live(vi) <= '1';
        else
          live(vi) <= '0';
        end if;
      end if;
      if next_v then
        if vi = NVOICES-1 then
          step <= 18;
        else
          vidx <= vidx + 1;
          step <= 1;
        end if;
      end if;

      -----------------------------------------------------------------------------------------
      -- The bus (after the engine: where both set the same flag in one clock, the CPU's
      -- write is the one that counts)
      -----------------------------------------------------------------------------------------
      ram_a_we <= '0';
      soft_reset <= '0';
      q_we <= '0';
      if q_st = 0 and q_count /= 0 then
        q_st <= 1;
      elsif q_st = 1 then
        q_st <= 3;
      end if;
      q_free := to_unsigned(2**queue_addr_bits, queue_addr_bits+1) - q_count;
      rv := to_integer(b_addr(8 downto 4));
      rr := to_integer(b_addr(3 downto 0));
      cpv := par(rv);
      for i in 0 to NVOICES-1 loop
        if ctrl_load(i) = '1' then
          active(i) := run_req(i);
          curdir(i) := dir_req(i);
        else
          active(i) := running(i);
          curdir(i) := dirv(i);
        end if;
      end loop;
      ramsize := to_unsigned((2**ram_addr_bits) / 65536, 8);
      rate16 := to_unsigned(sample_rate, 16);

      case bus_state is
        when 0 =>
          if req_toggle /= req_seen then
            req_seen <= req_toggle;
            b_write <= req_write;
            b_ram <= req_ram;
            b_addr <= req_addr;
            b_wdata <= req_wdata;
            b_queue <= '0';
            bus_state <= 1;
          elsif q_due then
            -- The CPU is not on the bus: the queued write that is due, as if it made it
            b_write <= '1';
            b_ram <= '0';
            b_addr <= resize(q_rdata(17 downto 8), 24);
            b_wdata <= q_rdata(7 downto 0);
            b_queue <= '1';
            bus_state <= 1;
            q_rptr <= q_rptr + 1;
            q_count <= q_count - 1;
            if q_count = 1 then
              q_st <= 0;
            else
              q_st <= 1;
            end if;
          end if;

        when 1 =>
          if b_ram = '1' then
            -- The sample RAM: what lies beyond it reads 0 and ignores writes
            if b_write = '1' and b_addr(23 downto ram_addr_bits) = 0 then
              ram_a_we <= '1';
            end if;
            bus_state <= 2;
          else
            rd := x"00";
            if b_addr(9) = '0' then
              -- A voice's registers
              rd := resize(shift_right(cpv, rr * 8), 8);
              if pos_load(rv) = '1' then
                cpos := cpv(79 downto 56);
              else
                cpos := st_pos(rv)(39 downto 16);
              end if;
              case rr is
                when 0 =>
                  rd(0) := active(rv);
                  rd(4) := curdir(rv);
                  rd(7) := endflag(rv);
                when 7 =>
                  -- the position as it is now; its other two bytes are held for the reads
                  -- that follow
                  rd := cpos(7 downto 0);
                  if b_write = '0' then
                    pos_snap <= cpos(23 downto 8);
                  end if;
                when 8 =>
                  rd := pos_snap(7 downto 0);
                when 9 =>
                  rd := pos_snap(15 downto 8);
                when others =>
                  null;
              end case;
              if b_write = '1' then
                for i in 0 to 15 loop
                  if rr = i then
                    par(rv)(i*8+7 downto i*8) <= b_wdata;
                  end if;
                end loop;
                if rr = 0 then
                  run_req(rv) <= b_wdata(0);
                  dir_req(rv) <= b_wdata(4);
                  ctrl_load(rv) <= '1';
                  endflag(rv) <= '0';
                end if;
                if rr = 9 then
                  pos_load(rv) <= '1';
                end if;
              end if;
            elsif b_addr(8 downto 5) = "0000" then
              -- The globals
              case to_integer(b_addr(4 downto 0)) is
                when 0  => rd := x"4D";                     -- 'M'
                when 1  => rd := x"47";                     -- 'G'
                when 2  => rd := VERSION;
                when 3  => rd := ramsize;
                when 4  => rd := "0000000" & g_enable;
                when 5  => rd := g_master;
                when 6  => rd := g_ramp;
                when 7  => rd := to_unsigned(NVOICES, 8);
                when 8  => rd := unsigned(active(7 downto 0));
                when 9  => rd := unsigned(active(15 downto 8));
                when 10 => rd := unsigned(active(23 downto 16));
                when 11 => rd := unsigned(active(31 downto 24));
                when 12 => rd := unsigned(endflag(7 downto 0));
                when 13 => rd := unsigned(endflag(15 downto 8));
                when 14 => rd := unsigned(endflag(23 downto 16));
                when 15 => rd := unsigned(endflag(31 downto 24));
                when 16 => rd := rate16(7 downto 0);
                when 17 => rd := rate16(15 downto 8);
                when 20 =>
                  -- the frame count as it is now; its other three bytes are held for the
                  -- reads that follow
                  rd := g_frames(7 downto 0);
                  if b_write = '0' then
                    frames_snap <= g_frames(31 downto 8);
                  end if;
                when 21 => rd := frames_snap(7 downto 0);
                when 22 => rd := frames_snap(15 downto 8);
                when 23 => rd := frames_snap(23 downto 16);
                when 24 => rd := q_time(7 downto 0);
                when 25 => rd := q_time(15 downto 8);
                when 26 => rd := q_time(23 downto 16);
                when 27 => rd := q_reg(7 downto 0);
                when 28 => rd := "000000" & q_reg(9 downto 8);
                when 30 =>
                  -- empty, full, and whether a write was lost to a full queue
                  rd := x"00";
                  if q_count = 0 then
                    rd(0) := '1';
                  end if;
                  rd(1) := q_count(queue_addr_bits);
                  rd(7) := q_over;
                when 31 =>
                  -- room, in sixteens of writes (255: for 4080 or more)
                  if q_free(queue_addr_bits downto 4) > 255 then
                    rd := x"FF";
                  else
                    rd := resize(q_free(queue_addr_bits downto 4), 8);
                  end if;
                when others => null;
              end case;
              if b_write = '1' and b_queue = '1' and b_addr(4 downto 3) = "11" then
                -- a queued write to the queue's own registers: not made
                null;
              elsif b_write = '1' then
                case to_integer(b_addr(4 downto 0)) is
                  when 4 =>
                    g_enable <= b_wdata(0);
                    soft_reset <= b_wdata(7);       -- as the reset line: all voices off
                  when 5 => g_master <= b_wdata;
                  when 6 => g_ramp <= b_wdata;
                  when 20 => g_frames <= (others => '0');   -- any value: count from here
                  when 24 => q_time(7 downto 0) <= b_wdata;
                  when 25 => q_time(15 downto 8) <= b_wdata;
                  when 26 => q_time(23 downto 16) <= b_wdata;
                  when 27 => q_reg(7 downto 0) <= b_wdata;
                  when 28 => q_reg(9 downto 8) <= b_wdata(1 downto 0);
                  when 29 =>
                    -- a write for the queue: to the register after it, the next one
                    if q_count(queue_addr_bits) = '1' then
                      q_over <= '1';
                    else
                      q_we <= '1';
                      q_waddr <= q_wptr;
                      q_wdata <= q_time & q_reg & b_wdata;
                      q_wptr <= q_wptr + 1;
                      q_count <= q_count + 1;
                    end if;
                    q_reg <= q_reg + 1;
                  when 30 =>
                    if b_wdata(7) = '1' then        -- forget all that is queued
                      q_wptr <= (others => '0');
                      q_rptr <= (others => '0');
                      q_count <= (others => '0');
                      q_over <= '0';
                      q_st <= 0;
                    end if;
                  when 12 | 13 | 14 | 15 =>
                    -- a 1 clears that voice's ended flag
                    for i in 0 to 7 loop
                      if b_wdata(i) = '1' then
                        endflag((to_integer(b_addr(1 downto 0))) * 8 + i) <= '0';
                      end if;
                    end loop;
                  when others => null;
                end case;
              end if;
            end if;
            if b_queue = '0' then
              ack_rdata <= rd;
              ack_toggle <= req_seen;
            else
              queue_wrote <= '1';
            end if;
            b_queue <= '0';
            bus_state <= 0;
          end if;

        when 2 =>
          bus_state <= 3;

        when 3 =>
          if b_addr(23 downto ram_addr_bits) = 0 then
            ack_rdata <= ram_a_rdata;
          else
            ack_rdata <= x"00";
          end if;
          ack_toggle <= req_seen;
          bus_state <= 0;
      end case;

      -----------------------------------------------------------------------------------------
      -- Reset (the line, or bit 7 written to the control register): every voice stops at
      -- once, the globals go back to their defaults. The voice registers and the sample RAM
      -- keep what they hold.
      -----------------------------------------------------------------------------------------
      if reset = '1' or soft_reset = '1' then
        step <= 0;
        running <= (others => '0');
        dirv <= (others => '0');
        live <= (others => '0');
        endflag <= (others => '0');
        run_req <= (others => '0');
        dir_req <= (others => '0');
        ctrl_load <= (others => '0');
        pos_load <= (others => '0');
        g_enable <= '1';
        g_master <= x"20";
        g_ramp <= x"40";
        g_frames <= (others => '0');
        audio_left <= (others => '0');
        audio_right <= (others => '0');
        q_time <= (others => '0');
        q_reg <= (others => '0');
        q_wptr <= (others => '0');
        q_rptr <= (others => '0');
        q_count <= (others => '0');
        q_over <= '0';
        q_we <= '0';
        q_st <= 0;
        voices_go <= '0';
      end if;

    end if;
  end process;

end architecture;
