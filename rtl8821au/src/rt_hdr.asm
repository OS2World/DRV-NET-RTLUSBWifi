; SPDX-License-Identifier: GPL-2.0-only
;
; rt_hdr.asm - device driver header, segment layout and strategy entry of the RTL88EU driver
; (NASM, OMF output).
;
; Written from the IBM Physical Device Driver Reference, "Physical Device Driver Header" and
; "Device Attribute" (see v012/PROVENANCE.md, section 2). The header must be the first item of
; the first data segment, so it lives in its own segment HEADER that the link order puts first.
; This module is linked FIRST and declares every segment, which fixes the class order
; (HEADER, CONST, DATA, BSS, ENDDATA, CODE); rt_end.asm is linked LAST and carries the
; resident-end markers.
;
; Assemble with:  nasm -f obj rt_hdr.asm -o rt_hdr.obj

        bits 16

segment HEADER  class=HEADER   align=2 public use16
segment CONST   class=CONST    align=2 public use16
segment _DATA   class=DATA     align=2 public use16
segment _BSS    class=BSS      align=2 public use16
segment RTEND   class=ENDDATA  align=2 public use16
segment _TEXT   class=CODE     align=2 public use16

group DGROUP HEADER CONST _DATA _BSS RTEND

; attribute word: bit 15 character device, bit 14 IDC entry present, bit 11 open/close requests,
; bits 9-7 = 011 (header carries a capabilities strip).
ATTR_CHAR       equ 0x8000
ATTR_IDC        equ 0x4000
ATTR_OPENCLOSE  equ 0x0800
ATTR_LEVEL3     equ 0x0180
; capabilities strip: bit 4 = the INIT COMPLETE command (1Fh) is supported
CAP_INITCOMPLETE equ 0x00000010

extern _rt_strategy                             ; C: void __cdecl rt_strategy(RPH far *rp)
extern _rt_idc                                  ; C: void __far __cdecl rt_idc(RP_GENIOCTL far *rp)

segment HEADER
global rt_head
rt_head:
        dd      -1                              ; far pointer to the next header (none)
        dw      ATTR_CHAR | ATTR_IDC | ATTR_OPENCLOSE | ATTR_LEVEL3
        dw      rt_strategy_entry               ; offset of the strategy routine
        dw      _rt_idc                         ; offset of the IDC entry point
        db      'WRND32$ '                      ; 8-byte device name: the GenMAC wrapper name the OS/2
                                                ; wpa_supplicant looks for
        times 8 db 0                            ; reserved
        dd      CAP_INITCOMPLETE                ; capabilities bit strip

segment _DATA
global _rt_devhelp
_rt_devhelp:                                    ; DevHelp entry point, filled in from the INIT packet
        dd      0

segment _TEXT

; Strategy entry: called far with the request packet at ES:BX; DS is the driver's data segment.
global rt_strategy_entry
rt_strategy_entry:
        push    es
        push    bx                              ; far pointer argument: offset at the lower address
        call    _rt_strategy
        add     sp, 4
        retf
