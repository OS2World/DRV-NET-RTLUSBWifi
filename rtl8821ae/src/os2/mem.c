/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * mem.c - DMA memory, scratch mappings and the firmware file buffer (from qc_mem.c of the QCA6174 driver).
 *
 * DevHelp services used (IBM PDD reference): AllocGDTSelector (2Dh, INIT only), AllocPhys (18h),
 * PhysToGDTSelector (2Eh). AllocPhys memory is below 4 GB, which every supported device can reach.
 */
#include "drv.h"

#define NCHUNK      24                       /* 24 x 64 KB = 1.5 MB of mapped DMA memory (rings are 32 KB) */
#define NSCR        6                        /* scratch selectors (SCR_A .. SCR_D in drv.h)                 */
#define CHUNK_TOP   0xFFF0U                  /* usable bytes per chunk (selector limit 0FFFEh)              */

extern USHORT g_barsel[];

static USHORT g_csel[NCHUNK];               /* chunk selectors                                             */
static ULONG  g_cphys[NCHUNK];
static USHORT g_cused[NCHUNK];
static USHORT g_cmapped[NCHUNK];
static USHORT g_nchunk;
static USHORT g_scr[NSCR];

BLOB g_fwblob;

/* INIT: every GDT selector the driver will ever need, and the DMA chunks. */
int mem_init(void)
{
   static USHORT sels[32 + NCHUNK + NSCR];     /* passed by address to DevHelp: static (DS), not stack        */
   USHORT i;

   if (rt_dh_alloc_gdt(sels, (USHORT)(32 + NCHUNK + NSCR)))
      return -1;
   for (i = 0; i < 32; i++)
      g_barsel[i] = sels[i];
   for (i = 0; i < NCHUNK; i++)
      g_csel[i] = sels[32 + i];
   for (i = 0; i < NSCR; i++)
      g_scr[i] = sels[32 + NCHUNK + i];

   for (i = 0; i < NCHUNK; i++) {
      static ULONG ph;

      if (rt_dh_alloc_phys(0x10000UL, 0, &ph))
         break;
      g_cphys[i] = ph;
      g_cused[i] = 0;
      g_cmapped[i] = 0;                          /* mapped on first use (chunk_ready), at task time: a write through
                                                  * a selector mapped here hung the boot (first report)         */
   }
   g_nchunk = i;
   return g_nchunk ? 0 : -2;
}

static int chunk_ready(USHORT c)
{
   if (!g_cmapped[c])
      g_cmapped[c] = (USHORT)(rt_dh_phys_to_gdt(g_cphys[c], 0xFFFF, g_csel[c]) == 0);
   return g_cmapped[c];
}

/* A buffer whose PHYSICAL address is a multiple of align (a power of two up to 4096). */
int dma_get_al(USHORT size, USHORT align, DMA *d)
{
   USHORT c, pad, m;

   if (align < 8)
      align = 8;
   m = (USHORT)(align - 1);
   size = (USHORT)((size + 7) & ~7);
   for (c = 0; c < g_nchunk; c++) {
      ULONG ph = g_cphys[c] + g_cused[c];

      pad = (USHORT)((align - ((USHORT)ph & m)) & m);
      if ((ULONG)g_cused[c] + pad + size > CHUNK_TOP)
         continue;
      if (!chunk_ready(c))
         continue;
      g_cused[c] = (USHORT)(g_cused[c] + pad);
      d->sel  = g_csel[c];
      d->off  = g_cused[c];
      d->phys = g_cphys[c] + g_cused[c];
      g_cused[c] = (USHORT)(g_cused[c] + size);
      rt_setmem(DPTR(*d), 0, size);
      return 0;
   }
   return -1;
}

int dma_get(USHORT size, DMA *d)
{
   return dma_get_al(size, 8, d);
}

USHORT dma_free_bytes(void)
{
   USHORT c, best = 0;

   for (c = 0; c < g_nchunk; c++)
      if ((USHORT)(CHUNK_TOP - g_cused[c]) > best)
         best = (USHORT)(CHUNK_TOP - g_cused[c]);
   return best;
}

ULONG dma_big(ULONG size)
{
   static ULONG ph;

   if (rt_dh_alloc_phys(size, 0, &ph))
      return 0;
   return ph;
}

/* A larger block aligned to align (<= 4096): AllocPhys of exactly size first; when that block is not aligned,
 * it is returned and size + align is taken instead. */
ULONG dma_big_al(ULONG size, USHORT align)
{
   ULONG ph = dma_big(size);
   USHORT m = (USHORT)(align - 1);

   if (!ph || !((USHORT)ph & m))
      return ph;
   dd_dh_free_phys(ph);
   ph = dma_big(size + align);
   if (!ph)
      return 0;
   return ph + (ULONG)(USHORT)((align - ((USHORT)ph & m)) & m);
}

USHORT scr_map(USHORT slot, ULONG phys, USHORT len)
{
   if (rt_dh_phys_to_gdt(phys, len ? len : 1, g_scr[slot]))
      return 0;
   return g_scr[slot];
}

/* ---- firmware blobs ---------------------------------------------------------------------------------- */
void blob_reset(BLOB *b)
{
   /* the blocks are kept for reuse; only the length restarts */
   b->len = 0;
   b->done = 0;
}

int blob_write(BLOB *b, ULONG src, USHORT n)
{
   U32    pos;
   USHORT boff, k, ss, ds;
   ULONG  room;

   while (n) {
      pos.l = b->len;
      boff = pos.w[0];
      if (pos.w[1] >= BLOB_MAXBLK)
         return -1;                              /* file larger than 4 MB                                    */
      if (pos.w[1] >= b->nblk) {
         static ULONG ph;

         if (rt_dh_alloc_phys(0x10000UL, 0, &ph))
            return -2;
         b->blk[b->nblk++] = ph;
      }
      room = 0x10000UL - boff;
      k = (room < (ULONG)n) ? (USHORT)room : n;
      if (k == 0)
         k = n;                                  /* cannot happen (room >= 1)                                */
      ss = scr_map(SCR_B, src, k);
      ds = scr_map(SCR_A, b->blk[pos.w[1]] + boff, k);
      if (!ss || !ds)
         return -3;
      rt_movmem(MAKEP(ds, 0), MAKEP(ss, 0), k);
      b->len += k;
      src += k;
      n = (USHORT)(n - k);
   }
   return 0;
}

int blob_read(BLOB *b, ULONG off, UCHAR FAR *dst, USHORT n)
{
   U32    pos;
   USHORT k, ss;
   ULONG  room;

   if (off + n > b->len || off + n < off)
      return -1;
   while (n) {
      pos.l = off;
      room = 0x10000UL - pos.w[0];
      k = (room < (ULONG)n) ? (USHORT)room : n;
      ss = scr_map(SCR_C, b->blk[pos.w[1]] + pos.w[0], k);
      if (!ss)
         return -3;
      rt_movmem(dst, MAKEP(ss, 0), k);
      dst += k;
      off += k;
      n = (USHORT)(n - k);
   }
   return 0;
}

ULONG blob_rd32(BLOB *b, ULONG off)
{
   static U32 v;

   v.l = 0;
   blob_read(b, off, (UCHAR FAR *)&v, 4);
   return v.l;
}
