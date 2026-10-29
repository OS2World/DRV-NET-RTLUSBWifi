/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rt_os2dd.h - OS/2 physical device driver definitions used by the RTL88EU driver.
 *
 * Written from the IBM Physical Device Driver Reference (request packets, status word,
 * INIT/OPEN/CLOSE/generic IOCtl commands, DevHelp services). The origin of every number is
 * listed in v012/PROVENANCE.md. Compile with structure packing of 1 (wcc -zp1) and -zu
 * (SS is not DS in a driver).
 */
#ifndef RT_OS2DD_H
#define RT_OS2DD_H

typedef unsigned char   UCHAR;
typedef unsigned short  USHORT;
typedef unsigned long   ULONG;
typedef char            CHAR;
typedef short           SHORT;
typedef long            LONG;

#define FAR             __far
#define NULL            0

typedef void FAR        *PVOID;
typedef PVOID FAR       *PPVOID;
typedef char FAR        *PSZ;
typedef UCHAR FAR       *PUCHAR;
typedef USHORT FAR      *PUSHORT;
typedef ULONG FAR       *PULONG;

/* Far pointer parts (the pointer is an lvalue). */
#define SELECTOROF(p)   (((USHORT FAR *)&(p))[1])
#define OFFSETOF(p)     (((USHORT FAR *)&(p))[0])

/* Build a far pointer from a selector and an offset. */
#define MAKEP(sel, off) ((PVOID)(((ULONG)(USHORT)(sel) << 16) | (USHORT)(off)))

#pragma pack(push, 1)

/* Request packet header, 13 bytes (PDD "Request Packets"). */
typedef struct {
   UCHAR  Len;                  /* total length of the packet                            */
   UCHAR  Unit;                 /* block device unit (unused for character devices)      */
   UCHAR  Cmd;                  /* strategy command code                                  */
   USHORT Status;               /* result, see ST_*                                       */
   ULONG  Reserved;
   ULONG  Link;                 /* queue linkage                                          */
} RPH;

/* INIT (command 00h): PDD "INIT - 0h", request block format and remarks. */
typedef struct {
   RPH    rph;
   UCHAR  Data1;                /* out: units (block devices); 0 for character devices   */
   union {
      ULONG DevHlp;             /* in : far pointer to the DevHlp entry point            */
      struct {
         USHORT CodeEnd;        /* out: offset of the end of the code segment            */
         USHORT DataEnd;        /* out: offset of the end of the data segment            */
      } End;
   } p1;
   PSZ    Args;                 /* in : far pointer to the DEVICE= arguments             */
   UCHAR  Data2;                /* in : drive number of the first block unit             */
} RP_INIT;

/* Generic IOCtl (command 10h) with DosDevIOCtl2 lengths (attribute level 3). PDD "GENERIC IOCtl - 10h". */
typedef struct {
   RPH    rph;
   UCHAR  Category;
   UCHAR  Function;
   PVOID  ParmPacket;
   PVOID  DataPacket;
   USHORT sfn;                  /* system file number                                     */
   USHORT ParmLen;
   USHORT DataLen;
} RP_GENIOCTL;

/* READ (04h) / WRITE (08h), character device: transfer address is a locked physical address,
 * count is in bytes (PDD "READ/WRITE/WRITE WITH VERIFY - 4h, 8h, 9h"). */
typedef struct {
   RPH    rph;
   UCHAR  Media;
   ULONG  Xfer;
   USHORT Count;
   ULONG  StartSector;
   USHORT sfn;
} RP_RW;

/* OPEN (0Dh) / CLOSE (0Eh): header plus the system file number. */
typedef struct {
   RPH    rph;
   USHORT sfn;
} RP_OPENCLOSE;

#pragma pack(pop)

/* Strategy command codes (PDD "Summary of Strategy Commands"). */
#define CMD_INIT          0x00
#define CMD_READ          0x04
#define CMD_WRITE         0x08
#define CMD_OPEN          0x0D
#define CMD_CLOSE         0x0E
#define CMD_GENIOCTL      0x10
#define CMD_SHUTDOWN      0x1C
#define CMD_INITCOMPLETE  0x1F

/* Status word (PDD "Request Packets"): bit 8 done, bit 15 error, low byte = error code. */
#define STDON             0x0100
#define STERR             0x8000
#define ERROR_I24_BAD_COMMAND  0x0003   /* "Unknown Command" */
#define ERROR_I24_GEN_FAILURE  0x000C   /* "General Failure" */

/* ProcBlock interruptibility flag (PDD "ProcBlock": DH = 0 interruptible, 1 not). */
#define WAIT_INTERRUPTIBLE      0
#define WAIT_UNINTERRUPTIBLE    1

/* VerifyAccess type (PDD "VerifyAccess": DH = 0 read, 1 read/write). */
#define VERIFY_READONLY         0
#define VERIFY_READWRITE        1

/* GetDOSVar index of the global information segment (PDD "GetDOSVar"). */
#define DOSVAR_SYSINFOSEG       1

/* ---- rt_lib.asm: runtime and DevHelp services (all __cdecl, preserve SI DI BP DS) ------------------ */
extern USHORT __cdecl rt_getds(void);
extern void   __cdecl rt_setmem(PVOID p, UCHAR c, USHORT n);
extern void   __cdecl rt_movmem(PVOID dst, PVOID src, USHORT n);
extern UCHAR  __cdecl rt_tas(UCHAR *p);                  /* atomic exchange with 1: returns the old value */
extern void   __cdecl rt_atomic_inc(USHORT *p);
extern void   __cdecl rt_atomic_dec(USHORT *p);
extern void   __cdecl tcom_always(char c);               /* raw byte to COM1 (debug channel) */
extern long   __cdecl rt_imul16(int a, int b);            /* signed 16 x 16 -> 32 */
extern void   __cdecl rt_cli(void);
extern void   __cdecl rt_sti(void);
/* SMP helpers (486+ instructions): CPU identity, local interrupt state, owner-tagged locks, bounded spin wait */
extern USHORT __cdecl rt_cpu_id(void);                     /* initial local APIC id of this CPU */
extern USHORT __cdecl rt_irq_save(void);                   /* returns FLAGS, interrupts off on this CPU */
extern void   __cdecl rt_irq_restore(USHORT flags);
extern USHORT __cdecl rt_lock_try(USHORT *p, USHORT id);       /* 0 = taken, 1 = held */
extern USHORT __cdecl rt_lock_acquire(USHORT *p, USHORT id);   /* spins; 0 = taken, 1 = already held by id */
extern void   __cdecl rt_lock_release(USHORT *p);
extern USHORT __cdecl rt_wait_zero(USHORT *p, USHORT rounds);  /* 0 = *p became 0, 1 = timeout */
extern USHORT __cdecl rt_atomic_dec_nz(USHORT *p);              /* 1 = decremented, 0 = was already 0 */
extern USHORT __cdecl rt_getif(void);
extern USHORT __cdecl rt_dh_alloc_ctxhook(USHORT handler, ULONG *handle);          /* 0 = ok */
extern USHORT __cdecl rt_dh_arm_ctxhook(ULONG data, ULONG handle);                 /* 0 = armed */
extern USHORT __cdecl rt_hook_offset(void);                          /* 1 = interrupts enabled */

extern ULONG  rt_devhelp;                                 /* DevHelp entry point (far), set from INIT */
extern USHORT __cdecl rt_dh_procblock(ULONG id, ULONG timeout, USHORT flag);   /* returns the awake code */
extern USHORT __cdecl rt_dh_procrun(ULONG id, USHORT *awake);
extern USHORT __cdecl rt_dh_verify(USHORT sel, USHORT len, USHORT off, UCHAR type);   /* 0 = access ok */
extern USHORT __cdecl rt_dh_alloc_gdt(USHORT *sel, USHORT count);                    /* 0 = ok */
extern USHORT __cdecl rt_dh_phys_to_gdt(ULONG phys, USHORT len, USHORT sel);        /* 0 = ok */
extern USHORT __cdecl rt_dh_virt_to_phys(PVOID p, ULONG *phys);                      /* 0 = ok */
extern USHORT __cdecl rt_dh_getdosvar(USHORT index, USHORT member, PPVOID out);      /* 0 = ok */
extern USHORT __cdecl rt_dh_attach_dd(char *name, USHORT *table);                      /* 0 = ok; table = 6 words */
extern USHORT __cdecl rt_dh_alloc_phys(ULONG size, USHORT memtype, ULONG *phys);   /* 0 = ok; memtype 0 = above 1 MB */
extern void   __cdecl rt_save_message(void *table);                                  /* Save_Message */

/* IDC: call another driver's IDC entry: far entry point, the target's DS, far request packet. */
extern void   __cdecl rt_idc_call(ULONG entry, USHORT ds, PVOID rp);

/* resident-end markers (rt_end.asm, linked last) */
extern USHORT __cdecl rt_code_end(void);
extern USHORT __cdecl rt_data_end(void);

#endif
