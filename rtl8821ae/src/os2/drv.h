/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * drv.h - OS/2 services shared by every module of the driver: trace, clock, PCI and MMIO, DMA memory,
 * firmware blobs, the NDIS MAC and the GenMAC device.
 *
 * The OS/2 side (strategy routine, NDIS MAC, GenMAC OID device, DevHelp glue) comes from the QCA6174 driver
 * (github.com/psmedley/qca6174), which adapted it from the OS2World RTL88EU driver (GPL-2.0-only).
 *
 * Compiler constraints (16-bit Open Watcom, no C runtime): no 32-bit multiply, divide or variable shift;
 * SS != DS (never pass the address of a stack variable to code that dereferences it DS-relative).
 */
#ifndef DRV_H
#define DRV_H

#include "os2dd.h"

#define DRV_NAME        "RTL8821AE"
#define DRV_VERSION     "0.0.1"
#define DRV_BUILD       "dev"
#define DRV_NIF         "RTL8821AE_NIF"         /* PROTOCOL.INI section, NDIS module name                     */
#define DRV_NIF_LC      "rtl8821ae_nif"         /* the same in lower case (PROTOCOL.INI parser)                */
#define DRV_NIF_FILE    "RTL8821AE.NIF"
#define DRV_DESC        "Realtek RTL8821AE PCIe WLAN (OS/2 native)"   /* NDIS and GenMAC adapter description  */

#ifndef TRACE
#define TRACE 1                                 /* 1: progress markers and errors on COM1                      */
#endif

/* ---- little helpers ---------------------------------------------------------------------------- */
typedef union { ULONG l; USHORT w[2]; UCHAR b[4]; } U32;
#define LO16(x)         ((USHORT)(x))
#define HI16(x)         (((U32 FAR *)&(x))->w[1])          /* x must be an lvalue */

ULONG  u32_div(ULONG n, ULONG d);               /* n / d without the C runtime (dbg.c)                         */
void   put16(UCHAR FAR *p, USHORT v);           /* little-endian stores and loads (dbg.c)                      */
void   put32(UCHAR FAR *p, ULONG v);
USHORT get16(UCHAR FAR *p);
ULONG  get32(UCHAR FAR *p);
void   fmemcpy(UCHAR FAR *d, UCHAR FAR *s, USHORT n);
void   fmemset(UCHAR FAR *d, UCHAR c, USHORT n);
int    fmemeq(UCHAR FAR *a, UCHAR FAR *b, USHORT n);

/* ---- trace (dbg.c) ----------------------------------------------------------------------------- */
#if TRACE
#define tcom(c) tcom_always(c)
#else
#define tcom(c) ((void)(c))
#endif
void   tstr(const char *s);                     /* near string in DGROUP                                       */
void   th8(UCHAR b);
void   th16(USHORT w);
void   th32(ULONG v);
void   tnl(void);
void   dd_err(const char *what, ULONG v);       /* "RTL8821AE: <what> <v>" on COM1, kept for the status text      */
extern ULONG g_last_err;
extern char *g_last_err_txt;

void   dd_step(USHORT n);                       /* boot diagnostics: n beeps with /B (strat.c)                  */

/* ---- time (strat.c) ---------------------------------------------------------------------------- */
ULONG  dd_ms(void);                             /* ms since boot (global information segment)                   */
void   dd_msleep(USHORT ms);                    /* task time only: ProcBlock with a timeout                      */
#define dd_udelay(us) dd_iodelay(us)

/* ---- PCI and MMIO (pci.c) ---------------------------------------------------------------------- */
extern USHORT g_pci_bdf;                        /* bus << 8 | dev << 3 | fn, 0xFFFF = not found                 */
extern USHORT g_pci_ven, g_pci_dev, g_pci_subven, g_pci_subdev;
extern UCHAR  g_pci_rev, g_pci_irq;
extern ULONG  g_bar0;                           /* physical address of BAR0                                    */
extern ULONG  g_bar_len;                        /* bytes of BAR0 that are mapped (set by the chip code)        */
extern UCHAR  g_bar_cfg;                        /* configuration offset of the register BAR (10h = BAR0)       */
int    pci_find(void);                          /* 0 = a supported device was found (hw_pci_match)             */
ULONG  pci_cfg_rd32(UCHAR reg);
void   pci_cfg_wr32(UCHAR reg, ULONG v);
USHORT pci_cfg_rd16(UCHAR reg);
void   pci_cfg_wr16(UCHAR reg, USHORT v);
int    pci_enable(void);                        /* memory space + bus master, INTx on, ASPM off                 */
int    bar_map(void);                           /* GDT selectors over BAR0 (task time)                          */
ULONG  mmio_rd(ULONG off);                      /* one 32-bit MMIO read of BAR0 + off                           */
void   mmio_wr(ULONG off, ULONG v);
void   mmio_wr8(ULONG off, UCHAR v);            /* one 8-bit MMIO write                                         */
UCHAR  mmio_rd8(ULONG off);                     /* one 8-bit MMIO read                                          */
USHORT mmio_rd16(ULONG off);                    /* one 16-bit MMIO read                                         */
void   mmio_wr16(ULONG off, USHORT v);          /* one 16-bit MMIO write                                        */
int    irq_hook(void);
void   irq_unhook(void);
void   hook_arm(void);
int    hook_init(void);
extern USHORT g_irq_hooked, g_irq_count, g_irq_spurious, g_nbarsel;

/* ---- DMA memory (mem.c) ------------------------------------------------------------------------ *
 * Physically contiguous 64 KB chunks from AllocPhys, each with a permanent GDT selector (allocated at
 * INIT). Small buffers are carved out of them and never freed. A DMA buffer never crosses a chunk.    */
typedef struct {
   USHORT sel;                                  /* GDT selector of the chunk                                    */
   USHORT off;                                  /* offset of the buffer in the chunk                            */
   ULONG  phys;                                 /* physical (= device) address of the buffer                    */
} DMA;
#define DPTR(d)         ((UCHAR FAR *)MAKEP((d).sel, (d).off))
int    mem_init(void);                          /* INIT: selectors + chunks                                     */
int    dma_get(USHORT size, DMA *d);            /* 8-byte aligned, zeroed; 0 = ok                               */
int    dma_get_al(USHORT size, USHORT align, DMA *d);   /* aligned to align (power of 2, <= 4096) in PHYSICAL memory */
USHORT dma_free_bytes(void);
ULONG  dma_big(ULONG size);                     /* a larger contiguous block (no mapping); 0 = fail             */
ULONG  dma_big_al(ULONG size, USHORT align);    /* the same, aligned (power of 2, <= 4096)                      */
USHORT scr_map(USHORT slot, ULONG phys, USHORT len);   /* scratch selector over phys, returns the selector       */
#define SCR_A  0                                /* request path (nic_lock): firmware blobs, status text          */
#define SCR_B  1
#define SCR_C  2
#define SCR_RX 3                                /* receive buffers (service path, one CPU at a time)             */
#define SCR_TX 4                                /* NDIS TransmitChain physical blocks (under g_txsel_lock)       */
#define SCR_D  5                                /* chip code, request path (firmware copies)                     */

/* Firmware files, written to the device at run time ("copy /b file \DEV\WRND32$"). Kept as a list of
 * 64 KB physical blocks (they need not be contiguous: only the driver reads them). */
#define BLOB_MAXBLK  64                         /* 4 MB                                                         */
typedef struct {
   ULONG  len;
   USHORT nblk;
   USHORT done;                                 /* complete (the writer closed the device)                      */
   ULONG  blk[BLOB_MAXBLK];
} BLOB;
extern BLOB g_fwblob;
int    blob_write(BLOB *b, ULONG src_phys, USHORT n);   /* append n bytes from physical src; 0 = ok           */
int    blob_read(BLOB *b, ULONG off, UCHAR FAR *dst, USHORT n);   /* 0 = ok                                 */
ULONG  blob_rd32(BLOB *b, ULONG off);
void   blob_reset(BLOB *b);

/* ---- NDIS and GenMAC (ndis.c, genmac.c) -------------------------------------------------------- */
extern USHORT g_lannum;
extern UCHAR  g_mymac[6];                       /* the address in use (firmware/NVM or PROTOCOL.INI)            */
extern UCHAR  g_cfgmac;                         /* NETADDRESS was given in PROTOCOL.INI                         */
void   ndis_find_lan(void);
int    ndis_set_mac(UCHAR *m);
UCHAR FAR *ndis_rx_get(USHORT *handle);         /* a free NDIS receive buffer (1520 bytes) or NULL              */
void   ndis_rx_put(USHORT h, USHORT len);       /* queue it for the protocol (delivered from the context hook)  */
void   ndis_hook_deliver(void);
USHORT ndis_init(void);
void   dd_genmac_ioctl(RP_GENIOCTL FAR *rp);
void   dd_shutdown(void);
void   nic_lock(void);
void   nic_unlock(void);

/* ---- device writes: firmware upload and debug commands (dbg.c) -------------------------------- */
void   dd_dev_open(USHORT sfn);
void   dd_dev_close(USHORT sfn);
void   dd_dev_write(USHORT sfn, ULONG phys, USHORT n);
USHORT dd_dev_read(USHORT sfn, ULONG phys, USHORT n);

/* ---- interrupt / context hook glue (pci.c) ----------------------------------------------------- */
USHORT __cdecl dd_irq_c(void);
void   __cdecl dd_hook_c(void);

#include "hw.h"
#include "wlan.h"

#endif
