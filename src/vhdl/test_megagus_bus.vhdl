--
-- Testbench for MegaGus on the slow device bus: slow_devices.vhdl with the engine in it, and
-- accesses made as the CPU makes them (the request toggle from the CPU clock, slow_devices
-- on the pixel clock at twice that). Checks the address decoding at $Exxxxxx, the handshake
-- across the two clocks, and that a voice set up this way sounds.
--
-- Run by src/tests/megagus/megagus_test.py after the engine's own test.
--

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use work.cputypes.all;

entity test_megagus_bus is
end entity;

architecture tb of test_megagus_bus is
  signal pixelclock : std_logic := '0';
  signal cpuclock   : std_logic := '0';
  signal done       : boolean := false;

  signal req   : std_logic := '0';
  signal ready : std_logic;
  signal wr    : std_logic := '0';
  signal addr  : unsigned(27 downto 0) := (others => '0');
  signal wdata : unsigned(7 downto 0) := x"00";
  signal rdata : unsigned(7 downto 0);

  signal megagus_left  : signed(15 downto 0);
  signal megagus_right : signed(15 downto 0);
  signal heard_left    : boolean := false;
  signal heard_right   : boolean := false;

  signal qspidb : unsigned(3 downto 0);
  signal cart_ba, cart_rw, cart_roml, cart_romh, cart_io1, cart_io2 : std_logic;
  signal cart_a : unsigned(15 downto 0);
begin

  -- 81 MHz and 40.5 MHz, edge on edge
  pixelclock <= not pixelclock after 6173 ps when not done else '0';
  process (pixelclock)
  begin
    if rising_edge(pixelclock) then
      cpuclock <= not cpuclock;
    end if;
  end process;

  dut: entity work.slow_devices
    generic map (
      target => mega65r6,
      megagus_ram_bits => 12
      )
    port map (
      pixelclock => pixelclock,
      cpuclock => cpuclock,
      reset => '1',
      sector_buffer_mapped => '0',
      slow_access_request_toggle => req,
      slow_access_ready_toggle => ready,
      slow_access_write => wr,
      slow_access_address => addr,
      slow_access_wdata => wdata,
      slow_access_rdata => rdata,
      megagus_left => megagus_left,
      megagus_right => megagus_right,
      expansionram_busy => '1',
      QspiDB => qspidb,
      cart_nmi => '1',
      cart_irq => '1',
      cart_dma => '1',
      cart_exrom => '1',
      cart_game => '1',
      cart_ba => cart_ba,
      cart_rw => cart_rw,
      cart_roml => cart_roml,
      cart_romh => cart_romh,
      cart_io1 => cart_io1,
      cart_io2 => cart_io2,
      cart_d_in => x"FF",
      cart_a => cart_a
      );

  process (cpuclock)
  begin
    if rising_edge(cpuclock) then
      if megagus_left /= 0 then
        heard_left <= true;
      end if;
      if megagus_right /= 0 then
        heard_right <= true;
      end if;
    end if;
  end process;

  process
    variable tog : std_logic := '0';
    variable got : unsigned(7 downto 0);
    variable pos : unsigned(23 downto 0);
    variable clocks : integer;
    variable worst : integer := 0;

    procedure slow (w : std_logic; a : unsigned(27 downto 0); d : unsigned(7 downto 0)) is
    begin
      wait until rising_edge(cpuclock);
      wr <= w;
      addr <= a;
      wdata <= d;
      tog := not tog;
      req <= tog;
      clocks := 0;
      loop
        wait until rising_edge(cpuclock);
        clocks := clocks + 1;
        exit when ready = tog;
        assert clocks < 100 report "no answer from the slow device bus" severity failure;
      end loop;
      if clocks > worst then
        worst := clocks;
      end if;
      got := rdata;
    end procedure;

    procedure expect (a : unsigned(27 downto 0); d : unsigned(7 downto 0); what : string) is
    begin
      slow('0', a, x"00");
      assert got = d
        report what & ": read " & integer'image(to_integer(got)) & ", expected "
               & integer'image(to_integer(d))
        severity failure;
    end procedure;

    constant REGS : unsigned(27 downto 0) := x"EFFF000";
    constant RAM  : unsigned(27 downto 0) := x"E000000";
  begin
    for i in 1 to 20 loop
      wait until rising_edge(cpuclock);
    end loop;

    -- the engine answers at its registers
    expect(REGS + 16#200#, x"4D", "ID");
    expect(REGS + 16#201#, x"47", "ID");
    expect(REGS + 16#207#, x"20", "voices");
    expect(REGS + 16#205#, x"20", "master volume");

    -- its sample RAM: written, read back, nothing beyond its end
    for i in 0 to 63 loop
      slow('1', RAM + 16#100# + i, to_unsigned(i * 4, 8));
    end loop;
    for i in 0 to 63 loop
      expect(RAM + 16#100# + i, to_unsigned(i * 4, 8), "sample RAM");
    end loop;
    slow('1', RAM + 16#1100#, x"AA");
    expect(RAM + 16#1100#, x"00", "beyond the sample RAM");
    expect(RAM + 16#100#, x"00", "sample RAM after a write beyond it");

    -- what else lives on this bus still answers, and so does the engine after it
    slow('0', x"7FEFFFF", x"00");
    slow('0', x"F000000", x"00");
    expect(REGS + 16#201#, x"47", "ID again");

    -- voice 3: a looping ramp, a little to the right
    slow('1', REGS + 16#31#, x"C0");      -- PAN
    slow('1', REGS + 16#32#, x"FF");      -- VOL
    slow('1', REGS + 16#33#, x"FF");
    slow('1', REGS + 16#34#, x"00");      -- FREQ: one sample a frame
    slow('1', REGS + 16#35#, x"00");
    slow('1', REGS + 16#36#, x"01");
    slow('1', REGS + 16#3A#, x"00");      -- START
    slow('1', REGS + 16#3B#, x"01");
    slow('1', REGS + 16#3C#, x"00");
    slow('1', REGS + 16#3D#, x"40");      -- END
    slow('1', REGS + 16#3E#, x"01");
    slow('1', REGS + 16#3F#, x"00");
    slow('1', REGS + 16#37#, x"00");      -- POS
    slow('1', REGS + 16#38#, x"01");
    slow('1', REGS + 16#39#, x"00");
    slow('1', REGS + 16#30#, x"03");      -- CTRL: run, loop
    expect(REGS + 16#30#, x"03", "CTRL right after the write");
    expect(REGS + 16#208#, x"08", "the running voices");

    wait for 3 ms;
    assert heard_left and heard_right report "the voice doesn't sound" severity failure;
    expect(REGS + 16#30#, x"03", "CTRL while it plays");
    slow('0', REGS + 16#37#, x"00");
    pos(7 downto 0) := got;
    slow('0', REGS + 16#38#, x"00");
    pos(15 downto 8) := got;
    slow('0', REGS + 16#39#, x"00");
    pos(23 downto 16) := got;
    assert pos >= 16#100# and pos < 16#140#
      report "POS outside the loop: " & integer'image(to_integer(pos)) severity failure;

    report "MegaGus on the slow device bus: PASS (the slowest access took "
           & integer'image(worst) & " CPU clocks)" severity note;
    done <= true;
    wait;
  end process;

end architecture;
