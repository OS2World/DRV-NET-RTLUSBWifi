; SPDX-License-Identifier: GPL-2.0-only
;
; rt_end.asm - resident-end markers of the RTL88EU driver (NASM, OMF output).
;
; The INIT packet reports where the resident code and data end; everything after is released.
; This module must be linked LAST so its labels are the highest offsets in their segments.
;
; Assemble with:  nasm -f obj rt_end.asm -o rt_end.obj

        bits 16

segment HEADER  class=HEADER   align=2 public use16
segment CONST   class=CONST    align=2 public use16
segment _DATA   class=DATA     align=2 public use16
segment _BSS    class=BSS      align=2 public use16
segment RTEND   class=ENDDATA  align=2 public use16
segment _TEXT   class=CODE     align=2 public use16

group DGROUP HEADER CONST _DATA _BSS RTEND

segment RTEND
rt_data_end_mark:

segment _TEXT

; unsigned __cdecl rt_code_end(void): offset of the end of the code segment
global _rt_code_end
_rt_code_end:
        mov     ax, rt_code_end_mark
        ret

; unsigned __cdecl rt_data_end(void): offset of the end of the data group
global _rt_data_end
_rt_data_end:
        mov     ax, rt_data_end_mark wrt DGROUP
        ret

rt_code_end_mark:
