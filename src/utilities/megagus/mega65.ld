/*
 * llvm-mos linker script for a MEGA65-mode program that lives beside the MEGA65's ROM
 * rather than switching it off (the MegaGus test program, examples and module player):
 *
 *   mos-mega65-clang -Os -mlto-zp=0 -T mega65.ld -o prog.prg prog.c
 *
 * The program loads to $2001 with a BASIC 65 line that starts it, so DLOAD and RUN do.
 * It runs in the memory as BASIC's SYS leaves it: RAM from $2001 to $BFFF, the KERNAL and
 * the I/O above that, interrupts on, so the KERNAL's screen and keyboard go on working.
 * (The SDK's own script takes $C000-$CFFF as RAM too and switches the ROM there off; ROM
 * 920413's interrupt handler needs it, and the first character printed hangs the machine.)
 *
 * Of the zero page it takes $02-$21 only, the compiler's 32 registers (hence -mlto-zp=0),
 * and puts BASIC's bytes back when the program ends: main() returns to READY.
 */
__basic_zp_start = 0x0002;
__basic_zp_end = 0x0022;

MEMORY {
    ram (rw) : ORIGIN = 0x2001, LENGTH = 0x9fff
}

INCLUDE commodore.ld

/* BASIC's zero page saved at the start and put back at the end */
INPUT(save-basic.o)

/* The C stack: from the top of the RAM down */
__stack = 0xc000;

OUTPUT_FORMAT {
    SHORT(0x2001)
    TRIM(ram)
}
