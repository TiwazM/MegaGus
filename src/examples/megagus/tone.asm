; MegaGus from assembly (ACME): a 440 Hz tone on voice 0 until a key is pressed.
;
;   acme -f cbm -o tone.prg tone.asm          then, in MEGA65 mode:  LOAD "TONE.PRG"  and  RUN
;
; docs/megagus-programming.md goes through it.

!cpu m65

ptr     = $fb                   ; a 32-bit pointer in the zero page, for lda/sta [ptr],z

* = $2001                       ; BASIC 65: 10 SYS 8210
        !word next, 10
        !byte $9e
        !text "8210"
        !byte 0
next    !word 0

        * = 8210
        ; --- the wave: 64 bytes from here into the sample RAM at $E000000, by DMA ---
        lda #$00                ; the list is in bank 0, megabyte 0
        sta $d702
        sta $d704
        lda #>dmalist
        sta $d701
        lda #<dmalist
        sta $d705               ; writing the low byte here starts an enhanced DMA job

        ; --- voice 0's registers, $EFFF000: PAN .. END from a table, then CTRL ---
        lda #$00
        sta ptr
        lda #$f0
        sta ptr+1
        lda #$ff
        sta ptr+2
        lda #$0e
        sta ptr+3               ; ptr = $0EFFF000
        ldz #0
        lda #0
        sta [ptr],z             ; CTRL = 0: stopped while it is set up
        ldz #1
-       tza
        tax
        lda regs-1,x
        sta [ptr],z
        inz
        cpz #16
        bne -
        ldz #0
        lda #$03                ; RUN + LOOP
        sta [ptr],z

        ; --- wait for a key, fade, stop ---
-       jsr $ffe4               ; GETIN
        beq -
        ldz #2
        lda #0
        sta [ptr],z             ; VOL = 0: the level ramps down, no click
        inz
        sta [ptr],z
        ldx #4                  ; a few frames of the screen for the ramp
--      lda $d012
-       cmp $d012
        beq -
        dex
        bne --
        ldz #0
        lda #0
        sta [ptr],z             ; CTRL = 0
        rts

regs    !byte 128               ; +1  PAN: the middle
        !word 24000             ; +2  VOL
        !24 38448               ; +4  FREQ = 440 * 64 * 65536 / 48000
        !24 0                   ; +7  POS: its last byte sets it
        !24 0                   ; +10 START
        !24 64                  ; +13 END

dmalist !byte $0b               ; options are in the F018B's form
        !byte $80, $00          ; source megabyte $00
        !byte $81, $e0          ; destination megabyte $E0: the sample RAM
        !byte $00               ; end of options
        !byte $00               ; copy
        !word 64                ; count
        !word wave              ; source address
        !byte $00               ; source bank
        !word $0000             ; destination address
        !byte $00               ; destination bank: with megabyte $E0, $E000000
        !byte $00               ; command high byte
        !word $0000             ; modulo

wave    !for i, 0, 63 { !byte int(127.0 * sin(i * 3.14159265 / 32.0) + 256.5) & 255 }
