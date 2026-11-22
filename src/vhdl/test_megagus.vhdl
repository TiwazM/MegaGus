--
-- Testbench for megagus.vhdl: plays a script of bus accesses against the engine and writes
-- down every frame's output, every access's result and the clock edge each took effect at,
-- for src/tests/megagus/megagus_test.py to replay through its bit-exact model of the engine.
--
-- The script (megagus_stim.txt, decimal numbers):
--   W <ram> <addr> <data>   write a register (ram = 0) or a sample RAM byte (ram = 1)
--   R <ram> <addr>          read one
--   F <n>                   wait for the quiet part (clock 600) of the n-th frame from here
--   J <n>                   wait n clocks
--   X                       reset
--
-- The output (megagus_out.txt):
--   T <edge>                a frame began: the clock edge its tick was at
--   S <left> <right>        that frame's output
--   A <edge> <data>         an access (or a reset) took effect at this clock edge; what a
--                           read returned
--
-- Accesses fall wherever the script puts them, in the middle of a voice too: the model
-- replays them at the same clock edge.
--

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;
use std.textio.all;

entity test_megagus is
end entity;

architecture tb of test_megagus is
  signal clock : std_logic := '0';
  signal reset : std_logic := '0';
  signal done  : boolean := false;

  signal req_toggle : std_logic := '0';
  signal req_write  : std_logic := '0';
  signal req_ram    : std_logic := '0';
  signal req_addr   : unsigned(23 downto 0) := (others => '0');
  signal req_wdata  : unsigned(7 downto 0) := x"00";
  signal ack_toggle : std_logic;
  signal ack_rdata  : unsigned(7 downto 0);

  signal frame_tick  : std_logic;
  signal audio_left  : signed(15 downto 0);
  signal audio_right : signed(15 downto 0);

  -- Before a clock edge: that edge's number, and how many edges since the frame's tick
  signal abs_edge   : integer := 0;
  signal since_tick : integer := 1000;      -- no frame yet

  file fout : text open write_mode is "megagus_out.txt";
begin

  clock <= not clock after 12345 ps when not done else '0';

  dut: entity work.megagus
    generic map (
      clock_frequency => 40500000,
      sample_rate     => 48000,
      ram_addr_bits   => 12
      )
    port map (
      clock => clock,
      reset => reset,
      req_toggle => req_toggle,
      req_write => req_write,
      req_ram => req_ram,
      req_addr => req_addr,
      req_wdata => req_wdata,
      ack_toggle => ack_toggle,
      ack_rdata => ack_rdata,
      frame_tick => frame_tick,
      audio_left => audio_left,
      audio_right => audio_right
      );

  -- Every frame: where it began, and what came out (taken at clock 600, after the engine
  -- has done all 32 voices and the master volume)
  process (clock)
    variable lo : line;
  begin
    if rising_edge(clock) then
      abs_edge <= abs_edge + 1;
      if frame_tick = '1' then
        write(lo, string'("T "));
        write(lo, abs_edge - 1);
        writeline(fout, lo);
        since_tick <= 2;
      else
        since_tick <= since_tick + 1;
        if since_tick = 600 then
          write(lo, string'("S "));
          write(lo, to_integer(audio_left));
          write(lo, string'(" "));
          write(lo, to_integer(audio_right));
          writeline(fout, lo);
        end if;
      end if;
    end if;
  end process;

  process
    file fin : text open read_mode is "megagus_stim.txt";
    variable l, lo : line;
    variable op : character;
    variable a, b, c : integer;
    variable tog : std_logic := '0';

    procedure log_access (edge, data : integer) is
    begin
      write(lo, string'("A "));
      write(lo, edge);
      write(lo, string'(" "));
      write(lo, data);
      writeline(fout, lo);
    end procedure;

    procedure access_bus (wr : std_logic; ram, addr, data : integer) is
    begin
      req_write <= wr;
      if ram = 1 then
        req_ram <= '1';
      else
        req_ram <= '0';
      end if;
      req_addr <= to_unsigned(addr, 24);
      req_wdata <= to_unsigned(data, 8);
      tog := not tog;
      req_toggle <= tog;
      loop
        wait until rising_edge(clock);
        exit when ack_toggle = tog;
      end loop;
      -- The engine acted on it one clock before its acknowledge showed (a register), or
      -- three (the sample RAM)
      if ram = 1 then
        log_access(abs_edge - 3, to_integer(ack_rdata));
      else
        log_access(abs_edge - 1, to_integer(ack_rdata));
      end if;
    end procedure;
  begin
    wait until rising_edge(clock);
    while not endfile(fin) loop
      readline(fin, l);
      read(l, op);
      case op is
        when 'W' =>
          read(l, a); read(l, b); read(l, c);
          access_bus('1', a, b, c);
        when 'R' =>
          read(l, a); read(l, b);
          access_bus('0', a, b, 0);
        when 'F' =>
          read(l, a);
          for i in 1 to a loop
            wait until rising_edge(clock) and frame_tick = '1';
            for j in 1 to 600 loop
              wait until rising_edge(clock);
            end loop;
          end loop;
        when 'J' =>
          read(l, a);
          for i in 1 to a loop
            wait until rising_edge(clock);
          end loop;
        when 'X' =>
          wait until rising_edge(clock);
          reset <= '1';
          log_access(abs_edge + 1, 0);
          wait until rising_edge(clock);
          wait until rising_edge(clock);
          reset <= '0';
        when others =>
          null;
      end case;
    end loop;
    done <= true;
    wait;
  end process;

end architecture;
