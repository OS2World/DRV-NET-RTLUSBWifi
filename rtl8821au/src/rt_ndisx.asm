; SPDX-License-Identifier: GPL-2.0-only
;
; rt_ndisx.asm - NDIS MAC entry stubs of the RTL88EU driver (NASM, OMF output).
;
; NDIS 2.0.1: the protocol calls the MAC with Microsoft C FAR Pascal conventions: the arguments are pushed left to
; right, the last one is the called module's data segment, the callee removes them (RETF n). The called module saves
; the caller's DS, loads its own from that last parameter and restores it on return; SI, DI, BP are preserved and the
; direction flag is clear on entry and exit. Each stub does that and calls a C function with the remaining arguments
; in __cdecl order.
;
; Assemble with:  nasm -f obj rt_ndisx.asm -o rt_ndisx.obj

        bits 16

segment HEADER  class=HEADER   align=2 public use16
segment CONST   class=CONST    align=2 public use16
segment _DATA   class=DATA     align=2 public use16
segment _BSS    class=BSS      align=2 public use16
segment RTEND   class=ENDDATA  align=2 public use16
segment _TEXT   class=CODE     align=2 public use16

group DGROUP HEADER CONST _DATA _BSS RTEND

segment _TEXT

extern _ndis_c_sysreq
extern _ndis_c_genreq
extern _ndis_c_transmit
extern _ndis_c_transfer
extern _ndis_c_release
extern _ndis_c_indon
extern _ndis_c_indoff

; Frame after "push bp / mov bp,sp / push ds": [bp+0] caller bp, [bp+2] ret ip, [bp+4] ret cs, [bp+6] last pascal parameter.

; WORD far pascal SystemRequest(DWORD param1, DWORD param2, WORD param3, WORD opcode, WORD targetDS)
global _ndis_sysreq_entry
_ndis_sysreq_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        push    word [bp+8]                     ; opcode
        push    word [bp+10]                    ; param3
        push    word [bp+14]                    ; param2 high
        push    word [bp+12]                    ; param2 low
        push    word [bp+18]                    ; param1 high
        push    word [bp+16]                    ; param1 low
        call    _ndis_c_sysreq
        add     sp, 12
        pop     ds
        pop     bp
        retf    14

; WORD far pascal GeneralRequest(WORD protID, WORD reqHandle, WORD param1, DWORD param2, WORD opcode, WORD macDS)
global _ndis_genreq_entry
_ndis_genreq_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        push    word [bp+8]                     ; opcode
        push    word [bp+12]                    ; param2 high
        push    word [bp+10]                    ; param2 low
        push    word [bp+14]                    ; param1
        push    word [bp+16]                    ; reqHandle
        push    word [bp+18]                    ; protID
        call    _ndis_c_genreq
        add     sp, 12
        pop     ds
        pop     bp
        retf    14

; WORD far pascal TransmitChain(WORD protID, WORD reqHandle, void far *txBufDescr, WORD macDS)
global _ndis_transmit_entry
_ndis_transmit_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        push    word [bp+10]                    ; descriptor segment
        push    word [bp+8]                     ; descriptor offset
        push    word [bp+12]                    ; reqHandle
        push    word [bp+14]                    ; protID
        call    _ndis_c_transmit
        add     sp, 8
        pop     ds
        pop     bp
        retf    10

; WORD far pascal TransferData(WORD far *bytesCopied, WORD frameOffset, void far *tdBufDescr, WORD macDS)
global _ndis_transfer_entry
_ndis_transfer_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        push    word [bp+10]                    ; descriptor segment
        push    word [bp+8]                     ; descriptor offset
        push    word [bp+12]                    ; frameOffset
        push    word [bp+16]                    ; bytesCopied segment
        push    word [bp+14]                    ; bytesCopied offset
        call    _ndis_c_transfer
        add     sp, 10
        pop     ds
        pop     bp
        retf    12

; WORD far pascal ReceiveRelease(WORD reqHandle, WORD macDS)
global _ndis_release_entry
_ndis_release_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        push    word [bp+8]                     ; reqHandle
        call    _ndis_c_release
        add     sp, 2
        pop     ds
        pop     bp
        retf    4

; WORD far pascal IndicationOn(WORD macDS)
global _ndis_indon_entry
_ndis_indon_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        call    _ndis_c_indon
        pop     ds
        pop     bp
        retf    2

; WORD far pascal IndicationOff(WORD macDS)
global _ndis_indoff_entry
_ndis_indoff_entry:
        push    bp
        mov     bp, sp
        push    ds
        mov     ds, [bp+6]
        call    _ndis_c_indoff
        pop     ds
        pop     bp
        retf    2
