#!/usr/bin/env python3
"""
Loads a mega65-core bitstream over JTAG and keeps it running, with m65rs (MEGA65 Studio).

A core with MEGAFLASH in it restarts the FPGA from flash slot 1 on its first boot, so a
bitstream sent with `m65rs jtag bit` alone is gone again within a second. This halts the
CPU the moment the new core's serial monitor answers, sets the Hypervisor's own "not the
first boot" flag (the BIT at first_boot_flag_instruction becomes a JMP, as Hyppo does itself
later), and resets. Nothing is written to the flash: a power cycle brings the old core back.

  ./jtagboot.py [--timer] [FILE.bit]    default: bin/mega65r6.bit, the flag's address from
                                        src/hyppo/HICKUP.sym of the same build

--timer is for a core built before 2026-11-26 with ROM 920413 on the SD card: that ROM
waits on CIA 2's timer A as it starts, which in those cores held 1 and never showed what the
ROM waits for; the option writes $FFFF to it. Later cores have a 6526's own value there.

M65RS and M65_DEVICE in the environment name the tool and the serial port (macOS).
"""

import array
import fcntl
import os
import re
import subprocess
import sys
import termios
import time

PORT = os.environ.get("M65_DEVICE", "/dev/cu.usbserial-101")
M65RS = os.environ.get("M65RS", "/Applications/mega65studio/m65rs")
TOP = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
IOSSIOSPEED = 0x80045402


def open_port():
    fd = os.open(PORT, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = 0
    a[1] = 0
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[3] = 0
    a[6][termios.VMIN] = 0
    a[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, a)
    fcntl.ioctl(fd, IOSSIOSPEED, array.array("i", [2000000]), 1)
    return fd


def drain(fd, quiet=0.05):
    out = b""
    last = time.time()
    while time.time() - last < quiet:
        try:
            d = os.read(fd, 65536)
        except BlockingIOError:
            d = b""
        if d:
            out += d
            last = time.time()
        else:
            time.sleep(0.002)
    return out


def cmd(fd, text, quiet=0.08):
    os.write(fd, text.encode() + b"\r")
    return drain(fd, quiet).decode("latin1")


def byte_at(fd, addr):
    m = re.search(r":0?%07X:([0-9A-F]{2})" % addr, cmd(fd, f"m{addr:07x}").upper())
    return m.group(1) if m else None


def main():
    args = [a for a in sys.argv[1:] if a != "--timer"]
    timer = "--timer" in sys.argv[1:]
    bit = args[0] if args else os.path.join(TOP, "bin", "mega65r6.bit")
    sym = open(os.path.join(TOP, "src", "hyppo", "HICKUP.sym")).read()
    flag = int(re.search(r"\bfirst_boot_flag_instruction\s*=\s*\$([0-9a-fA-F]+)", sym).group(1), 16)
    addr = 0xFFF8000 + (flag - 0x8000)
    fd = open_port()
    drain(fd, 0.1)

    p = subprocess.Popen([M65RS, "jtag", "bit", bit], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    t0 = time.time()
    seen = b""
    alive = None
    gone = None                 # the old core stops answering while the FPGA is being configured
    last = time.time()
    while time.time() - t0 < 60:
        try:
            os.write(fd, b"t1\r" if gone else b"\r")
        except BlockingIOError:
            pass
        try:
            d = os.read(fd, 65536)
        except BlockingIOError:
            d = b""
        now = time.time()
        if d:
            last = now
            if gone:
                seen += d
                if alive is None and b"t1" in seen:
                    alive = now
        elif gone is None and now - last > 0.3:
            gone = now
        if alive is not None and now - alive > 0.02:
            break
        time.sleep(0.001)
    if alive is None:
        print(p.communicate()[0].strip())
        sys.exit("the new core's monitor never answered")
    drain(fd, 0.1)

    was = byte_at(fd, addr)
    if was not in ("2C", "4C"):
        cmd(fd, "t0")
        print(p.communicate()[0].strip())
        sys.exit(f"${addr:07X} holds {was}, not Hyppo's first boot flag: caught too late, or HICKUP.sym "
                 "is of another build. Left alone.")
    cmd(fd, f"s{addr:07x} 4c")
    now_is = byte_at(fd, addr)
    if timer:
        # ROM 920413 waits on CIA 2's timer A before it sets it up; in a core from before
        # 2026-11-26 the timer holds its power-on value of 1 and BASIC never starts
        cmd(fd, "sffd3d04 ff ff")
    cmd(fd, "!", 0.2)
    cmd(fd, "t0", 0.2)
    print(p.communicate()[0].strip())
    os.close(fd)
    if now_is != "4C":
        sys.exit(f"the first boot flag reads {now_is} after writing it")
    print(f"caught {alive - t0:.1f} s after the start; the first boot flag at ${addr:07X} was ${was}, is $4C; reset")


if __name__ == "__main__":
    main()
