; What modplay.c needs of the MEGA65 in assembly (llvm-mos syntax; modplay.ld is the plan
; of memory this goes with).

; ---------------------------------------------------------------------------------------
; The BASIC 65 line the program file begins with:
;
;   10 BLOAD"MODPLAY.OVL",B5,P0:BANK0:SYS8231
;
; BASIC itself loads the overlays' file, from the disk the program came from, to the start
; of bank 5, and then starts the program (modplay.ld checks that it does begin at 8231).
; ---------------------------------------------------------------------------------------
        .section .basic_header,"a",@progbits
        .short  basic_end               ; where the next line would be
        .short  10                      ; the line's number
        .byte   $fe, $11                ; BLOAD
        .ascii  "\"MODPLAY.OVL\",B5,P0:"
        .byte   $fe, $02                ; BANK
        .ascii  "0:"
        .byte   $9e                     ; SYS
        .ascii  "8231"
        .byte   0                       ; the line's end
basic_end:
        .short  0                       ; the program's end

; ---------------------------------------------------------------------------------------
; unsigned char hyppo(unsigned char function, unsigned char x, unsigned char y);
;
; The Hypervisor's DOS function, with X and Y as given. Returns 1 when it came back with
; the carry set (it worked), else 0; what it left in A, X and Y is in hy_a, hy_x, hy_y.
; Z is 0 again afterwards, as compiled code expects it.
; (llvm-mos passes the first two bytes in A and X and the third in __rc2.)
; ---------------------------------------------------------------------------------------
        .global hyppo
        .section .text.hyppo,"ax",@progbits
hyppo:
        ldy     mos8(__rc2)
        sta     $d640                   ; the trap
        clv                             ; the byte after it is skipped
        sta     hy_a
        stx     hy_x
        sty     hy_y
        ldz     #0
        lda     #0
        rol                             ; the carry
        rts

; ---------------------------------------------------------------------------------------
; void map_none(void): nothing mapped, so $E000-$FFFF is RAM (the KERNAL is gone).
; void map_basic(void): the mapping a program started by BASIC's SYS runs in: the KERNAL
;   at $E000.
; nmi_ignore: where the RAM's own NMI vector points meanwhile (the RESTORE key).
; ---------------------------------------------------------------------------------------
        .global map_none, map_basic, nmi_ignore
        .section .text.map,"ax",@progbits
map_none:
        lda     #0
        tax
        tay
        taz
        map
        eom
        rts
map_basic:
        lda     #$00
        ldx     #$e0
        ldy     #$00
        ldz     #$83
        map
        eom
        ldz     #0
        rts
nmi_ignore:
        rti
