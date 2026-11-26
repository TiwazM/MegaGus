/*
 * llvm-mos linker script for the module player (modplay.c, modplay65.s): a MEGA65-mode
 * program in parts, since all of it is more than there is room for at once.
 *
 *   $2001   the main part: code, texts, initialised variables: the program file
 *   $6200   the variables, and the C stack down from $9A00
 *   $9A00   OVLA: the player of the module's kind: MOD and S3M (overlay 3) or XM (4)
 *   $D000   I/O
 *   $E000   OVLB: the file list (overlay 1) or the playing screens (2)
 *   $F800   two buffers, to $FFF9; then the processor's vectors
 *
 * The program runs with the interrupts off and, once it has started, with the ROM switched
 * off at $C000 and $E000. The four overlays are not in the program file: they are one
 * behind the other in a second file, MODPLAY.OVL (the Makefile cuts them out of the linked
 * program), which the program's BASIC line loads to the start of bank 5 before it starts
 * the program; from there each is copied into OVLA or OVLB when it is needed.
 *
 * All of BASIC's zero page is the compiler's; it is saved and put back (save-basic.o).
 */
__basic_zp_start = 0x0002;
__basic_zp_end = 0x0090;

__vars = 0x6200;
__ovla = 0x9a00;
__ovlb = 0xe000;
__high = 0xf800;

MEMORY {
    ram  (rw) : ORIGIN = 0x2001, LENGTH = __vars - 0x2001
    vars (rw) : ORIGIN = __vars, LENGTH = __ovla - __vars
    ovla (rw) : ORIGIN = __ovla, LENGTH = 0xd000 - __ovla
    ovlb (rw) : ORIGIN = __ovlb, LENGTH = __high - __ovlb
    high (rw) : ORIGIN = __high, LENGTH = 0xfffa - __high
}

__rc0 = __basic_zp_start;
INCLUDE imag-regs.ld
__basic_zp_size = __basic_zp_end - __basic_zp_start;
MEMORY { zp : ORIGIN = __rc31 + 1, LENGTH = __basic_zp_end - (__rc31 + 1) }

REGION_ALIAS("c_readonly", ram)
REGION_ALIAS("c_writeable", ram)

INPUT(save-basic.o)

SECTIONS {
    .basic_header : { KEEP(*(.basic_header)) } >ram
    .text : { INCLUDE text-sections.ld } >ram
    .rodata : { INCLUDE rodata-sections.ld } >ram
    .data : { INCLUDE data-sections.ld } >ram
    INCLUDE data-symbols.ld
    INCLUDE zp.ld   /* behind the rest, so that the code begins right after the BASIC line */
    __main_end = .;

    /* Each overlay is linked to run in its place; none is in the program file. Two share
       each place, so each is given a load address of its own, far from everything (nothing
       is ever loaded there): the linker would otherwise write one over the other. */
    .ovl1 __ovlb : AT(0x100000) { *(ovl1.text) *(ovl1.rodata) } >ovlb
    .ovl2 __ovlb : AT(0x110000) { *(ovl2.text) *(ovl2.rodata) } >ovlb
    .ovl3 __ovla : AT(0x120000) { *(ovl3.text) *(ovl3.rodata) } >ovla
    .ovl4 __ovla : AT(0x130000) { *(ovl4.text) *(ovl4.rodata) } >ovla

    /* a page for the Hypervisor's directory entries, which it wants below $8000; in a
       section of its own: what is in .bss is cleared when the program starts, by its size */
    .lowpage __vars (NOLOAD) : { *(lowpage) } >vars
    .bss (NOLOAD) : { INCLUDE bss-sections.ld } >vars
    INCLUDE bss-symbols.ld
    .noinit (NOLOAD) : { INCLUDE noinit-sections.ld } >vars
    .high (NOLOAD) : { __high_start = .; *(high) __high_end = .; } >high
}

ASSERT(_start == 8231, "the program does not begin where its BASIC line says")
ASSERT(SIZEOF(.ovl1) <= __high - __ovlb, "overlay 1 is too big for OVLB")
ASSERT(SIZEOF(.ovl2) <= __high - __ovlb, "overlay 2 is too big for OVLB")
ASSERT(SIZEOF(.ovl3) <= 0xd000 - __ovla, "overlay 3 is too big for OVLA")
ASSERT(SIZEOF(.ovl4) <= 0xd000 - __ovla, "overlay 4 is too big for OVLA")

/* how long each overlay is, and where it begins in MODPLAY.OVL and so in bank 5 */
__ovl1_size = SIZEOF(.ovl1);
__ovl2_size = SIZEOF(.ovl2);
__ovl3_size = SIZEOF(.ovl3);
__ovl4_size = SIZEOF(.ovl4);
__ovl2_image = __ovl1_size;
__ovl3_image = __ovl2_image + __ovl2_size;
__ovl4_image = __ovl3_image + __ovl3_size;

/* The C stack: down from OVLA */
__stack = __ovla;

OUTPUT_FORMAT {
    SHORT(0x2001)
    TRIM(ram)
}
