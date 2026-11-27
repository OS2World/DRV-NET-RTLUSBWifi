; SPDX-License-Identifier: GPL-2.0-only
;
; hdr.asm - device driver header, segment layout and strategy entry of the RTL8821AE OS/2 driver
; (NASM, OMF output).
;
; Derived from rt_hdr.asm of the OS2World RTL88EU driver (GPL-2.0-only), which was written from the IBM
; Physical Device Driver Reference ("Physical Device Driver Header", "Device Attribute").
; The header must be the first item of the first data segment, so it lives in its own segment HEADER
; that the link order puts first. This module is linked FIRST and declares every segment, which fixes the
; class order (HEADER, CONST, DATA, BSS, ENDDATA, CODE); dd_end.asm is linked LAST.
;
; Assemble with:  nasm -f obj dd_hdr.asm -o dd_hdr.obj

        bits 16

segment HEADER  class=HEADER   align=2 public use16
segment CONST   class=CONST    align=2 public use16
segment _DATA   class=DATA     align=2 public use16
segment _BSS    class=BSS      align=2 public use16
segment RTEND   class=ENDDATA  align=2 public use16
segment _TEXT   class=CODE     align=2 public use16

group DGROUP HEADER CONST _DATA _BSS RTEND

; attribute word: bit 15 character device, bit 11 open/close requests,
; bits 9-7 = 011 (header carries a capabilities strip). Bit 14 = generic IOCtl (DosDevIOCtl category/function)
; support: without it the kernel never passes the GenMAC calls (category 99h) to the strategy routine.
; (This bit is the one the RTL driver sets and calls "IDC"; there is no IDC entry in this driver.)
ATTR_IOCTL      equ 0x4000
ATTR_CHAR       equ 0x8000
ATTR_OPENCLOSE  equ 0x0800
ATTR_LEVEL3     equ 0x0180
; capabilities strip: bit 4 = the INIT COMPLETE command (1Fh) is supported
CAP_INITCOMPLETE equ 0x00000010

extern _rt_strategy                             ; C: void __cdecl rt_strategy(RPH far *rp)

segment HEADER
global rt_head
rt_head:
        dd      -1                              ; far pointer to the next header (none)
        dw      ATTR_CHAR | ATTR_IOCTL | ATTR_OPENCLOSE | ATTR_LEVEL3
        dw      rt_strategy_entry               ; offset of the strategy routine
        dw      0                               ; no IDC entry point
        db      'WRND32$ '                      ; 8-byte device name: the GenMAC wrapper name that the OS/2
                                                ; wpa_supplicant and the Wireless LAN Monitor open
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
