/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * pci.c - PCI discovery, BAR0 mapping, MMIO access, the interrupt entry and the context hook.
 *
 * From qc_pci.c of the QCA6174 driver (github.com/psmedley/qca6174), made chip-independent: the back end says which
 * functions it drives (hw_pci_match), which BAR holds its registers (g_bar_cfg: configuration offset 10h for BAR0,
 * 18h for BAR2) and how big it is (g_bar_len), and handles its own interrupt (hw_irq at interrupt time, hw_hook at
 * task time from the context hook). The RTL8821AE has an I/O BAR0 and its memory registers in BAR2 (Linux rtlwifi:
 * bar_id 2), so this copy of the file reads the BAR the back end names; "BAR0" below means that register window.
 *
 * PCI configuration space is reached with configuration mechanism #1 (ports 0CF8h/0CFCh, PCI Local Bus
 * Specification 2.x section 3.2.2.3.2). The OS/2 way is the OEMHLP$ PCI IOCtl; see docs/ARCHITECTURE.md.
 * BAR0 is mapped through GDT selectors of 64 KB each (PhysToGDTSelector); every register access is one
 * 32-bit bus cycle (dd_rd32 / dd_wr32 in lib.asm).
 */
#include "drv.h"

#define PCI_ADDR_PORT   0x0CF8
#define PCI_DATA_PORT   0x0CFC

#define PCI_CMD_MEM     0x0002
#define PCI_CMD_MASTER  0x0004
#define PCI_CMD_INTXDIS 0x0400

USHORT g_pci_bdf = 0xFFFF;
USHORT g_pci_ven, g_pci_dev, g_pci_subven, g_pci_subdev;
UCHAR  g_pci_rev, g_pci_irq;
ULONG  g_bar0;                              /* physical address of the register BAR (g_bar_cfg)             */
UCHAR  g_bar_cfg = 0x10;                    /* configuration offset of that BAR: set by hw_pci_match        */
ULONG  g_bar_len = 0x00010000UL;            /* the back end may set more (up to 2 MB)                       */

#define BARSEL_MAX 32
USHORT g_barsel[BARSEL_MAX];               /* allocated in mem_init()                                      */
USHORT g_nbarsel;                          /* windows mapped                                               */

USHORT g_irq_hooked, g_irq_count, g_irq_spurious;

/* ---- configuration mechanism #1 -------------------------------------------------------------------- */
static ULONG cfg_addr(USHORT bdf, UCHAR reg)
{
   U32 a;

   a.w[1] = (USHORT)(0x8000 | (bdf >> 8));                 /* enable bit 31, bus number in bits 23..16     */
   a.w[0] = (USHORT)(((bdf & 0xFF) << 8) | (reg & 0xFC));   /* device 15..11, function 10..8, register     */
   return a.l;
}

static ULONG cfg_rd(USHORT bdf, UCHAR reg)
{
   USHORT fl;
   ULONG  v;

   fl = rt_irq_save();
   dd_outpd(PCI_ADDR_PORT, cfg_addr(bdf, reg));
   v = dd_inpd(PCI_DATA_PORT);
   rt_irq_restore(fl);
   return v;
}

static void cfg_wr(USHORT bdf, UCHAR reg, ULONG v)
{
   USHORT fl;

   fl = rt_irq_save();
   dd_outpd(PCI_ADDR_PORT, cfg_addr(bdf, reg));
   dd_outpd(PCI_DATA_PORT, v);
   rt_irq_restore(fl);
}

ULONG pci_cfg_rd32(UCHAR reg)            { return cfg_rd(g_pci_bdf, reg); }
void  pci_cfg_wr32(UCHAR reg, ULONG v)   { cfg_wr(g_pci_bdf, reg, v); }

USHORT pci_cfg_rd16(UCHAR reg)
{
   U32 v;

   v.l = cfg_rd(g_pci_bdf, reg);
   return (reg & 2) ? v.w[1] : v.w[0];
}

void pci_cfg_wr16(UCHAR reg, USHORT val)
{
   U32 v;

   v.l = cfg_rd(g_pci_bdf, reg);
   if (reg & 2) v.w[1] = val; else v.w[0] = val;
   cfg_wr(g_pci_bdf, reg, v.l);
}

/* The first function the back end accepts. Buses 0..255, devices 0..31, functions 0..7 (functions 1..7 only
 * when function 0 is a multi-function device). */
int pci_find(void)
{
   USHORT bus, dev, fn, bdf;
   U32    id, r, sub;

   for (bus = 0; bus < 256; bus++)
      for (dev = 0; dev < 32; dev++)
         for (fn = 0; fn < 8; fn++) {
            bdf = (USHORT)((bus << 8) | (dev << 3) | fn);
            id.l = cfg_rd(bdf, 0x00);
            if (id.w[0] == 0xFFFF) {
               if (fn == 0) break;               /* no device here                                         */
               continue;
            }
            sub.l = cfg_rd(bdf, 0x2C);
            if (hw_pci_match(id.w[0], id.w[1], sub.w[0], sub.w[1])) {
               g_pci_bdf = bdf;
               g_pci_ven = id.w[0];
               g_pci_dev = id.w[1];
               r.l = cfg_rd(bdf, 0x08);
               g_pci_rev = r.b[0];
               g_pci_subven = sub.w[0];
               g_pci_subdev = sub.w[1];
               r.l = cfg_rd(bdf, 0x3C);
               g_pci_irq = r.b[0];
               r.l = cfg_rd(bdf, g_bar_cfg);
               g_bar0 = r.l & 0xFFFFFFF0UL;
               if (r.b[0] & 0x01) {
                  dd_err("the register BAR is an I/O BAR", r.l);
                  g_bar0 = 0;                    /* the back end named the wrong BAR                       */
               } else if ((r.b[0] & 0x06) == 0x04 && cfg_rd(bdf, (UCHAR)(g_bar_cfg + 4)) != 0UL) {
                  dd_err("the register BAR is above 4 GB, not reachable", cfg_rd(bdf, (UCHAR)(g_bar_cfg + 4)));
                  g_bar0 = 0;                    /* 64-bit BAR placed above 4 GB by the firmware           */
               }
               return 0;
            }
            if (fn == 0) {
               r.l = cfg_rd(bdf, 0x0C);
               if (!(r.b[2] & 0x80)) break;      /* header type bit 7 clear: single-function device         */
            }
         }
   return -1;
}

/* Memory decode and bus mastering on, INTx allowed (the INTx-disable bit cleared: the iwlwifi drivers do this
 * as a hardware-bug workaround; rtlwifi leaves it as the BIOS set it, and an RTL8821AE needs INTx); ASPM L0s/L1 (PCIe Link Control bits 1..0) off. */
int pci_enable(void)
{
   USHORT cmd, st;
   UCHAR  cap;
   USHORT guard = 0;

   cmd = pci_cfg_rd16(0x04);
   cmd |= PCI_CMD_MEM | PCI_CMD_MASTER;
   cmd &= ~PCI_CMD_INTXDIS;
   pci_cfg_wr16(0x04, cmd);

   st = pci_cfg_rd16(0x06);
   if (!(st & 0x0010))                       /* no capability list                                         */
      return 0;
   cap = (UCHAR)(pci_cfg_rd16(0x34) & 0xFC);
   while (cap && guard++ < 48) {
      U32 c;

      c.l = pci_cfg_rd32(cap);
      if (c.b[0] == 0x10) {                  /* PCI Express capability: Link Control at +10h               */
         USHORT lc = pci_cfg_rd16((UCHAR)(cap + 0x10));

         pci_cfg_wr16((UCHAR)(cap + 0x10), (USHORT)(lc & ~0x0003));
         break;
      }
      cap = (UCHAR)(c.b[1] & 0xFC);
   }
   return 0;
}

/* ---- BAR0 windows ---------------------------------------------------------------------------------- */
int bar_map(void)
{
   USHORT i, n;
   ULONG  ph;

   if (g_nbarsel)
      return 0;
   if (!g_bar0)
      return -1;
   n = (USHORT)(HI16(g_bar_len));            /* number of 64 KB windows                                    */
   if (n == 0)
      n = 1;
   if (n > BARSEL_MAX) n = BARSEL_MAX;
   ph = g_bar0;
   for (i = 0; i < n; i++) {
      if (rt_dh_phys_to_gdt(ph, 0xFFFF, g_barsel[i]))
         return -2;
      ph += 0x10000UL;
   }
   g_nbarsel = n;
   return 0;
}

ULONG mmio_rd(ULONG off)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return 0xFFFFFFFFUL;
   return dd_rd32(g_barsel[a.w[1]], a.w[0]);
}

void mmio_wr(ULONG off, ULONG v)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return;
   dd_wr32(g_barsel[a.w[1]], a.w[0], v);
}

UCHAR mmio_rd8(ULONG off)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return 0xFF;
   return dd_rd8(g_barsel[a.w[1]], a.w[0]);
}

USHORT mmio_rd16(ULONG off)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return 0xFFFF;
   return dd_rd16(g_barsel[a.w[1]], a.w[0]);
}

void mmio_wr16(ULONG off, USHORT v)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return;
   dd_wr16(g_barsel[a.w[1]], a.w[0], v);
}

void mmio_wr8(ULONG off, UCHAR v)
{
   U32 a;

   a.l = off;
   if (a.w[1] >= g_nbarsel)
      return;
   dd_wr8(g_barsel[a.w[1]], a.w[0], v);
}

/* ---- context hook: task-time servicing armed by the interrupt handler ---------------------------- */
static ULONG  g_hook;
static USHORT g_hook_ok;
USHORT        g_hook_runs, g_hook_armfail, g_lat_max, g_lat_n;
ULONG         g_lat_sum, g_busy_tot, g_busy_win, g_win_t;
USHORT        g_busy_max, g_in_hook;     /* service time per second (ms), worst second; set while the hook services */
static ULONG  g_irq_t;

int hook_init(void)
{
   if (g_hook_ok)
      return 0;
   if (rt_dh_alloc_ctxhook(rt_hook_offset(), &g_hook) != 0)
      return -1;
   g_hook_ok = 1;
   return 0;
}

void hook_arm(void)
{
   if (g_hook_ok && rt_dh_arm_ctxhook(0UL, g_hook) != 0)
      g_hook_armfail++;                      /* already armed: it will run anyway                          */
}

void __cdecl dd_hook_c(void)
{
   USHORT d = (USHORT)(dd_ms() - g_irq_t);          /* interrupt -> task-time servicing, in ms              */
   ULONG  t0, t1;

   if (d < 1000) {
      if (d > g_lat_max) g_lat_max = d;
      g_lat_sum += d;
      g_lat_n++;
   }
   g_hook_runs++;
   t0 = dd_ms();
   g_in_hook = 1;                                   /* frames queued now are delivered below: no need to re-arm */
   hw_hook();
   g_in_hook = 0;
   ndis_hook_deliver();
   t1 = dd_ms();
   g_busy_tot += t1 - t0;
   g_busy_win += t1 - t0;
   if ((long)(t1 - g_win_t) >= 1000L) {
      if (g_busy_win > g_busy_max && g_busy_win < 1000UL * 2)
         g_busy_max = (USHORT)g_busy_win;
      g_busy_win = 0;
      g_win_t = t1;
   }
}

/* Interrupt time. Returns 1 when the interrupt was ours (EOI issued), 0 otherwise (shared line). */
USHORT __cdecl dd_irq_c(void)
{
   if (!g_nbarsel || !hw_irq()) {
      g_irq_spurious++;
      return 0;
   }
   g_irq_count++;
   g_irq_t = dd_ms();
   dd_dh_eoi(g_pci_irq);
   hook_arm();
   return 1;
}

int irq_hook(void)
{
   if (g_irq_hooked)
      return 0;
   if (g_pci_irq == 0 || g_pci_irq == 0xFF)
      return -1;
   if (dd_dh_setirq(dd_irq_offset(), g_pci_irq, 1))
      return -2;
   g_irq_hooked = 1;
   return 0;
}

void irq_unhook(void)
{
   if (g_irq_hooked) {
      dd_dh_unsetirq(g_pci_irq);
      g_irq_hooked = 0;
   }
}
