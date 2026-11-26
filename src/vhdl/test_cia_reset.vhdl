-- Testbench: a CIA's timers after power-on and after a reset are all ones, as a 6526's.
--
-- What it stands for: ROM 920413 reads CIA 2's timer A as it starts, before it has set the
-- timer up, and goes round until bit 6 or 7 of the low byte is set. With the timer at the 1
-- it had here after power-on that never came.
--
--   nvc --std=1993 -a src/vhdl/debugtools.vhdl src/vhdl/cia6526.vhdl src/vhdl/test_cia_reset.vhdl \
--       -e test_cia_reset -r test_cia_reset

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity test_cia_reset is
end entity;

architecture tb of test_cia_reset is
  signal clock : std_logic := '0';
  signal phi : std_logic := '0';
  signal reset : std_logic := '1';
  signal cs, wr : std_logic := '0';
  signal addr, wdata, rdata : unsigned(7 downto 0) := x"00";
  signal done : boolean := false;
begin
  clock <= not clock after 12.5 ns when not done;

  -- one pulse of the 1 MHz clock every 40 clocks
  process (clock)
    variable n : integer range 0 to 39 := 0;
  begin
    if rising_edge(clock) then
      phi <= '0';
      if n = 39 then
        n := 0;
        phi <= '1';
      else
        n := n + 1;
      end if;
    end if;
  end process;

  cia : entity work.cia6526
    generic map (unit => x"1")
    port map (
      cpuclock => clock, phi0_1mhz => phi, todclock => '0', reset => reset,
      cpu_slow => '0', hypervisor_mode => '0',
      cs => cs, fastio_address => addr, fastio_write => wr, fastio_wdata => wdata,
      fastio_rdata => rdata,
      portain => (others => '1'), portbin => (others => '1'),
      flagin => '1', spin => '1', countin => '1');

  process
    procedure poke (a, v : in integer) is
    begin
      wait until rising_edge(clock);
      addr <= to_unsigned(a, 8); wdata <= to_unsigned(v, 8); cs <= '1'; wr <= '1';
      wait until rising_edge(clock);
      cs <= '0'; wr <= '0';
      wait until rising_edge(clock);
    end procedure;
    procedure peek (a : in integer; v : out integer) is
    begin
      wait until rising_edge(clock);
      addr <= to_unsigned(a, 8); cs <= '1'; wr <= '0';
      wait until rising_edge(clock);
      wait until rising_edge(clock);
      v := to_integer(rdata);
      cs <= '0';
      wait until rising_edge(clock);
    end procedure;
    procedure all_ones (why : in string) is
      variable v : integer;
    begin
      for a in 4 to 7 loop
        peek(a, v);
        assert v = 255 report why & ": register " & integer'image(a) & " reads " & integer'image(v) severity failure;
      end loop;
    end procedure;
    variable v, most : integer;
  begin
    wait for 200 ns;
    all_ones("after power-on");

    -- a program leaves a count of 1 in timer A, running: it only ever reads 1 or 0
    poke(4, 1);
    poke(5, 0);
    poke(14, 1);
    wait for 5 us;      -- what is read follows the counter from its first tick on
    most := 0;
    for i in 1 to 200 loop
      peek(4, v);
      if v > most then
        most := v;
      end if;
    end loop;
    assert most <= 1 report "timer A with a latch of 1 read " & integer'image(most) severity failure;

    -- a reset: all ones again, and the low byte has the bit the ROM waits for
    reset <= '0';
    wait for 2 us;
    reset <= '1';
    wait for 200 ns;
    most := 0;
    for i in 1 to 200 loop
      peek(4, v);
      if v > most then
        most := v;
      end if;
    end loop;
    assert most >= 64 report "after a reset timer A's low byte never had bit 6 or 7 set" severity failure;
    peek(5, v);
    assert v = 255 report "after a reset timer A's high byte reads " & integer'image(v) severity failure;

    -- stopped and loaded by its high byte, it is what was written
    poke(14, 0);
    poke(4, 16#34#);
    poke(5, 16#12#);
    poke(14, 16#10#);  -- force load: the visible value follows
    peek(4, v);
    assert v = 16#34# report "timer A's low byte after a load reads " & integer'image(v) severity failure;
    peek(5, v);
    assert v = 16#12# report "timer A's high byte after a load reads " & integer'image(v) severity failure;

    report "PASS: the timers are all ones after power-on and after a reset";
    done <= true;
    wait;
  end process;
end architecture;
