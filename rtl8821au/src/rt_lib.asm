; SPDX-License-Identifier: GPL-2.0-only
;
; rt_lib.asm - runtime routines and DevHelp services of the RTL88EU driver (NASM, OMF output).
;
; Every routine follows the 16-bit C calling convention of the Open Watcom __cdecl: arguments on the
; stack pushed right to left, the caller removes them, the result is returned in AX (DX:AX for a long),
; and SI, DI, BP, DS are preserved. A far pointer argument occupies two words, offset first. The code
; assumes DS is the driver's data segment on entry (the kernel and USBD guarantee it at every entry point).
;
; The DevHelp register conventions are those of the IBM Physical Device Driver Reference, the page named in
; the comment above each service (listed in v012/PROVENANCE.md, section 1).
;
; Assemble with:  nasm -f obj rt_lib.asm -o rt_lib.obj

        bits 16

segment HEADER  class=HEADER   align=2 public use16
segment CONST   class=CONST    align=2 public use16
segment _DATA   class=DATA     align=2 public use16
segment _BSS    class=BSS      align=2 public use16
segment RTEND   class=ENDDATA  align=2 public use16
segment _TEXT   class=CODE     align=2 public use16

group DGROUP HEADER CONST _DATA _BSS RTEND

extern _rt_devhelp                              ; dd, defined in rt_hdr.asm

segment _TEXT

; ---- plain runtime ---------------------------------------------------------------------------

; USHORT rt_getds(void)
global _rt_getds
_rt_getds:
        mov     ax, ds
        ret

; void rt_setmem(void far *p, UCHAR c, USHORT n)
global _rt_setmem
_rt_setmem:
        push    bp
        mov     bp, sp
        push    di
        push    es
        les     di, [bp+4]
        mov     al, [bp+8]
        mov     cx, [bp+10]
        cld
        rep stosb
        pop     es
        pop     di
        pop     bp
        ret

; void rt_movmem(void far *dst, void far *src, USHORT n): safe when the ranges overlap
global _rt_movmem
_rt_movmem:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    ds
        push    es
        mov     cx, [bp+12]
        les     di, [bp+4]
        lds     si, [bp+8]
        mov     ax, es
        mov     dx, ds
        cld
        cmp     ax, dx
        jne     .fwd
        cmp     si, di
        jae     .fwd
        std                                     ; same segment and the source is below the destination
        add     si, cx
        dec     si
        add     di, cx
        dec     di
        rep movsb
        cld
        jmp     short .done
.fwd:
        shr     cx, 1
        rep movsw
        adc     cx, cx
        rep movsb
.done:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret

; UCHAR rt_tas(UCHAR *p): atomically set *p to 1, return its previous value
global _rt_tas
_rt_tas:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        mov     al, 1
        xchg    al, [bx]                        ; xchg with memory is implicitly locked
        xor     ah, ah
        pop     bp
        ret

; void rt_atomic_inc(USHORT *p) / rt_atomic_dec(USHORT *p)
global _rt_atomic_inc
_rt_atomic_inc:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        lock inc word [bx]
        pop     bp
        ret

global _rt_atomic_dec
_rt_atomic_dec:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        lock dec word [bx]
        pop     bp
        ret

global _rt_cli
_rt_cli:
        cli
        ret

global _rt_sti
_rt_sti:
        sti
        ret

; USHORT rt_getif(void): 1 when interrupts are enabled (IF), 0 when disabled
global _rt_getif
_rt_getif:
        pushf
        pop     ax
        shr     ax, 9
        and     ax, 1
        ret

; ---- DevHelp services --------------------------------------------------------------------------
; DL = function code, far call through the entry point handed over in the INIT packet.

; USHORT rt_dh_procblock(ULONG id, ULONG timeout, USHORT flag)     PDD "ProcBlock" (04h)
;   BX:AX = id (low:high), CX:DI = timeout ms (low:high, -1 = never), DH = 0 interruptible / 1 not.
;   Returns the awake code (AL).
global _rt_dh_procblock
_rt_dh_procblock:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     bx, [bp+4]
        mov     ax, [bp+6]
        mov     cx, [bp+8]
        mov     di, [bp+10]
        mov     dh, [bp+12]
        mov     dl, 0x04
        call    far [_rt_devhelp]
        xor     ah, ah
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_procrun(ULONG id, USHORT *awake)                    PDD "ProcRun" (05h)
;   BX:AX = id (low:high); returns the number of threads awakened in AX (stored through awake).
global _rt_dh_procrun
_rt_dh_procrun:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     bx, [bp+4]
        mov     ax, [bp+6]
        mov     dl, 0x05
        call    far [_rt_devhelp]
        mov     bx, [bp+8]
        mov     [bx], ax
        xor     ax, ax
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_verify(USHORT sel, USHORT len, USHORT off, UCHAR type)   PDD "VerifyAccess" (27h)
;   AX = selector, CX = length, DI = offset, DH = 0 read / 1 read-write. Returns 0 when access is valid.
global _rt_dh_verify
_rt_dh_verify:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     ax, [bp+4]
        mov     cx, [bp+6]
        mov     di, [bp+8]
        mov     dh, [bp+10]
        mov     dl, 0x27
        call    far [_rt_devhelp]
        jc      .bad
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_alloc_gdt(USHORT *sel, USHORT count)                PDD "AllocGDTSelector" (2Dh)
;   ES:DI = array of words that receives the selectors, CX = count. Returns 0 when successful.
global _rt_dh_alloc_gdt
_rt_dh_alloc_gdt:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    es
        mov     ax, ds
        mov     es, ax
        mov     di, [bp+4]
        mov     cx, [bp+6]
        mov     dl, 0x2D
        call    far [_rt_devhelp]
        jc      .bad
        xor     ax, ax
        jmp     short .out
.bad:
        or      ax, ax
        jnz     .out
        mov     ax, 1
.out:
        pop     es
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_phys_to_gdt(ULONG phys, USHORT len, USHORT sel)     PDD "PhysToGDTSelector" (2Eh)
;   AX:BX = physical address (high:low), CX = length, SI = selector. Returns 0 when successful.
global _rt_dh_phys_to_gdt
_rt_dh_phys_to_gdt:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     bx, [bp+4]
        mov     ax, [bp+6]
        mov     cx, [bp+8]
        mov     si, [bp+10]
        mov     dl, 0x2E
        call    far [_rt_devhelp]
        jc      .bad
        xor     ax, ax
        jmp     short .out
.bad:
        or      ax, ax
        jnz     .out
        mov     ax, 1
.out:
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_virt_to_phys(void far *p, ULONG *phys)              PDD "VirtToPhys" (16h)
;   DS:SI = virtual address; returns AX:BX = physical address (high:low). DS is replaced by the call, so the
;   DevHelp entry point is copied to the stack first.
global _rt_dh_virt_to_phys
_rt_dh_virt_to_phys:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    ds
        push    word [_rt_devhelp+2]
        push    word [_rt_devhelp]              ; entry point at [bp-10]
        lds     si, [bp+4]
        mov     dl, 0x16
        call    far [bp-10]
        mov     ds, [bp-6]
        jc      .bad
        mov     si, [bp+8]
        mov     [si], bx
        mov     [si+2], ax
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        add     sp, 4
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_getdosvar(USHORT index, USHORT member, void far * far *out)   PDD "GetDOSVar" (24h)
;   AL = index, CX = member; returns the pointer AX:BX (segment:offset), stored through out.
global _rt_dh_getdosvar
_rt_dh_getdosvar:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    es
        mov     ax, [bp+4]
        mov     cx, [bp+6]
        mov     dl, 0x24
        call    far [_rt_devhelp]
        jc      .bad
        les     di, [bp+8]
        mov     [es:di], bx
        mov     [es:di+2], ax
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        pop     es
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_attach_dd(char *name, USHORT *table)                PDD "AttachDD" (2Ah)
;   BX = name, DI = 12-byte area (3 reserved words, IDC entry offset, IDC entry CS, IDC driver DS).
global _rt_dh_attach_dd
_rt_dh_attach_dd:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     bx, [bp+4]
        mov     di, [bp+6]
        mov     dl, 0x2A
        call    far [_rt_devhelp]
        jc      .bad
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        pop     di
        pop     si
        pop     bp
        ret

; void rt_save_message(void *table)                                PDD "Save_Message" (3Dh)
;   DS:SI = message table, BX = 0.
global _rt_save_message
_rt_save_message:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     si, [bp+4]
        xor     bx, bx
        mov     dl, 0x3D
        call    far [_rt_devhelp]
        pop     di
        pop     si
        pop     bp
        ret

; ---- IDC ---------------------------------------------------------------------------------------

; void rt_idc_call(ULONG entry, USHORT ds, void far *rp)
;   Far call to another driver's IDC entry (PDD "AttachDD": the caller sets DS to the target's DS).
;   The USB stack's IDC routines are C far functions taking the far request-packet pointer on the stack
;   (USBPRM 6.1: void IDCRoutine(RPH_GENIOCTL FAR *pRP); calling convention checked by behaviour).
global _rt_idc_call
_rt_idc_call:
        push    bp
        mov     bp, sp
        push    si
        push    di
        push    ds
        push    word [bp+12]                    ; rp segment
        push    word [bp+10]                    ; rp offset
        mov     ds, [bp+8]
        call    far [bp+4]
        add     sp, 4
        pop     ds
        pop     di
        pop     si
        pop     bp
        ret

; ---- debug channel ---------------------------------------------------------------------------

; void tcom_always(char c): one raw byte to the COM1 UART (line status 3FDh bit 5 = transmit holding register
; empty, transmit data 3F8h). The wait is bounded so it never hangs.
global _tcom_always
_tcom_always:
        push    bp
        mov     bp, sp
        mov     cx, 0xFFFF
        mov     dx, 0x03FD
.wait:
        in      al, dx
        test    al, 0x20
        jnz     .send
        loop    .wait
.send:
        mov     dx, 0x03F8
        mov     al, [bp+4]
        out     dx, al
        pop     bp
        ret

; ---- arithmetic --------------------------------------------------------------------------------

; long rt_imul16(int a, int b): signed 16 x 16 -> 32 bit product in DX:AX (one IMUL; no 32-bit helper needed)
global _rt_imul16
_rt_imul16:
        push    bp
        mov     bp, sp
        mov     ax, [bp+4]
        imul    word [bp+6]
        pop     bp
        ret

; ---- AllocPhys ---------------------------------------------------------------------------------------

; USHORT rt_dh_alloc_phys(ULONG size, USHORT memtype, ULONG *phys)       PDD "AllocPhys" (18h)
;   AX:BX = size (high:low), DH = 0 above 1 MB / 1 below; returns AX:BX = physical address. 0 = ok.
global _rt_dh_alloc_phys
_rt_dh_alloc_phys:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     bx, [bp+4]
        mov     ax, [bp+6]
        mov     dh, [bp+8]
        mov     dl, 0x18
        call    far [_rt_devhelp]
        jc      .bad
        mov     si, [bp+10]
        mov     [si], bx
        mov     [si+2], ax
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        pop     di
        pop     si
        pop     bp
        ret

; ---- context hook (task-time work armed from the USB completion callback) ------------------------------
; PDD "AllocateCtxHook" (63h), "ArmCtxHook" (65h). The handler is entered by the kernel with EAX = the data given to
; ArmCtxHook and must save and restore every register it uses; it returns far.

; USHORT rt_dh_alloc_ctxhook(USHORT handler, ULONG *handle): EAX = handler offset, EBX = -1; returns the handle in EAX
global _rt_dh_alloc_ctxhook
_rt_dh_alloc_ctxhook:
        push    bp
        mov     bp, sp
        push    si
        push    di
        movzx   eax, word [bp+4]
        mov     ebx, 0xFFFFFFFF
        mov     dl, 0x63
        call    far [_rt_devhelp]
        jc      .bad
        mov     bx, [bp+6]
        mov     [bx], eax
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_dh_arm_ctxhook(ULONG data, ULONG handle): EAX = data, EBX = handle, ECX = -1; 0 when armed
global _rt_dh_arm_ctxhook
_rt_dh_arm_ctxhook:
        push    bp
        mov     bp, sp
        push    si
        push    di
        mov     eax, [bp+4]
        mov     ebx, [bp+8]
        mov     ecx, 0xFFFFFFFF
        mov     dl, 0x65
        call    far [_rt_devhelp]
        jc      .bad
        xor     ax, ax
        jmp     short .out
.bad:
        mov     ax, 1
.out:
        pop     di
        pop     si
        pop     bp
        ret

; USHORT rt_hook_offset(void): offset of the hook entry (for AllocateCtxHook)
global _rt_hook_offset
_rt_hook_offset:
        mov     ax, rt_hook_entry
        ret

extern _ndis_hook_c

; The hook entry: loads the driver's DS, runs the C handler, restores everything.
rt_hook_entry:
        pushad
        push    ds
        push    es
        mov     ax, DGROUP
        mov     ds, ax
        mov     es, ax
        cld
        call    _ndis_hook_c
        pop     es
        pop     ds
        popad
        retf

; ---- SMP helpers --------------------------------------------------------------------------------------
; These use 486/Pentium instructions (CMPXCHG, CPUID, PAUSE): every machine that runs an SMP kernel has them.
; 32-bit registers are saved and restored in full, so callers never see their high halves change.

; USHORT rt_cpu_id(void): initial local APIC id of the CPU running this code (CPUID leaf 1, EBX bits 31..24).
global _rt_cpu_id
_rt_cpu_id:
        push    ebx
        push    ecx
        push    edx
        push    eax
        mov     eax, 1
        cpuid
        shr     ebx, 24
        pop     eax                             ; restores the high half of EAX; AX is the result
        mov     ax, bx
        pop     edx
        pop     ecx
        pop     ebx
        ret

; USHORT rt_irq_save(void): returns FLAGS, then disables interrupts on this CPU
global _rt_irq_save
_rt_irq_save:
        pushf
        pop     ax
        cli
        ret

; void rt_irq_restore(USHORT flags): restores FLAGS (and with it the interrupt flag) saved by rt_irq_save
global _rt_irq_restore
_rt_irq_restore:
        push    bp
        mov     bp, sp
        push    word [bp+4]
        popf
        pop     bp
        ret

; USHORT rt_lock_try(USHORT *p, USHORT id): one attempt to change *p from 0 to id (id != 0).
; Returns 0 when the lock was taken, 1 when it is held (by anyone, including id).
global _rt_lock_try
_rt_lock_try:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        mov     dx, [bp+6]
        xor     ax, ax
        lock cmpxchg [bx], dx
        jz      .got
        mov     ax, 1
        pop     bp
        ret
.got:
        xor     ax, ax
        pop     bp
        ret

; USHORT rt_lock_acquire(USHORT *p, USHORT id): spin until *p changes from 0 to id. Returns 0 when taken, or 1 at
; once when *p already holds id (the same CPU re-entered: it must not wait for itself).
global _rt_lock_acquire
_rt_lock_acquire:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        mov     dx, [bp+6]
.try:
        xor     ax, ax
        lock cmpxchg [bx], dx                   ; ZF = 1: taken. ZF = 0: AX = current owner
        jz      .got
        cmp     ax, dx
        je      .self
        pause
        jmp     .try
.got:
        xor     ax, ax
        pop     bp
        ret
.self:
        mov     ax, 1
        pop     bp
        ret

; void rt_lock_release(USHORT *p): *p = 0 with a locked exchange (full barrier)
global _rt_lock_release
_rt_lock_release:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        xor     ax, ax
        xchg    ax, [bx]
        pop     bp
        ret

; USHORT rt_wait_zero(USHORT *p, USHORT rounds): spin with PAUSE until *p is 0, at most rounds x 65536 passes
; (rounds >= 1). Memory is read again on every pass. Returns 0 when *p became 0, 1 on timeout.
global _rt_wait_zero
_rt_wait_zero:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
        mov     dx, [bp+6]
.outer:
        xor     cx, cx
.inner:
        cmp     word [bx], 0
        je      .zero
        pause
        loop    .inner                          ; CX = 0 on entry: 65536 passes
        dec     dx
        jnz     .outer
        mov     ax, 1
        pop     bp
        ret
.zero:
        xor     ax, ax
        pop     bp
        ret

; USHORT rt_atomic_dec_nz(USHORT *p): if *p != 0, decrement it atomically and return 1; return 0 when it was 0
global _rt_atomic_dec_nz
_rt_atomic_dec_nz:
        push    bp
        mov     bp, sp
        mov     bx, [bp+4]
.retry:
        mov     ax, [bx]
        or      ax, ax
        jz      .zero
        mov     dx, ax
        dec     dx
        lock cmpxchg [bx], dx
        jnz     .retry
        mov     ax, 1
        pop     bp
        ret
.zero:
        pop     bp
        ret
