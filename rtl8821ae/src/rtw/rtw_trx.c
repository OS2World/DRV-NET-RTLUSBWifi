/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtw_trx.c - the PCIe data path of the RTL8821AE back end: DMA descriptor rings, transmit, receive, the interrupt
 * and the service path.
 *
 * The ring protocol is that of Linux rtlwifi for the chips without the "new TRX flow" (pci.c: _rtl_pci_init_tx_ring,
 * _rtl_pci_init_rx_ring, _rtl_pci_rx_interrupt, rtl_pci_tx; rtl8821ae/trx.c: rtl8821ae_tx_fill_desc,
 * rtl8821ae_query_rx_desc, rtl8821ae_tx_polling; Copyright(c) 2009-2012 Realtek Corporation, GPL-2.0). The TX
 * descriptor contents and the RX status parsing are those of the USB driver (rtl8821au/src/rtl8821_tx.c, rtl_rx.c),
 * which use the same Jaguar descriptor.
 *
 * Contexts (docs/ARCHITECTURE.md of this driver):
 *   hw_irq      interrupt time: is the (shared) line ours? Then mask the chip (HIMR = 0) until the hook ran.
 *   hw_service  task time or context hook, never blocks: acknowledge, drain the receive ring, firmware reports.
 *   rtw_tx_frame any context (NDIS TransmitChain can come at interrupt time): a ring lock with interrupts off.
 * A transmit entry is free again when the chip has cleared its OWN bit; the chip works through a ring in order, so
 * the entry at the head is the oldest one in use, and a set OWN bit there means the ring is full.
 */
#include "drv.h"
#include "rtw.h"

#define RX_N        32                       /* receive ring entries                                          */
#define RX_BUF      4096                     /* buffer: PHY status + shift + up to a 3839-byte A-MSDU         */
#define TXQ_BE      0
#define TXQ_MG      1
#define TX_N_BE     32
#define TX_N_MG     8
#define TX_BUF_BE   1664                     /* 802.11 + QoS + CCMP headers (34) + 1536 bytes of LLC payload  */
#define TX_BUF_MG   512                      /* management frames (association request ~180 bytes)            */

USHORT g_trx_on;
USHORT g_rx_frames, g_rx_crc, g_rx_c2h, g_rx_rdu, g_rx_big, g_rx_fovw;
USHORT g_tx_ok[2], g_tx_full[2], g_tx_fovw;
USHORT g_svc_runs, g_isr_last, g_c2h_reg;
ULONG  g_hisr_last;
UCHAR  g_rx_rate;

static DMA    g_rxd;                         /* RX descriptor ring                                            */
static DMA    g_rxb[RX_N];                   /* its buffers                                                   */
static USHORT g_rxi;                         /* next entry the chip gives back                                */

typedef struct {
   DMA    ring;
   USHORT n, size, head, lock, bit;
   UCHAR  qsel;
} txq_t;
static txq_t  g_txq[2];
static DMA    g_txb_be[TX_N_BE];
static DMA    g_txb_mg[TX_N_MG];
static DMA    g_dummy;                       /* 2-entry ring for the queues the driver does not use            */

static USHORT g_svc_lock, g_svc_again, g_irq_masked;
static ULONG  g_imr = IMR_ROK | IMR_RDU | IMR_C2HCMD | IMR_BEDOK | IMR_MGNTDOK;
static ULONG  g_imre = IMRE_RXFOVW | IMRE_TXFOVW;

static DMA *txbuf(USHORT q, USHORT i)
{
   return q == TXQ_BE ? &g_txb_be[i] : &g_txb_mg[i];
}

/* ---- allocation and ring set-up ------------------------------------------------------------------------- */
static int ring_alloc(USHORT q, USHORT n, USHORT bsize, UCHAR qsel, USHORT bit)
{
   USHORT i;
   txq_t *t = &g_txq[q];

   if (dma_get_al((USHORT)(n * TXD_SIZE), 256, &t->ring))
      return -1;
   for (i = 0; i < n; i++)
      if (dma_get_al(bsize, 64, txbuf(q, i)))
         return -1;
   t->n = n;
   t->size = bsize;
   t->qsel = qsel;
   t->bit = bit;
   return 0;
}

int rtw_trx_alloc(void)
{
   USHORT i;
   static USHORT done;

   if (done)
      return 0;
   if (dma_get_al(RX_N * RXD_SIZE, 256, &g_rxd))
      return -1;
   for (i = 0; i < RX_N; i++)
      if (dma_get_al(RX_BUF, 64, &g_rxb[i]))
         return -2;
   if (ring_alloc(TXQ_BE, TX_N_BE, TX_BUF_BE, QSLT_BE, TXQ_BIT_BE))
      return -3;
   if (ring_alloc(TXQ_MG, TX_N_MG, TX_BUF_MG, QSLT_MGNT, TXQ_BIT_MGNT))
      return -4;
   if (dma_get_al(2 * TXD_SIZE, 256, &g_dummy))
      return -5;
   done = 1;
   return 0;
}

static void rx_arm(USHORT i)
{
   UCHAR FAR *d = DPTR(g_rxd) + i * RXD_SIZE;
   ULONG w0 = TXD_OWN | (ULONG)RX_BUF;

   if (i == RX_N - 1)
      w0 |= 0x40000000UL;                    /* EOR: the chip goes back to the first entry                     */
   fmemset(d + 4, 0, RXD_SIZE - 4);
   put32(d + 24, g_rxb[i].phys);
   put32(d, w0);                             /* OWN last                                                       */
}

/* Rings back to their first entry (the chip starts there after the DESA registers are written). */
void rtw_trx_init(void)
{
   USHORT i, q;
   UCHAR FAR *d;

   for (i = 0; i < RX_N; i++)
      rx_arm(i);
   g_rxi = 0;
   for (q = 0; q < 2; q++) {
      txq_t *t = &g_txq[q];

      d = DPTR(t->ring);
      fmemset(d, 0, (USHORT)(t->n * TXD_SIZE));
      for (i = 0; i < t->n; i++)            /* dw12: the next entry (circular)                               */
         put32(d + i * TXD_SIZE + 48, t->ring.phys + (ULONG)(USHORT)(((i + 1) % t->n) * TXD_SIZE));
      t->head = 0;
      t->lock = 0;
   }
   d = DPTR(g_dummy);
   fmemset(d, 0, 2 * TXD_SIZE);
   put32(d + 48, g_dummy.phys + TXD_SIZE);
   put32(d + TXD_SIZE + 48, g_dummy.phys);
}

/* _rtl8821ae_init_trx_desc_hw_address */
void rtw_trx_desa(void)
{
   rtw_w32(REG_BCNQ_DESA, g_dummy.phys);
   rtw_w32(REG_MGQ_DESA, g_txq[TXQ_MG].ring.phys);
   rtw_w32(REG_VOQ_DESA, g_dummy.phys);
   rtw_w32(REG_VIQ_DESA, g_dummy.phys);
   rtw_w32(REG_BEQ_DESA, g_txq[TXQ_BE].ring.phys);
   rtw_w32(REG_BKQ_DESA, g_dummy.phys);
   rtw_w32(REG_HQ_DESA, g_dummy.phys);
   rtw_w32(REG_RX_DESA, g_rxd.phys);
}

/* ---- interrupts ------------------------------------------------------------------------------------------ */
void rtw_irq_enable(void)
{
   rtw_w32(REG_HISR, 0xFFFFFFFFUL);
   rtw_w32(REG_HISRE, 0xFFFFFFFFUL);
   g_irq_masked = 0;
   rtw_w32(REG_HIMR, g_imr);
   rtw_w32(REG_HIMRE, g_imre);
}

void rtw_irq_disable(void)
{
   rtw_w32(REG_HIMR, 0UL);
   rtw_w32(REG_HIMRE, 0UL);
}

/* Interrupt time: is the (shared) line ours? Then mask the chip until the context hook has serviced it. */
USHORT hw_irq(void)
{
   ULONG r, e;

   if (!g_trx_on)
      return 0;
   r = rtw_r32(REG_HISR);
   if (r == 0xFFFFFFFFUL)
      return 0;                              /* the card is gone (or powered down)                             */
   e = rtw_r32(REG_HISRE);
   if (!(r & g_imr) && !(e & g_imre))
      return 0;
   rtw_w32(REG_HIMR, 0UL);
   rtw_w32(REG_HIMRE, 0UL);
   g_irq_masked = 1;
   return 1;
}

/* ---- transmit ----------------------------------------------------------------------------------------------
 * One frame: the 802.11 header h, an optional 8-byte CCMP header iv, the body b; copied into the entry's buffer.
 * 0 = queued, -2 = the ring is full (or this CPU is already in here), -1 = too long. */
int rtw_tx_frame(UCHAR qsel, UCHAR FAR *h, USHORT hl, UCHAR FAR *iv, UCHAR FAR *b, USHORT bl, UCHAR rate, USHORT flags)
{
   USHORT q = (qsel == QSLT_MGNT) ? TXQ_MG : TXQ_BE;
   txq_t *t = &g_txq[q];
   USHORT fl, len, i;
   UCHAR FAR *d, FAR *p;
   DMA   *bf;
   ULONG  w;

   if (!g_trx_on)
      return -1;
   len = (USHORT)(hl + (iv ? 8 : 0) + bl);
   if (len > t->size)
      return -1;
   fl = rt_irq_save();
   if (rt_lock_acquire(&t->lock, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);                    /* re-entered on this CPU                                         */
      return -2;
   }
   i = t->head;
   d = DPTR(t->ring) + i * TXD_SIZE;
   if (get32(d) & TXD_OWN) {                 /* the oldest entry has not been sent yet: full                    */
      g_tx_full[q]++;
      rt_lock_release(&t->lock);
      rt_irq_restore(fl);
      return -2;
   }
   bf = txbuf(q, i);
   p = DPTR(*bf);
   fmemcpy(p, h, hl);
   if (iv)
      fmemcpy(p + hl, iv, 8);
   fmemcpy(p + hl + (iv ? 8 : 0), b, bl);

   fmemset(d, 0, 48);                        /* dw0..dw11 (dw12 = next entry stays)                           */
   w = (ULONG)(q == TXQ_MG ? 1 : 0) | ((ULONG)qsel << 8) | ((ULONG)(rate < RATE_6M ? 8 : 7) << 16);   /* MACID, QSEL, RATE_ID B / G */
   if (flags & TXF_SEC)
      w |= 0x00C00000UL;                     /* SEC_TYPE = AES (the chip encrypts with the CAM entry of A1)     */
   put32(d + 4, w);
   if (flags & TXF_RPT)
      put32(d + 8, 0x00080000UL);            /* SPE_RPT: TX report (C2H) after sending                         */
   put32(d + 12, 0x00000500UL);              /* USE_RATE | DISABLE_FB                                          */
   put32(d + 16, (ULONG)(rate & 0x7F) | 0x00000800UL | 0x00020000UL | 0x00300000UL);   /* rate, fallback limit, retry limit 12 */
   put32(d + 28, (ULONG)len);                /* TX_BUFFER_SIZE                                                 */
   put32(d + 36, (ULONG)(USHORT)(get16(h + 22) >> 4) << 12);   /* SEQ: the header's (HWSEQ_EN off, as rtlwifi does) */
   put32(d + 40, bf->phys);                  /* TX_BUFFER_ADDRESS                                              */
   w = (ULONG)len | ((ULONG)TXD_HDR << 16) | 0x0C000000UL;   /* PKT_SIZE, OFFSET, LS | FS                     */
   if (flags & TXF_BCAST)
      w |= 0x01000000UL;
   put32(d, w);
   put32(d, w | TXD_OWN);                    /* the chip may take it now                                       */
   t->head = (USHORT)((i + 1) % t->n);
   rtw_w16(REG_PCIE_CTRL_REG, t->bit);       /* doorbell (rtl8821ae_tx_polling)                                */
   g_tx_ok[q]++;
   rt_lock_release(&t->lock);
   rt_irq_restore(fl);
   return 0;
}

/* ---- receive ------------------------------------------------------------------------------------------------
 * Signal strength (dBm) from the PHY status (drvinfo) in front of the frame: CCK from the AGC report, OFDM / HT from
 * the path-A power byte (rtl8821au/src/rtl_rx.c rx_rssi, the stock driver's formulas). 0 = no PHY status. */
static short rx_rssi(UCHAR FAR *st, UCHAR FAR *ph, USHORT drv)
{
   UCHAR rate = (UCHAR)(st[12] & 0x3F);
   int   pw;
   UCHAR lna, vga;

   if (!(st[3] & 0x04) || drv < 20)
      return 0;
   if (rate <= 3) {
      lna = (UCHAR)((ph[5] & 0xE0) >> 5);
      vga = (UCHAR)(ph[5] & 0x1F);
      switch (lna) {
         case 7:  pw = (vga <= 27) ? (-100 + 2 * (27 - vga)) : -100; break;
         case 6:  pw = -48 + 2 * (2 - vga); break;
         case 5:  pw = -42 + 2 * (7 - vga); break;
         case 4:  pw = -36 + 2 * (7 - vga); break;
         case 3:  pw = -24 + 2 * (7 - vga); break;
         case 2:  pw = -12 + 2 * (5 - vga); break;
         case 1:  pw = 8 - 2 * vga; break;
         default: pw = 14 - 2 * vga; break;
      }
      pw += 6;
   } else
      pw = (int)(ph[0] & 0x7F) - 110;
   if (pw > 0) pw = 0;
   if (pw < -110) pw = -110;
   return (short)pw;
}

/* RX status (24 bytes, rtl8821ae/trx.h):
 *   dw0  PKT_LEN[13:0] CRC32[14] ICVERR[15] DRV_INFO_SIZE[19:16] (x 8 bytes) SHIFT[25:24] PHYST[26] SWDEC[27] OWN[31]
 *   dw1  PAGGR[15]     dw2  C2H[28] (RPT_SEL)     dw3  RX_RATE[6:0]     dw5  TSFL */
static void rx_service(void)
{
   USHORT n, pl, drv, sh, fl;
   UCHAR FAR *d, FAR *buf;
   ULONG  w0;
   short  rssi;

   for (n = 0; n < RX_N; n++) {
      d = DPTR(g_rxd) + g_rxi * RXD_SIZE;
      w0 = get32(d);
      if (w0 & TXD_OWN)
         break;
      pl  = (USHORT)(LO16(w0) & 0x3FFF);
      drv = (USHORT)((d[2] & 0x0F) * 8);
      sh  = (USHORT)(d[3] & 0x03);
      buf = DPTR(g_rxb[g_rxi]);
      if ((ULONG)pl + drv + sh > RX_BUF || pl == 0)
         g_rx_big++;
      else if (d[11] & 0x10) {               /* a firmware report (C2H packet), not a frame                    */
         g_rx_c2h++;
         rtw_c2h(buf + drv + sh, pl);
      } else if (d[1] & 0xC0)                /* CRC32 / ICV error                                              */
         g_rx_crc++;
      else {
         UCHAR FAR *f = buf + drv + sh;

         g_rx_frames++;
         rssi = rx_rssi(d, buf, drv);
         fl = 0;
         if ((f[1] & 0x40) && !(d[3] & 0x08))
            fl |= RXF_DECRYPTED;             /* protected and decrypted by the chip (CCMP header + MIC stay)   */
         if ((f[0] & 0x0C) == 0x08 && pl >= 24)
            g_rx_rate = (UCHAR)(d[12] & 0x7F);
         sta_rx_frame(f, pl, g_curch, rssi, fl, get32(d + 20));
      }
      rx_arm(g_rxi);
      g_rxi = (USHORT)((g_rxi + 1) % RX_N);
   }
}

/* C2H through the event registers (IMR_C2HCMD; c2h_evt_read_88xx): 0x1AF = 0xFF when the firmware has written an
 * event: 0x1A0 = id, 0x1A1 = sequence, payload from 0x1A2; 0x1AF = 0 gives the registers back. */
static void c2h_reg(void)
{
   static UCHAR c[16];
   USHORT i;

   if (rtw_r8(REG_C2HEVT_CLEAR) != 0xFF)
      return;
   for (i = 0; i < 16; i++)
      c[i] = rtw_r8(REG_C2HEVT_MSG + i);
   rtw_w8(REG_C2HEVT_CLEAR, 0x00);
   g_c2h_reg++;
   rtw_c2h((UCHAR FAR *)c, 16);
}

/* A firmware report: [0] = id, [1] = sequence, then the data. 0x11 = IQ calibration finished. */
void rtw_c2h(UCHAR FAR *c, USHORT n)
{
   if (n >= 2 && c[0] == 0x11) {
      g_iqk_c2h = (UCHAR)(n >= 3 ? c[2] : 0xFF);
      g_iqk_done = 1;
   }
}

static void service_once(void)
{
   ULONG r, e;

   r = rtw_r32(REG_HISR);
   if (r == 0xFFFFFFFFUL)
      return;
   e = rtw_r32(REG_HISRE);
   r &= g_imr;
   e &= g_imre;
   if (r)
      rtw_w32(REG_HISR, r);                  /* write-1-to-clear (rtl8821ae_interrupt_recognized)             */
   if (e)
      rtw_w32(REG_HISRE, e);
   if (r) {
      g_hisr_last = r;
      g_isr_last = (USHORT)LO16(r);
   }
   if (r & IMR_RDU) g_rx_rdu++;
   if (e & IMRE_RXFOVW) g_rx_fovw++;
   if (e & IMRE_TXFOVW) g_tx_fovw++;
   if (r & IMR_C2HCMD)
      c2h_reg();
   rx_service();
   rtw_link_tick();
   sta_rx_tick();                            /* block-ack reorder buffers: frames that waited too long        */
}

void hw_service(void)
{
   USHORT id = (USHORT)(rt_cpu_id() + 1);

   if (!g_trx_on)
      return;
   if (rt_lock_try(&g_svc_lock, id)) {
      g_svc_again = 1;                       /* the owner runs once more                                       */
      return;
   }
   do {
      g_svc_again = 0;
      g_svc_runs++;
      service_once();
   } while (g_svc_again);
   rt_lock_release(&g_svc_lock);
}

void hw_hook(void)
{
   hw_service();
   if (g_irq_masked && g_trx_on) {
      g_irq_masked = 0;
      rtw_w32(REG_HIMR, g_imr);
      rtw_w32(REG_HIMRE, g_imre);
   }
}

int hw_wait(USHORT *flag, USHORT ms)
{
   ULONG t0 = dd_ms();

   for (;;) {
      hw_service();
      if (*flag)
         return 0;
      if ((long)(dd_ms() - t0) > (long)ms)
         return -1;
      dd_msleep(2);
   }
}

/* status line (rtw_hal.c hw_status) */
USHORT rtw_trx_state(USHORT what)
{
   switch (what) {
   case 0: return g_rxi;
   case 1: return g_txq[TXQ_BE].head;
   case 2: return g_txq[TXQ_MG].head;
   case 3: return g_irq_masked;
   default: return 0;
   }
}
