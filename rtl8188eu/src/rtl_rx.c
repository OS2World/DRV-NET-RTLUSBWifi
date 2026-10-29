/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL88EU - Realtek RTL8188EU USB WiFi driver for OS/2 and ArcaOS
 * Copyright (C) 2026 the RTL88EU-OS2 project
 *
 * Portions of this file are derived from the Linux rtl8188eu driver,
 * Copyright(c) 2007 - 2011 Realtek Corporation. All rights reserved.
 * That code is licensed under version 2 of the GNU General Public License
 * only (no "or later" option), so this file is GPL-2.0-only as well.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
/*
 * rtl_rx.c - RTL8188EU RX bring-up (Phase 2 milestone 2.6).
 *
 * 2.6a (this): enable receive - TX buffer boundary + LLT packet-buffer link
 * table (InitLLTTable) + RCR receive filter (promiscuous, accept management so
 * we hear beacons) + MAC TX/RX enable. The other post-RF hal_init steps (efuse
 * patch, _InitInterrupt, EDCA, beacon params, adaptive ctrl) are omitted as not
 * essential for a passive scan; add later if RX misbehaves.
 *
 * 2.6b (next): channel-set + the bulk-IN receive loop (the actual "hear a frame").
 */
#include "rt_usb.h"
#include "rt_ndis.h"

#define REG_TRXFF_BNDY              0x0114
#define REG_CR                      0x0100
#define REG_LLT_INIT                0x01E0
#define REG_TDECTRL                 0x0208
#define REG_RCR                     0x0608
#define REG_MAR                     0x0620
#define REG_TXPKTBUF_BCNQ_BDNY      0x0424
#define REG_TXPKTBUF_MGQ_BDNY       0x0425
#define REG_TXPKTBUF_WMAC_LBK_BF_HD 0x045D

#define TX_PAGE_BNDY   0xAA           /* TX_PAGE_BOUNDARY_88E                    */
#define LLT_LAST       176            /* LAST_ENTRY_OF_TX_PKT_BUFFER             */
/* RCR receive filter, bits per rtl8188e_spec.h (the MAC layout):
 *   AAP|APM|AM|AB (0x0F) + ADF BIT11 (accept data) + AMF BIT13 (accept
 *   MANAGEMENT = beacons) + ACRC32 BIT8|AICV BIT9 (accept CRC/ICV-error frames,
 *   so a mis-decoded frame still reaches us during bring-up).
 * BUG FIX: the old 0x0010000F set bit20 as "AMF" - that value is from the
 * MP-test PHY regdef (rtw_mp_phy_regdef.h), NOT the MAC RCR, so management
 * frames were never accepted and every beacon was filtered out by the MAC. */
#define RX_RCR         0x1000280FUL   /* AAP|APM|AM|AB + ADF + AMF(mgmt) + APP_PHYST_RXFF; NO CRC/ICV-error frames */
#define CR_MACTXRXEN   0x00C0         /* MACTXEN|MACRXEN (BIT6|BIT7)             */

/* One LLT entry write (_LLTWrite): value = data | (addr<<8) | (WRITE_ACCESS<<30).
 * Built via a union - avoids a 32-bit shift AND the SS!=DS stack-pointer trap
 * (a near-pointer cast of a stack local would be read DS-relative). The stock
 * driver's poll loop effectively reads once, so we do the same. */
static void llt_write(usb_dev_t *dev, UCHAR addr, UCHAR data)
{
   union { ULONG l; USHORT w[2]; } u;

   u.w[0] = (USHORT)(((USHORT)addr << 8) | data);
   u.w[1] = 0x4000;                     /* _LLT_OP(_LLT_WRITE_ACCESS) = bit30   */
   rtw_write32(dev, REG_LLT_INIT, u.l);
   (void)rtw_read32(dev, REG_LLT_INIT); /* settle (stock driver reads once)     */
}

/* TX buffer boundary + LLT packet-buffer link table (InitLLTTable). Split out so
 * the replay path can run it on its own. COM1: 'L' start, 'l' done. */
int rtl8188eu_llt_init(usb_dev_t *dev)
{
   USHORT i;

   /* TX buffer boundary = txpktbuf_bndy (set before LLT, per hal_init). */
   rtw_write8(dev, REG_TXPKTBUF_BCNQ_BDNY, TX_PAGE_BNDY);
   rtw_write8(dev, REG_TXPKTBUF_MGQ_BDNY, TX_PAGE_BNDY);
   rtw_write8(dev, REG_TXPKTBUF_WMAC_LBK_BF_HD, TX_PAGE_BNDY);
   rtw_write8(dev, REG_TRXFF_BNDY, TX_PAGE_BNDY);
   rtw_write8(dev, (USHORT)(REG_TDECTRL + 1), TX_PAGE_BNDY);

   /* LLT: link pages 0..bndy-1 sequentially, terminate, then ring the rest. */
   tcom('L');
   for (i = 0; i < (USHORT)(TX_PAGE_BNDY - 1); i++)
      llt_write(dev, (UCHAR)i, (UCHAR)(i + 1));
   llt_write(dev, (UCHAR)(TX_PAGE_BNDY - 1), 0xFF);
   for (i = TX_PAGE_BNDY; i < (USHORT)LLT_LAST; i++)
      llt_write(dev, (UCHAR)i, (UCHAR)(i + 1));
   llt_write(dev, (UCHAR)LLT_LAST, (UCHAR)TX_PAGE_BNDY);
   tcom('l');
   return 0;
}

/* Milestone 2.6a: bring up the receive path (registers only). COM1: 'L' LLT
 * start, 'l' LLT done, 'E' RX enabled. */
int rtl8188eu_rx_enable(usb_dev_t *dev)
{
   USHORT cr;

   rtl8188eu_llt_init(dev);

   /* Receive config: promiscuous + accept management (beacons); all multicast. */
   rtw_write32(dev, REG_RCR, RX_RCR);
   rtw_write32(dev, REG_MAR, 0xFFFFFFFFUL);
   rtw_write32(dev, (USHORT)(REG_MAR + 4), 0xFFFFFFFFUL);
   rtw_write8(dev, 0x060F, 4);          /* REG_RX_DRVINFO_SZ: 4*8 = 32B PHY status per frame */

   /* USB RX aggregation (DMA mode, InitUsbAggregationSetting): enable
    * RXDMA_AGG_EN in TRXDMA_CTRL + page count/timeout, so the chip flushes
    * received frames up the bulk-IN pipe (page timeout flushes even a single). */
   {
      /* RX aggregation OFF, as the Windows driver runs it (TRXDMA_CTRL low byte
       * 0xF0, RXDMA_AGG_EN clear): frames come up the bulk-IN pipe back to back,
       * each padded to 4 bytes (the scan parser relies on that). */
      UCHAR v8 = rtw_read8(dev, 0x010C);            /* REG_TRXDMA_CTRL         */
      rtw_write8(dev, 0x010C, (UCHAR)(v8 & ~0x04)); /* RXDMA_AGG_EN off        */
   }

   /* Enable the OFDM + CCK receiver demod blocks (rFPGA0_RFMOD 0x800). The BB
    * register table leaves bOFDMEn(BIT25)+bCCKEn(BIT24) OFF (0x80040000); the
    * stock driver turns them on only when RX actually starts (scan/connect),
    * which our manual path skipped -> demod dead -> zero false alarms -> deaf. */
   {
      ULONG rfmod = rtw_read32(dev, 0x0800);
      rtw_write32(dev, 0x0800, rfmod | 0x03000000UL);   /* bOFDMEn | bCCKEn */
   }

   /* Raise the initial gain (IGI, byte0 of rOFDM0_XAAGCCore1 0xC50) from the
    * table's 0x20 to 0x30: 694 false alarms with zero successful sync suggests
    * the RX is too sensitive - noise drowns real beacons. Higher IGI = less
    * gain = fewer false alarms, letting a strong beacon lock. (Cheap DIG tweak.) */
   rtw_write8(dev, 0x0C50, 0x30);

   /* Media status = AP (NT_LINK_AP): REG_CR bits[17:16] = 2. Without this the
    * MAC stays in NoLink mode (_InitNetworkType, usb_halinit.c) and may not
    * process received frames at all. */
   {
      ULONG cr32 = rtw_read32(dev, REG_CR);
      cr32 = (cr32 & ~0x00030000UL) | 0x00020000UL;
      rtw_write32(dev, REG_CR, cr32);
   }

   /* Per-subtype RX filter maps (REG_RXFLTMAP0/1/2): RCR_AMF is only the coarse
    * "accept management" gate - these are the fine per-subtype filter. If the
    * beacon subtype isn't enabled here, beacons are dropped despite RCR. Accept
    * ALL management (incl beacon), control and data subtypes. */
   rtw_write16(dev, 0x06A0, 0xFFFF);   /* management (beacon = subtype 8)      */
   rtw_write16(dev, 0x06A2, 0xFFFF);   /* control                              */
   rtw_write16(dev, 0x06A4, 0xFFFF);   /* data                                 */

   /* Enable MAC TX/RX. */
   cr = rtw_read16(dev, REG_CR);
   rtw_write16(dev, REG_CR, (USHORT)(cr | CR_MACTXRXEN));

   tcom('E');
   return 0;
}

/* ---- Milestone 2.6b: channel-set + one bulk-IN receive ----------------- */

/* Output a USHORT as 4 hex digits (16-bit constant shifts, no long helper). */
static void tcom_h16(USHORT v)
{
   static char hx[] = "0123456789ABCDEF";
   tcom(hx[(v >> 12) & 0x0F]);
   tcom(hx[(v >> 8)  & 0x0F]);
   tcom(hx[(v >> 4)  & 0x0F]);
   tcom(hx[ v        & 0x0F]);
}

/* Output one byte as 2 hex digits. */
static void tcom_h8(UCHAR b)
{
   static char hx[] = "0123456789ABCDEF";
   tcom(hx[(b >> 4) & 0x0F]);
   tcom(hx[ b       & 0x0F]);
}

/* Find the first bulk-IN endpoint by walking the cached config descriptor. */
static UCHAR find_bulk_in_ep(usb_dev_t *dev)
{
   UCHAR FAR *p;
   USHORT i, tot;

   if (dev->pDevInfo == 0)
      return 1;
   p   = dev->pDevInfo->configurationData;
   tot = (USHORT)((USHORT)p[2] | ((USHORT)p[3] << 8));   /* wTotalLength */
   if (tot == 0 || tot > 256)
      tot = 256;
   i = 0;
   while ((USHORT)(i + 3) < tot) {
      UCHAR len = p[i];
      if (len < 2)
         break;
      if (p[i + 1] == 0x05) {                 /* endpoint descriptor         */
         UCHAR addr = p[i + 2];
         UCHAR attr = p[i + 3];
         if ((addr & 0x80) && (UCHAR)(attr & 0x03) == 0x02) /* IN + bulk      */
            return (UCHAR)(addr & 0x0F);
      }
      i = (USHORT)(i + len);
   }
   return 1;                                   /* fallback: EP1               */
}

/* RF reg 0x18 (RF_CHNLBW) for a 20 MHz channel = 0xC00 | channel (bits[11:10] =
 * 11). The RF table leaves 0x0F407, i.e. BW bits = 01 = the 40 MHz filter; the
 * stock driver sets 0xC00 via PHY_SetBWMode -> rtl8188e_PHY_RF6052SetBandwidth
 * (rtl8188e_rf6052.c:113) and the Windows driver runs 0x00C00|ch (USBPcap
 * capture). We never made that call. Written over path-A LSSI 0x840 as
 * (0x18<<20) | value. */
static void rf_tune20(usb_dev_t *dev, USHORT ch)
{
   rtw_write32(dev, 0x0840, 0x01800C00UL | (ULONG)ch);
}

/* PHY-error delta over a dwell (RXERR_RPT 0x664) + good-frame count (0x284). */
static USHORT g_e0, g_e1, g_pk;
static void rx_probe(usb_dev_t *dev, USHORT ms)
{
   union { ULONG l; USHORT w[2]; } u;
   u.l = rtw_read32(dev, 0x0664); g_e0 = u.w[0];
   usb_mdelay(ms);
   u.l = rtw_read32(dev, 0x0664); g_e1 = u.w[0];
   u.l = rtw_read32(dev, 0x0284); g_pk = u.w[0];
}
static void rx_print(void)
{
   tcom_h16((USHORT)(g_e1 - g_e0)); tcom(','); tcom_h16(g_pk);
}

/* ===== Scan: receive-stream reassembly + beacon parser + network list ====== */

#define RXREAD   1024
#define ACCSZ    3600


net_t  g_net[MAXNET];
USHORT g_nnet;
static UCHAR  g_rxbuf[RXREAD];
static USHORT g_rxact;
static UCHAR  g_acc[ACCSZ];
static USHORT g_accn;
static UCHAR  g_acc_reset;                 /* task -> completion: drop the partial stream at the next feed. Only the
                                            * completion touches g_acc/g_accn, which on SMP runs alongside the task. */
static UCHAR  g_curch;
UCHAR  g_mymac[6] = { 0x02, 0x88, 0xE0, 0x00, 0x00, 0x01 };  /* fallback (locally administered); the real MAC is read from the eFuse at init */
static USHORT g_prme;     /* probe responses addressed to US */
static UCHAR  g_target[6];                 /* BSSID of the AP we are associating with */
static UCHAR  g_rx_auth, g_rx_assoc;       /* got an auth / assoc response from the target */
static USHORT g_auth_st = 0xFFFF, g_assoc_st = 0xFFFF, g_aid, g_rx_deauth;
static void tcom_str(char *s);
static USHORT g_nfrm, g_nbcn, g_ndesync, g_dbg, g_dbg2, g_dbgc, g_dbgo, g_lastdbg, g_prevpl, g_prevtot, g_ncrc, g_npr;
/* receive-stream diagnostics (read back through \\DEV\\WRND32$) */
/* event log of the last join: 1 auth TX, 2 assoc TX, 3 TX returned, others = frame-control byte of a unicast frame to us;
 * time in ms since the join started */
#define EVN 40
static UCHAR  g_ev_k[EVN], g_ev_i[EVN];
static USHORT g_ev_t[EVN], g_ev_n;
static ULONG  g_ev_t0;
static void ev2(UCHAR k, UCHAR info)
{
   if (g_ev_n < EVN) {
      g_ev_k[g_ev_n] = k;
      g_ev_i[g_ev_n] = info;
      g_ev_t[g_ev_n] = (USHORT)(rtl8188eu_ms() - g_ev_t0);
      g_ev_n++;
   }
}
#define ev(k) ev2((UCHAR)(k), 0)
static USHORT g_dg_ack, g_dg_ctl, g_dg_tmn;        /* ACK frames to our MAC / any control frame seen while joining / frames-to-us recorded */
static UCHAR  g_dg_tm[24];                         /* first three unicast frames to us: subtype, source MAC, length low byte */
static USHORT g_dg_tome;                           /* unicast management frames addressed to our MAC */
static USHORT g_dg_feeds, g_dg_maxn, g_dg_full, g_dg_dsev, g_dg_lastds, g_dg_multi, g_dg_pad8, g_dg_dsn;
static UCHAR  g_dg_ctx[72];                        /* first two desyncs: 12 bytes before + 24 after each */
static USHORT g_dg_inf[8];                         /* ... and prevpl, prevtot, off, accn */

/* Join state (read by rtl_nic.c) and the association IEs the supplicant asks for. */
UCHAR  g_joined, g_jch, g_jslen;
UCHAR  g_jbssid[6], g_jssid[33];
USHORT g_jrescap, g_jstatus, g_jaidv;
UCHAR  g_jreq[96], g_jres[128];
USHORT g_jreqlen, g_jreslen;

/* Received EAPOL frames (as Ethernet frames), waiting for the supplicant. */
#define EQN   4
#define EQSZ  288
static UCHAR  g_eq[EQN][EQSZ];
static USHORT g_eqlen[EQN];
static USHORT g_eqh, g_eqt, g_eqn;

static void tcom_dec(USHORT v)
{
   char b[6];
   int  i = 0;
   if (v == 0) { tcom('0'); return; }
   while (v) { b[i++] = (char)('0' + (v % 10)); v = (USHORT)(v / 10); }
   while (i) tcom(b[--i]);
}

/* Signal strength (dBm) from the PHY status that follows the 24B RX descriptor
 * (drvinfo): CCK from the AGC report (LNA/VGA table), OFDM/HT from the PWDB byte -
 * the stock driver's odm_HWConfig.c formulas. 0 = no PHY status present. */
static signed char rx_rssi(UCHAR *d, USHORT drv)
{
   UCHAR  rate = (UCHAR)(d[12] & 0x3F);
   UCHAR *ph = d + 24;
   int    pw;
   UCHAR  lna, vga;

   if (!(d[3] & 0x04) || drv < 20)
      return 0;
   if (rate <= 3) {                              /* CCK 1/2/5.5/11               */
      lna = (UCHAR)((ph[5] & 0xE0) >> 5);               /* CCK AGC report: byte 5 on this chip (measured) */
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
   } else {                                      /* OFDM / HT                    */
      pw = (int)((ph[4] >> 1) & 0x7F) - 110;           /* OFDM PWDB: byte 4 (measured: 0x65..0x82 for nearby APs) */
   }
   if (pw > 0) pw = 0;
   if (pw < -110) pw = -110;
   return (signed char)pw;
}

/* ===== Data frames from the AP (FromDS) ======================================
 * With the key in the chip's CAM the hardware decrypts CCMP: the frame arrives with its
 * 8-byte CCMP header still after the 802.11 header and the 8-byte MIC at the tail
 * (stock driver: iv_len = icv_len = 8). rx_parse sets g_rxsw when the chip could NOT
 * decrypt (software-decrypt flag, RX descriptor dw0 bit 27). Frames are converted to
 * Ethernet format: EAPOL goes to the supplicant's queue, everything else to the data
 * queue (consumed by the self-test now, by the NDIS front-end later). */
#define DQN   4
#define DQSZ  600
static UCHAR  g_dq[DQN][DQSZ];
static USHORT g_dqlen[DQN];
static USHORT g_dqh, g_dqt, g_dqn;
static USHORT g_ndrop, g_nd, g_np, g_nsw;       /* diag: AP data frames seen / protected / not decrypted by chip */
static UCHAR  g_rxsw;

/* Duplicate filter (IEEE 802.11-2016 10.3.2.14): the AP resends a frame whose acknowledgement it did not hear, with the
 * Retry bit set and the same sequence control. Without this filter the copy went up to TCP/IP too: duplicate segments
 * make the TCP receiver send duplicate ACKs, and the sender takes them for loss and slows down. One cache entry per
 * traffic identifier (QoS TID 0..15, 16 = non-QoS). */
static USHORT g_sc[17];
static USHORT g_ndup;

static USHORT g_led_act;                        /* set by data traffic, consumed by rtl8188eu_led_tick */
static UCHAR  g_ptk_ok, g_gtk_ok;                    /* pairwise / group key installed on the current join */

static void rx_data_frame(UCHAR *f, USHORT len)
{
   static USHORT nh;                              /* static: its address is passed to ndis_rx_get (SS != DS) */
   USHORT hl, i, pl, tail = 0, n;
   int    nd = 0;
   UCHAR  *d, *src;
   UCHAR  eapol;

   g_led_act = 1;
   if (f[0] & 0x40)                               /* null / no-data subtypes */
      return;
   for (i = 0; i < 6; i++)
      if (f[10 + i] != g_jbssid[i])
         return;
   if ((f[4] & 1) == 0)                           /* unicast: must be to us  */
      for (i = 0; i < 6; i++)
         if (f[4 + i] != g_mymac[i])
            return;
   {
      USHORT sc  = (USHORT)(f[22] | (f[23] << 8));
      UCHAR  tid = (UCHAR)((f[0] & 0x80) ? (f[24] & 0x0F) : 16);

      if ((f[1] & 0x08) && g_sc[tid] == sc) {     /* retransmission of the frame we already have */
         g_ndup++;
         return;
      }
      g_sc[tid] = sc;
   }
   g_nd++;
   if (f[1] & 0x40) { g_np++; if (g_rxsw) g_nsw++; }
   hl = (USHORT)((f[0] & 0x80) ? 26 : 24);
   if (f[1] & 0x40) {                             /* protected: CCMP header + MIC */
      if (g_rxsw) { g_ndrop++; return; }          /* chip did not decrypt: no key */
      hl = (USHORT)(hl + 8);
      tail = 0;                                   /* chip already strips the MIC: IP length matches exactly without trimming */
   }
   if ((USHORT)(hl + 8 + tail) > len)
      return;
   if (f[hl] != 0xAA || f[hl + 1] != 0xAA || f[hl + 2] != 0x03 || f[hl + 3] || f[hl + 4] || f[hl + 5])
      return;
   pl = (USHORT)(len - tail - hl - 8);            /* payload after the SNAP header */
   src = f + hl + 8;
   eapol = (UCHAR)(f[hl + 6] == 0x88 && f[hl + 7] == 0x8E);
   /* WPA2 rule: only EAPOL may travel unprotected. Every other data frame needs the pairwise key to be
    * installed AND the Protected bit set; otherwise anyone who spoofs the AP address could inject plaintext. */
   if (!eapol && (!g_ptk_ok || !(f[1] & 0x40))) {
      g_ndrop++;
      return;
   }
   if (eapol && pl >= 4) {
      USHORT el = (USHORT)(4 + ((src[2] << 8) | src[3]));
      if (el < pl) pl = el;
   }
   n = (USHORT)(14 + pl);
   if (eapol) {
      if (g_eqn >= EQN || n > EQSZ) return;
      d = g_eq[g_eqt];
   } else if (ndis_is_open()) {                    /* bound + open protocol stack: straight to NDIS */
      if (n > MAX_FRAME_SIZE) { g_ndrop++; return; }
      d = ndis_rx_get(&nh);
      if (!d) { g_ndrop++; return; }
      nd = 1;
   } else {
      if (g_dqn >= DQN || n > DQSZ) { g_ndrop++; return; }
      d = g_dq[g_dqt];
   }
   for (i = 0; i < 6; i++) { d[i] = f[4 + i]; d[6 + i] = f[16 + i]; }   /* DA, SA */
   d[12] = f[hl + 6]; d[13] = f[hl + 7];
   for (i = 0; i < pl; i++) d[14 + i] = src[i];
   if (eapol) {
      g_eqlen[g_eqt] = n;
      g_eqt = (USHORT)((g_eqt + 1) % EQN);
      rt_atomic_inc(&g_eqn);                       /* the supplicant's request pops on another CPU */
      tcom_str("[eapol rx "); tcom_dec(pl); tcom(']');
   } else if (nd) {
      if (dhcp_rx(d, n))                           /* reply to the driver's own DHCP exchange */
         ndis_rx_free(nh);
      else
         ndis_rx_put(nh, n);
   } else if (!dhcp_rx(d, n)) {
      g_dqlen[g_dqt] = n;
      g_dqt = (USHORT)((g_dqt + 1) % DQN);
      g_dqn++;
   }
}

/* One management frame (beacon 0x80 / probe response 0x50): record the network. */
static void note_frame(UCHAR *f, USHORT len, signed char rssi)
{
   UCHAR *ie, *end;
   USHORT cap, i, k, ielim;
   UCHAR  ch, sec, slen;
   char   ssid[33];

   g_nfrm++;
   if ((f[0] & 0x0C) == 0x04) {                     /* control frame (only passed on while a join is running) */
      g_dg_ctl++;
      if ((f[0] & 0xF0) == 0xD0 && len >= 10) {     /* ACK: receiver address = us -> our last frame reached an AP */
         USHORT q;
         UCHAR  me = 1;
         for (q = 0; q < 6; q++)
            if (f[4 + q] != g_mymac[q]) { me = 0; break; }
         if (me) { g_dg_ack++; ev(0xD4); }
      }
      return;
   }
   if (g_ev_n < EVN && len >= 24 && (f[0] & 0xFC) != 0x80) {   /* non-beacon frame sent by the AP we are joining, not to us */
      USHORT q;
      UCHAR  fromt = 1;
      for (q = 0; q < 6; q++)
         if (f[10 + q] != g_target[q]) { fromt = 0; break; }
      if (fromt && f[9] != g_mymac[5])
         ev2(f[0], f[9]);
   }
   if (g_joined && len >= 32 && (f[0] & 0x0C) == 0x08 && (f[1] & 0x03) == 0x02) {
      rx_data_frame(f, len);                     /* data frame from the AP (FromDS) */
      return;
   }
   if (len >= 28 && (f[0] & 0x0C) == 0 && (f[4] & 1) == 0) {   /* mgmt, unicast: auth / assoc response, deauth */
      UCHAR mine = 1, tome = 1;
      USHORT q;
      for (q = 0; q < 6; q++)
         if (f[4 + q] != g_mymac[q]) { tome = 0; break; }
      if (tome) {
         ev2(f[0], 0xEE);
         g_dg_tome++;
         if (g_dg_tmn < 3) {
            USHORT q, b = (USHORT)(g_dg_tmn * 8);
            g_dg_tm[b] = f[0];
            for (q = 0; q < 6; q++) g_dg_tm[b + 1 + q] = f[10 + q];
            g_dg_tm[b + 7] = (UCHAR)len;
            g_dg_tmn++;
         }
      }
      for (q = 0; q < 6; q++)
         if (f[4 + q] != g_mymac[q] || f[10 + q] != g_target[q]) { mine = 0; break; }
      if (mine) {
         UCHAR sub = (UCHAR)(f[0] & 0xF0);
         if (sub == 0xB0 && len >= 30) {
            g_rx_auth = 1;
            g_auth_st = (USHORT)(f[28] | (f[29] << 8));
         } else if (sub == 0x10 && len >= 30) {
            g_rx_assoc = 1;
            g_assoc_st = (USHORT)(f[26] | (f[27] << 8));
            g_aid = (USHORT)((f[28] | (f[29] << 8)) & 0x3FFF);
            g_jrescap = (USHORT)(f[24] | (f[25] << 8));
            g_jreslen = 0;                                    /* keep whole IEs only (no FCS tail) */
            for (q = 30; (USHORT)(q + 2) <= len && (USHORT)(q + 2 + f[q + 1]) <= len &&
                         (USHORT)(g_jreslen + 2 + f[q + 1]) <= sizeof(g_jres); q = (USHORT)(q + 2 + f[q + 1])) {
               USHORT m;
               for (m = 0; m < (USHORT)(2 + f[q + 1]); m++)
                  g_jres[g_jreslen + m] = f[q + m];
               g_jreslen = (USHORT)(g_jreslen + 2 + f[q + 1]);
            }
         } else if (sub == 0xC0 || sub == 0xA0) {
            g_rx_deauth = (USHORT)((f[24] | (f[25] << 8)) + 1);
         }
      }
   }
   if (len < 38 || (f[0] & 0x0C) != 0)          /* management frames only       */
      return;
   if ((f[0] & 0xF0) != 0x80 && (f[0] & 0xF0) != 0x50)
      return;
   if (f[16] & 0x01)                            /* BSSID must be unicast        */
      return;
   for (i = 0; i < 6; i++)                      /* beacon: SA must equal BSSID  */
      if (f[10 + i] != f[16 + i])
         return;

   g_nbcn++;
   if ((f[0] & 0xF0) == 0x50) {
      UCHAR me = 1;
      g_npr++;                                  /* probe response              */
      for (i = 0; i < 6; i++)
         if (f[4 + i] != g_mymac[i]) { me = 0; break; }
      if (me)
         g_prme++;                              /* ...addressed to our MAC     */
   }
   cap  = (USHORT)(f[34] | (f[35] << 8));
   ch   = g_curch;
   sec  = 0;
   slen = 0;
   ssid[0] = 0;
   ie  = f + 36;
   end = f + len;
   ielim = 12;                                  /* timestamp + interval + capability */
   while (ie + 2 <= end) {
      UCHAR t = ie[0], l = ie[1];
      if (ie + 2 + l > end)
         break;
      if (t == 0) {                              /* SSID                         */
         slen = (l > 32) ? 32 : l;
         for (i = 0; i < slen; i++) {
            UCHAR c = ie[2 + i];
            ssid[i] = (char)c;                            /* raw byte: UTF-8 names must match */
         }
         ssid[slen] = 0;
      } else if (t == 3 && l >= 1) {             /* DS Parameter Set = channel   */
         ch = ie[2];
      } else if (t == 48) {                      /* RSN = WPA2                   */
         sec |= 2;
      } else if (t == 221 && l >= 4 && ie[2] == 0x00 && ie[3] == 0x50 &&
                 ie[4] == 0xF2 && ie[5] == 0x01) {   /* MS WPA IE               */
         sec |= 1;
      }
      ie += 2 + l;
      if ((USHORT)(ie - (f + 24)) <= IEMAX)
         ielim = (USHORT)(ie - (f + 24));      /* last whole IE that still fits */
   }
   if (sec == 0 && (cap & 0x0010))
      sec = 4;                                   /* privacy bit, no WPA/RSN: WEP */

   for (k = 0; k < g_nnet; k++) {
      UCHAR same = 1;
      for (i = 0; i < 6; i++)
         if (g_net[k].bssid[i] != f[16 + i]) { same = 0; break; }
      if (same && g_net[k].ch == ch) {
         g_net[k].cnt++;
         if (rssi)                               /* smooth: (3 old + 1 new) / 4 */
            g_net[k].rssi = g_net[k].rssi ? (signed char)(((int)g_net[k].rssi * 3 + rssi) >> 2) : rssi;
         if (slen && !g_net[k].len) {            /* fill a hidden/late SSID      */
            g_net[k].len = slen;
            for (i = 0; i <= slen; i++) g_net[k].ssid[i] = ssid[i];
         }
         return;
      }
   }
   if (g_nnet >= MAXNET)
      return;
   for (i = 0; i < 6; i++) g_net[g_nnet].bssid[i] = f[16 + i];
   g_net[g_nnet].ch  = ch;
   g_net[g_nnet].sec = sec;
   g_net[g_nnet].len = slen;
   g_net[g_nnet].rssi = rssi;
   for (i = 0; i <= slen; i++) g_net[g_nnet].ssid[i] = ssid[i];
   g_net[g_nnet].cnt = 1;
   g_net[g_nnet].ielen = ielim;
   for (i = 0; i < ielim; i++)
      g_net[g_nnet].ie[i] = f[24 + i];
   g_nnet++;
}

/* Is there a plausible RX descriptor at g_acc[pos]? 1 yes, 0 no, 2 cannot tell yet (bytes missing).
 * Every frame carries the fixed 32-byte PHY status (REG_RX_DRVINFO_SZ = 4), so the drvinfo nibble must be 4. */
static int hdr_ok(USHORT pos)
{
   USHORT l;

   if ((USHORT)(pos + 4) > g_accn)
      return 2;
   l = (USHORT)(g_acc[pos] | ((g_acc[pos + 1] & 0x3F) << 8));
   if (l == 0 || l > 2400 || (g_acc[pos + 2] & 0x0F) != 4)
      return 0;
   return 1;
}

/* Walk g_acc: [24B RX desc][drvinfo][shift][pkt_len bytes] per frame, back to
 * back (RX aggregation is off, so no padding). Frames can straddle reads, so
 * leftover bytes are shifted to the front and completed by the next read. */
static void rx_parse(void)
{
   USHORT off = 0, remain, pl, drv, sh, total, i, nfr = 0, t8;
   UCHAR *p, newev;

   for (;;) {
      remain = (USHORT)(g_accn - off);
      if (remain < 24)
         break;
      p   = g_acc + off;
      pl  = (USHORT)(p[0] | ((p[1] & 0x3F) << 8));
      drv = (USHORT)((p[2] & 0x0F) * 8);
      sh  = (USHORT)(p[3] & 0x03);
      if (pl == 0 || pl > 2400 || drv > 64) {    /* not a descriptor: resync     */
         g_ndesync++;
         newev = (UCHAR)((USHORT)(off - g_dg_lastds) != 1 || g_dg_dsev == 0);
         if (newev)
            g_dg_dsev++;
         g_dg_lastds = off;
         if (newev && g_dg_dsn < 2) {
            USHORT q, b = (USHORT)(g_dg_dsn * 36), m = (USHORT)(g_dg_dsn * 4);

            g_dg_inf[m] = g_prevpl; g_dg_inf[m + 1] = g_prevtot; g_dg_inf[m + 2] = off; g_dg_inf[m + 3] = g_accn;
            for (q = 0; q < 12; q++)
               g_dg_ctx[b + q] = (UCHAR)((USHORT)(off + q) >= 12 ? g_acc[off + q - 12] : 0);
            for (q = 0; q < 24; q++)
               g_dg_ctx[b + 12 + q] = (UCHAR)((USHORT)(off + q) < g_accn ? g_acc[off + q] : 0);
            g_dg_dsn++;
         }
         if (g_dbg < 4 && (USHORT)(off - g_lastdbg) > 8) {   /* first few: show context */
            USHORT q;
            g_dbg++; g_lastdbg = off;
            tcom(10); tcom('!'); tcom_dec(g_prevpl); tcom('/'); tcom_dec(g_prevtot);
            tcom('@'); tcom_dec(off); tcom('/'); tcom_dec(g_accn); tcom(' ');
            for (q = (off >= 12 ? (USHORT)(off - 12) : 0); q < off; q++) tcom_h8(g_acc[q]);
            tcom('|');
            for (q = 0; q < 20 && (USHORT)(off + q) < g_accn; q++) tcom_h8(p[q]);
            tcom(10);
         }
         off++;
         continue;
      }
      total = (USHORT)(24 + drv + sh + pl);
      if (remain < total)
         break;                                  /* wait for the rest            */
      g_rxsw = (UCHAR)(p[3] & 0x08);                /* RX desc dw0 bit 27: chip did not decrypt */
      if (!(p[1] & 0xC0))                         /* skip CRC32 / ICV-error frames */
         note_frame(p + 24 + drv, pl, rx_rssi(p, drv));
      else
         g_ncrc++;
      if ((p[3] & 0x04) && drv >= 16 && (p[24 + drv] & 0xFC) == 0x80) {   /* PHY-status layout probe */
         UCHAR isck = (UCHAR)((p[12] & 0x3F) <= 3);
         if ((isck && g_dbgc < 4) || (!isck && g_dbgo < 4)) {
            USHORT q;
            if (isck) g_dbgc++; else g_dbgo++;
            tcom(10); tcom(isck ? 'C' : 'O'); tcom_h8((UCHAR)(p[12] & 0x3F)); tcom(':');
            for (q = 0; q < 16; q++) tcom_h8(p[24 + q]);
         }
      }
      g_prevpl = pl; g_prevtot = total;
      nfr++;
      /* The chip pads each frame to a 4- or 8-byte boundary depending on the host/mode: take the
       * padded step whose next descriptor looks right, else the plain end of this frame. */
      t8 = (USHORT)((total + 7) & ~7);
      total = (USHORT)((total + 3) & ~3);
      if (hdr_ok((USHORT)(off + total)) == 1)
         ;                                         /* 4-byte padding */
      else if (t8 != total && hdr_ok((USHORT)(off + t8)) == 1) {
         total = t8;                               /* 8-byte padding */
         g_dg_pad8++;
      } else if (hdr_ok((USHORT)(off + (USHORT)(24 + drv + sh + pl))) == 1)
         total = (USHORT)(24 + drv + sh + pl);     /* no padding */
      else if (remain < total)
         total = (USHORT)(24 + drv + sh + pl);
      off = (USHORT)(off + total);
   }
   if (nfr > 1)
      g_dg_multi++;
   if (off) {
      for (i = 0; i < (USHORT)(g_accn - off); i++)
         g_acc[i] = g_acc[off + i];
      g_accn = (USHORT)(g_accn - off);
   }
}

/* The receive stream is now fed by the always-on asynchronous bulk-IN request (see
 * usb_rx_start / rtl_rx_feed). What used to be a blocking read is a plain wait: the
 * callers loop on flags (g_rx_auth, g_rx_assoc, ...) that the completion handler sets,
 * or just collect beacons into the scan table for the duration. */
static USHORT rx_read(usb_dev_t *dev, UCHAR ep, USHORT tmo_ms)
{
   usb_mdelay(tmo_ms);
   return 0;
}

/* Completion context (task time, must not block): append to the stream and parse. */
void rtl_rx_feed(UCHAR *buf, USHORT n)
{
   USHORT i;

   g_dg_feeds++;
   if (n > g_dg_maxn) g_dg_maxn = n;
   if (n >= USB_RXBUF) g_dg_full++;
   if (g_acc_reset) {                            /* channel change / join: start a fresh stream */
      g_acc_reset = 0;
      g_accn = 0;
   }
   if ((USHORT)(g_accn + n) > ACCSZ)             /* overflow guard: start over   */
      g_accn = 0;
   for (i = 0; i < n; i++)
      g_acc[g_accn + i] = buf[i];
   g_accn = (USHORT)(g_accn + n);
   rx_parse();
}

void rtl8188eu_rx_engine_start(usb_dev_t *dev)
{
   if (!g_rx_run)
      usb_rx_start(dev, find_bulk_in_ep(dev));
}

static void tcom_mac(UCHAR *m)
{
   USHORT i;
   for (i = 0; i < 6; i++) {
      tcom_h8(m[i]);
      if (i < 5) tcom(':');
   }
}

static void tcom_str(char *s)
{
   while (*s) tcom(*s++);
}

/* TX prerequisites: queue->endpoint map as the Windows driver runs it (HIQ/MGQ/VIQ/VOQ ->
 * high EP, BE/BK -> normal EP), our MAC address, TX queues un-paused, response-rate set. */
void rtl8188eu_tx_prepare(usb_dev_t *dev)
{
   USHORT i;

   rtw_write16(dev, 0x010C, 0xFAF0);
   for (i = 0; i < 6; i++)
      rtw_write8(dev, (USHORT)(0x0610 + i), g_mymac[i]);
   rtw_write8(dev, 0x0522, 0x00);
   rtw_write32(dev, 0x0440, 0x000FFFF1UL);
}

/* One quiet pass over channels 1..13 into g_net (no serial output). The table is rebuilt. */
void rtl8188eu_scan_once(usb_dev_t *dev)
{
   UCHAR  ep = find_bulk_in_ep(dev);
   USHORT ch, r;

   g_nnet = 0;
   for (ch = 1; ch <= 13; ch++) {
      rf_tune20(dev, (UCHAR)ch);
      g_curch = (UCHAR)ch;
      g_acc_reset = 1;
      usb_mdelay(40);
      for (r = 0; r < 4; r++)
         if (rx_read(dev, ep, 150) == 0)
            break;
   }
}

/* Print the network table: summary line + one line per network. */
static void print_nets(char *title)
{
   USHORT k;

   tcom(10); tcom_str(title); tcom_str(" n="); tcom_dec(g_nnet);
   tcom_str(" frm="); tcom_dec(g_nfrm);
   tcom_str(" bcn="); tcom_dec(g_nbcn);
   tcom_str(" pr="); tcom_dec(g_npr);
   tcom_str(" prme="); tcom_dec(g_prme);
   tcom_str(" desync="); tcom_dec(g_ndesync);
   tcom_str(" crc="); tcom_dec(g_ncrc); tcom(10);
   for (k = 0; k < g_nnet; k++) {
      tcom('#'); tcom_dec((USHORT)(k + 1)); tcom(' ');
      tcom_mac(g_net[k].bssid);
      tcom_str(" ch"); tcom_dec(g_net[k].ch);
      tcom(' ');
      if (g_net[k].sec & 2)       tcom_str("WPA2");
      else if (g_net[k].sec & 1)  tcom_str("WPA");
      else if (g_net[k].sec & 4)  tcom_str("WEP");
      else                        tcom_str("open");
      tcom(' ');
      tcom_str(g_net[k].len ? g_net[k].ssid : "<hidden>");
#if SHOW_RSSI
      if (g_net[k].rssi) { tcom_str(" -"); tcom_dec((USHORT)(-g_net[k].rssi)); tcom_str("dBm"); }
#endif
      tcom_str(" x"); tcom_dec(g_net[k].cnt); tcom(10);
   }
}

/* ===== Transmit: management frames over bulk-OUT ============================
 * A bulk-OUT transfer is [32-byte TX descriptor][16 bytes pad][802.11 frame].
 * Layout and values follow the stock driver (rtl8188eu_xmit.c update_txdesc,
 * management path) and were checked against the probe requests the Windows
 * driver sent in the USBPcap capture: same descriptor words, same checksum.
 *   dw0 OWN|FS|LS | BMC | offset 32 | pkt_len     dw1 macid | QSEL_MGNT<<8 | pkt_offset 2 (16 B)
 *   dw3 EN_HWSEQ (hardware numbers the frame)    dw4 USERATE | disable data fallback
 *   dw5 fallback limits | rate (0 = 1 Mbps CCK)   dw7 = XOR of the 16 descriptor words
 * Management frames use bulk endpoint 2 (the high-priority queue). */
static UCHAR  g_txbuf[760];
static USHORT g_txact;

static void put32(UCHAR *p, USHORT hi, USHORT lo)
{
   p[0] = (UCHAR)(lo & 0xFF);
   p[1] = (UCHAR)(lo >> 8);
   p[2] = (UCHAR)(hi & 0xFF);
   p[3] = (UCHAR)(hi >> 8);
}

/* Send one frame (header+body in f, n bytes). data=0: MGNT queue on bulk EP2 (management and
 * plain EAPOL, proven). data=1: BE data queue on bulk EP3, as the stock driver sends data;
 * sec=1 selects hardware AES-CCMP (the frame already carries the 8-byte CCMP header). */
/* Fill the 32-byte TX descriptor (+16 pad bytes) at the start of b for an n-byte frame at b+48. */
UCHAR g_txrate = 8;                                 /* data rate code: 0=1M 4=6M 6=12M 8=24M 9=36M 0xB=54M (fixed for now) */

UCHAR g_ctlrate;                               /* rate code for handshake / ARP / DHCP frames (0 = 1 Mbit/s CCK); debug cmd "ctl" */

static void txd_fill(UCHAR *b, USHORT n, int bcast, int data, int sec, UCHAR rate)
{
   USHORT i, ck = 0;

   for (i = 0; i < 48; i++) b[i] = 0;

   put32(b + 0,  (USHORT)(bcast ? 0x8D20 : 0x8C20), n);       /* OWN FS LS [BMC] off=32 len */
   if (data)
      put32(b + 4, (USHORT)(0x0800 | (sec ? 0x00C0 : 0)), 0x0001);   /* macid 1, QSEL_BE, SEC_TYPE, pkt_offset 2 */
   else
      put32(b + 4, 0x0800, 0x1201);                           /* macid 1, QSEL_MGNT, pkt_offset 2 */
   put32(b + 8,  (USHORT)(data ? 0x0001 : 0), 0);             /* data: AGG_BK */
   put32(b + 12, 0x8000, 0);                                  /* EN_HWSEQ */
   put32(b + 16, 0, 0x0500);                                  /* USERATE | DISDATAFB */
   put32(b + 20, 0, (USHORT)(0x1F00 | (rate & 0x3F)));         /* fallback limits | rate code */
   put32(b + 24, 0, 0);
   put32(b + 28, 0, 0);
   for (i = 0; i < 16; i++)                                   /* checksum over the 16 words */
      ck ^= (USHORT)(b[2 * i] | (b[2 * i + 1] << 8));
   b[28] = (UCHAR)(ck & 0xFF);
   b[29] = (UCHAR)(ck >> 8);
}

/* Diagnostics, read back through \DEV\WRND32$ (see rtl_dhcp.c): why the last join failed, and USB/radio counters. */
static USHORT g_dg_reason;                         /* 0 ok, 1 no BSS with the wanted SSID, 2 no auth reply, 3 auth refused,
                                                    * 4 no assoc reply, 5 assoc refused, 6 wrong auth/cipher mode set */
static USHORT g_dg_auth = 0xFFFF, g_dg_assoc = 0xFFFF;   /* last status codes (0xFFFF = no reply) */
static USHORT g_dg_ch, g_dg_rssi, g_dg_nnet, g_dg_tries, g_dg_wantlen, g_dg_bssf;
static USHORT g_dg_txok, g_dg_txfail, g_dg_txlast;

UCHAR g_mgmtrate;                                  /* debug "mrate N": rate code for management frames (0 = 1 Mbit/s CCK) */
UCHAR g_txasync;                                   /* debug "txa 1": send management frames through the async TX slots */

static int tx_raw(usb_dev_t *dev, UCHAR *f, USHORT n, int bcast, int data, int sec)
{
   USHORT i;
   int    rc;

   if (g_txasync) {
      USHORT slot = usb_tx_alloc(), w;
      UCHAR  *b;

      if (slot == 0xFFFF)
         return -1;
      b = usb_tx_buf(slot);
      for (i = 0; i < n; i++)  b[48 + i] = f[i];
      txd_fill(b, n, bcast, data, sec, (UCHAR)(data ? g_ctlrate : g_mgmtrate));
      rc = usb_tx_submit(dev, (UCHAR)(data ? 3 : 2), slot, (USHORT)(48 + n));
      if (rc == 0) g_dg_txok++;
      else         { g_dg_txfail++; g_dg_txlast = (USHORT)rc; }
      for (w = 0; w < 100 && g_tx_busy; w++)
         usb_mdelay(1);
      return rc;
   }
   for (i = 0; i < n; i++)  g_txbuf[48 + i] = f[i];
   txd_fill(g_txbuf, n, bcast, data, sec, (UCHAR)(data ? g_ctlrate : g_mgmtrate));   /* mgmt frames 1 Mbit/s unless "mrate"; handshake data frames g_ctlrate */
   g_txact = 0;
   rc = usb_bulk_msg(dev, (UCHAR)(data ? 3 : 2), USB_DIR_OUT, g_txbuf, (USHORT)(48 + n), &g_txact);
   if (rc == 0) g_dg_txok++;
   else         { g_dg_txfail++; g_dg_txlast = (USHORT)rc; }
   return rc;
}

static int tx_mgmt(usb_dev_t *dev, UCHAR *f, USHORT n, int bcast)
{
   return tx_raw(dev, f, n, bcast, 0, 0);
}

/* Control-path health: chip id (0xF0), RCR, TXPAUSE, CR. All zeros = control reads failing. */
static void txchk(usb_dev_t *dev, char *tag)
{
   union { ULONG l; USHORT w[2]; } u;

   tcom(' '); tcom_str(tag); tcom('=');
   u.l = rtw_read32(dev, 0x00F0); tcom_h16(u.w[1]); tcom_h16(u.w[0]); tcom('/');
   u.l = rtw_read32(dev, 0x0608); tcom_h16(u.w[1]); tcom_h16(u.w[0]); tcom('/');
   tcom_h8(rtw_read8(dev, 0x0522));
}

/* Broadcast probe request with a wildcard SSID, 1-11 + 6-54 Mbps rates, DS channel. */
static int tx_probe_req(usb_dev_t *dev, UCHAR ch)
{
   static UCHAR f[64];
   static UCHAR rates[8]  = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
   static UCHAR xrates[4] = { 0x30, 0x48, 0x60, 0x6C };
   USHORT i, n = 0;

   for (i = 0; i < 64; i++) f[i] = 0;
   f[0] = 0x40;                                   /* probe request              */
   for (i = 0; i < 6; i++) {
      f[4 + i]  = 0xFF;                           /* DA broadcast               */
      f[10 + i] = g_mymac[i];                     /* SA                         */
      f[16 + i] = 0xFF;                           /* BSSID broadcast            */
   }
   n = 24;
   f[n++] = 0;  f[n++] = 0;                       /* SSID: wildcard             */
   f[n++] = 1;  f[n++] = 8;
   for (i = 0; i < 8; i++) f[n++] = rates[i];     /* supported rates            */
   f[n++] = 50; f[n++] = 4;
   for (i = 0; i < 4; i++) f[n++] = xrates[i];    /* extended rates             */
   f[n++] = 3;  f[n++] = 1; f[n++] = ch;          /* DS parameter set           */
   return tx_mgmt(dev, f, n, 1);
}

/* ===== Association (open system auth + association request) ================
 * Frames and descriptors copied from what the Windows driver sent when it joined
 * the same network (USBPcap capture): auth = 30 bytes (algo 0, seq 1), assoc request
 * = capability 0x1631, listen interval 2, SSID, rates, extended rates and a WPA2-PSK
 * RSN IE (CCMP/CCMP/PSK). Directed frames use the same TX descriptor as the probe
 * request with the broadcast bit cleared. */
static UCHAR g_rt8[8]  = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
static UCHAR g_rt4[4]  = { 0x30, 0x48, 0x60, 0x6C };
static UCHAR g_rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F,
                           0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };

static void mgmt_hdr(UCHAR *f, UCHAR fc, UCHAR *bssid)
{
   USHORT i;

   f[0] = fc;
   for (i = 0; i < 6; i++) {
      f[4 + i]  = bssid[i];                       /* DA = AP    */
      f[10 + i] = g_mymac[i];                     /* SA = us    */
      f[16 + i] = bssid[i];                       /* BSSID      */
   }
}

static int tx_auth(usb_dev_t *dev, UCHAR *bssid)
{
   static UCHAR f[32];
   USHORT i;

   for (i = 0; i < 32; i++) f[i] = 0;
   mgmt_hdr(f, 0xB0, bssid);
   f[26] = 1;                                     /* algorithm 0 (open), sequence 1, status 0 */
   return tx_mgmt(dev, f, 30, 0);
}

static int tx_deauth(usb_dev_t *dev, UCHAR *bssid)
{
   static UCHAR f[32];
   USHORT i;

   for (i = 0; i < 32; i++) f[i] = 0;
   mgmt_hdr(f, 0xC0, bssid);
   f[24] = 3;                                     /* reason 3: leaving */
   return tx_mgmt(dev, f, 26, 0);
}

static int tx_assoc(usb_dev_t *dev, UCHAR *bssid, char *ssid, UCHAR slen)
{
   static UCHAR f[112];
   USHORT i, n;

   for (i = 0; i < 112; i++) f[i] = 0;
   mgmt_hdr(f, 0x00, bssid);                      /* association request */
   n = 24;
   f[n++] = 0x31; f[n++] = 0x16;                  /* capability: ESS, privacy, short preamble/slot */
   f[n++] = 2;    f[n++] = 0;                     /* listen interval */
   f[n++] = 0;    f[n++] = slen;                  /* SSID */
   for (i = 0; i < slen; i++) f[n++] = (UCHAR)ssid[i];
   f[n++] = 1;    f[n++] = 8;
   for (i = 0; i < 8; i++) f[n++] = g_rt8[i];     /* supported rates */
   f[n++] = 50;   f[n++] = 4;
   for (i = 0; i < 4; i++) f[n++] = g_rt4[i];     /* extended rates */
   for (i = 0; i < 22; i++) f[n++] = g_rsn[i];    /* RSN: CCMP, CCMP, PSK */
   g_jreqlen = (USHORT)(n - 28);                  /* what the supplicant reads back as request IEs */
   for (i = 0; i < g_jreqlen; i++) g_jreq[i] = f[28 + i];
   return tx_mgmt(dev, f, n, 0);
}

/* The network to join: whatever SSID the supplicant / Wireless LAN Monitor asked for (OID SSID). */
static UCHAR g_want_ssid[33];
static UCHAR g_want_len;

void rtl8188eu_set_want_ssid(UCHAR FAR *s, UCHAR len)
{
   USHORT i;

   if (len > 32)
      len = 32;
   for (i = 0; i < len; i++)
      g_want_ssid[i] = s[i];
   g_want_ssid[len] = 0;
   g_want_len = len;
}

static int ssid_match(net_t *nt)
{
   USHORT i;

   if (!g_want_len || nt->len != g_want_len)
      return 0;
   for (i = 0; i < g_want_len; i++)
      if ((UCHAR)nt->ssid[i] != g_want_ssid[i])
         return 0;
   return 1;
}

/* ===== Join / leave / EAPOL, driven by the GenMAC facade (rtl_nic.c) ============
 * Joins the wanted SSID (rtl8188eu_set_want_ssid), picking the strongest-heard
 * BSS from the last scan (or exactly want_bssid when the supplicant named one). */
static void keys_reset(usb_dev_t *dev, UCHAR secure);

int rtl8188eu_join(usb_dev_t *dev, UCHAR *want_bssid)
{
   UCHAR  ep = find_bulk_in_ep(dev);
   USHORT k, best = 0xFFFF, bestcnt = 0, att, r, i;
   int    bestr = -128;

   g_joined = 0;
   g_eqh = g_eqt = g_eqn = 0;
   for (k = 0; k < 17; k++)                         /* fresh association: forget the duplicate-filter history */
      g_sc[k] = 0xFFFF;
   g_dg_tries++;
   g_ev_n = 0;
   g_ev_t0 = rtl8188eu_ms();
   g_dg_nnet = g_nnet;
   g_dg_wantlen = g_want_len;
   g_dg_bssf = (USHORT)(want_bssid ? 1 : 0);
   for (k = 0; k < g_nnet; k++) {
      if (!ssid_match(&g_net[k]))
         continue;
      if (want_bssid) {
         UCHAR same = 1;
         for (i = 0; i < 6; i++)
            if (g_net[k].bssid[i] != want_bssid[i]) { same = 0; break; }
         if (!same)
            continue;
      }
      /* strongest access point wins (unknown level counts as weak) */
      if (best == 0xFFFF || (g_net[k].rssi ? g_net[k].rssi : -120) > bestr) {
         best = k;
         bestr = g_net[k].rssi ? g_net[k].rssi : -120;
      }
   }
   tcom_always(10); tcom_always('J');            /* kept in release: join attempt */
   tcom(10); tcom_str("JOIN ");
   if (best == 0xFFFF) {
      tcom_str("no BSS with the wanted SSID in the scan table"); tcom(10);
      g_dg_reason = 1;
      return -1;
   }
   g_dg_ch = g_net[best].ch;
   g_dg_rssi = (USHORT)(-bestr);
   for (i = 0; i < 6; i++) g_target[i] = g_net[best].bssid[i];
   tcom_mac(g_target); tcom_str(" ch"); tcom_dec(g_net[best].ch);

   rtl8188eu_tx_prepare(dev);
   rf_tune20(dev, g_net[best].ch);
   g_curch = g_net[best].ch;
#if RUN_TXPOWER
   rtl8188eu_set_txpower(dev, g_net[best].ch);        /* per-rate power for this channel */
#endif
   usb_mdelay(40);
   g_rx_deauth = 0; g_auth_st = 0xFFFF; g_assoc_st = 0xFFFF; g_aid = 0;
   rtw_write32(dev, REG_RCR, RX_RCR | 0x1000UL);   /* ACF: let the AP's ACKs of our frames through (diagnostics) */

   for (att = 1; att <= 3 && g_assoc_st != 0; att++) {
      g_acc_reset = 1;
      for (r = 0; r < 2; r++)
         if (rx_read(dev, ep, 60) == 0)
            break;
      g_rx_auth = 0; g_rx_assoc = 0; g_auth_st = 0xFFFF; g_assoc_st = 0xFFFF;
      ev(1);
      tx_auth(dev, g_target);
      ev(3);
      for (r = 0; r < 40 && !g_rx_auth; r++)
         rx_read(dev, ep, 20);                       /* short steps: the association request follows the auth reply quickly */
      if (g_rx_auth && g_auth_st == 0) {
         ev(2);
         tx_assoc(dev, g_target, g_net[best].ssid, g_net[best].len);
         ev(3);
         for (r = 0; r < 40 && !g_rx_assoc; r++)
            rx_read(dev, ep, 20);
      }
      tcom_str(" a"); tcom_dec(att); tcom('=');
      if (g_auth_st == 0xFFFF) tcom_str("-"); else tcom_dec(g_auth_st);
      tcom('/');
      if (g_assoc_st == 0xFFFF) tcom_str("-"); else tcom_dec(g_assoc_st);
   }
   rtw_write32(dev, REG_RCR, RX_RCR);               /* control frames off again */
   g_dg_auth = g_auth_st;
   g_dg_assoc = g_assoc_st;
   if (g_assoc_st != 0) {
      tcom_str(" FAILED"); tcom(10);
      if (g_auth_st == 0xFFFF)       g_dg_reason = 2;      /* the AP never answered the authentication */
      else if (g_auth_st != 0)       g_dg_reason = 3;      /* the AP refused the authentication */
      else if (g_assoc_st == 0xFFFF) g_dg_reason = 4;      /* never answered the association request */
      else                           g_dg_reason = 5;      /* refused the association (status in JASSOC) */
      return -1;
   }
   g_dg_reason = 0;
   for (i = 0; i < 6; i++) g_jbssid[i] = g_target[i];
   for (i = 0; i < 33; i++) g_jssid[i] = (UCHAR)g_net[best].ssid[i];
   g_jslen   = g_net[best].len;
   g_jch     = g_net[best].ch;
   g_jaidv   = g_aid;
   g_jstatus = 0;
   g_joined  = 1;
   keys_reset(dev, 1);                            /* empty CAM, security on (no keys yet) */
   tcom_str(" OK aid="); tcom_dec(g_aid); tcom(10);
   return 0;
}

/* Debug "pr N L": send N directed probe requests (L padding bytes) to the BSS of the scan table that matches the wanted SSID
 * and count the probe responses addressed to us. Tests whether our transmissions reach the AP, independent of auth state. */
static USHORT g_pr_sent, g_pr_got;
void rtl8188eu_dbg_probe(USHORT count, USHORT pad)
{
   static UCHAR f[200];
   USHORT k, i, n, best = 0xFFFF, base;
   int    bestr = -1000, rv;

   for (k = 0; k < g_nnet; k++) {
      if (!ssid_match(&g_net[k]))
         continue;
      rv = g_net[k].rssi ? (int)g_net[k].rssi : -120;
      if (best == 0xFFFF || rv > bestr) {
         best = k;
         bestr = rv;
      }
   }
   g_pr_sent = 0; g_pr_got = 0;
   if (best == 0xFFFF || pad > 150)
      return;
   for (i = 0; i < 6; i++) g_target[i] = g_net[best].bssid[i];
   rtl8188eu_tx_prepare(&g_usbdev);
   rf_tune20(&g_usbdev, g_net[best].ch);
   g_curch = g_net[best].ch;
   usb_mdelay(40);
   base = g_prme;
   for (k = 0; k < count; k++) {
      for (i = 0; i < 200; i++) f[i] = 0;
      f[0] = 0x40;
      for (i = 0; i < 6; i++) {
         f[4 + i]  = g_target[i];                  /* DA = the AP */
         f[10 + i] = g_mymac[i];
         f[16 + i] = g_target[i];
      }
      n = 24;
      f[n++] = 0;  f[n++] = 0;                     /* wildcard SSID */
      f[n++] = 1;  f[n++] = 4;
      f[n++] = 0x82; f[n++] = 0x84; f[n++] = 0x8B; f[n++] = 0x96;
      if (pad) {
         f[n++] = 221; f[n++] = (UCHAR)pad;        /* vendor IE as padding to reach the wanted frame length */
         n = (USHORT)(n + pad);
      }
      if (tx_mgmt(&g_usbdev, f, n, 0) == 0)
         g_pr_sent++;
      usb_mdelay(60);
   }
   usb_mdelay(100);
   g_pr_got = (USHORT)(g_prme - base);
}

/* Note a failure that happens outside join (the OID layer refused the request). */
void rtl8188eu_diag_set(USHORT reason, USHORT a, USHORT b)
{
   g_dg_reason = reason;
   g_dg_auth = a;
   g_dg_assoc = b;
}

/* Diagnostic value number i (see the names in rtl_dhcp.c); 0xFFFF past the end. */
USHORT rtl8188eu_diag(USHORT i)
{
   if (i == 0)  return g_dg_reason;
   if (i == 1)  return g_dg_auth;
   if (i == 2)  return g_dg_assoc;
   if (i == 3)  return g_dg_ch;
   if (i == 4)  return g_dg_rssi;
   if (i == 5)  return g_dg_nnet;
   if (i == 6)  return g_dg_tries;
   if (i == 7)  return g_dg_wantlen;
   if (i == 8)  return g_dg_bssf;
   if (i == 9)  return g_nfrm;
   if (i == 10) return g_ncrc;
   if (i == 11) return g_ndesync;
   if (i == 12) return g_rx_errtot;
   if (i == 13) return g_dg_txok;
   if (i == 14) return g_dg_txfail;
   if (i == 15) return g_dg_txlast;
   if (i == 16) return g_tx_err;
   if (i == 17) return g_dg_feeds;
   if (i == 18) return g_dg_maxn;
   if (i == 19) return g_dg_full;
   if (i == 20) return g_dg_dsev;
   if (i == 21) return g_dg_multi;
   if (i == 22) return g_dg_pad8;
   if (i == 23) return g_dg_dsn;
   if (i >= 24 && i <= 29) return usb_ep_info((USHORT)(i - 24));
   if (i == 30) return g_dg_tome;
   if (i >= 31 && i <= 34) return usb_ep_info((USHORT)(i - 31 + 6));
   if (i >= 35 && i <= 39) return rtl8188eu_iqk_info((USHORT)(i - 35));
   if (i >= 46 && i <= 54) return usb_ep_info((USHORT)(i - 46 + 10));
   if (i >= 55 && i <= 59) return usb_ep_info((USHORT)(i - 55 + 60));
   if (i >= 60 && i <= 63) return ndis_stat((USHORT)(i - 60));
   if (i >= 64 && i <= 67) return usb_ep_info((USHORT)(i - 64 + 65));      /* TXBUSY TXSUB TXCMP TXSTAT */
   if (i == 68) return usb_ep_info(19);                                    /* RPNONE: calls that found no free request packet */
   if (i >= 69 && i <= 73) return ndis_stat((USHORT)(i - 69 + 4));
   if (i == 74) return usb_ep_info(69);                                    /* TXRECL */
   if (i == 75) return ndis_stat(9);                                       /* NDDROP */
   if (i >= 76 && i <= 78) return usb_ep_info((USHORT)(i - 76 + 70));       /* CNCRC STALE ACCLKF */         /* NDTX NDTXFULL NDTXFAIL TXLKF INDLKF */
   if (i >= 79 && i <= 81) return usb_ep_info((USHORT)(i - 79 + 20));       /* TXQRE CPUTX TXQTMO */
   if (i == 82) return ndis_diag(0);                                       /* INDTMO */
   if (i == 83) return ndis_diag(1);                                       /* INDRE */
   if (i == 84) return ndis_diag(3);                                       /* CPURX */
   if (i == 40) return g_dg_ack;
   if (i == 41) return g_dg_ctl;
   if (i == 42) return g_dg_tmn;
   if (i == 43) return g_ev_n;
   if (i == 44) return g_pr_sent;
   if (i == 45) return g_pr_got;
   return 0xFFFF;
}

/* A chip register read for the diagnostics (n 0..11; half 1 = high 16 bits, 0 = low 16 bits). */
USHORT rtl8188eu_diag_reg(USHORT n, USHORT half)
{
   static USHORT regs[22] = { 0x00F0, 0x0100, 0x0608, 0x0610, 0x0614, 0x0618, 0x0522, 0x0210, 0x0420, 0x010C, 0x0800, 0x0E08,
                              0x0C80, 0x0C94, 0x0C4C, 0x0C14, 0x0CA0, 0x0C50, 0x0E00, 0x0E04, 0x086C, 0x0E10 };
   static union { ULONG l; USHORT w[2]; } u;

   if (!g_hw_ready || n >= 22)
      return 0xFFFF;
   if (half)
      u.l = rtw_read32(&g_usbdev, regs[n]);
   return u.w[half ? 1 : 0];
}

/* Event log entry n: which 0 = kind, 1 = time (ms since the join started). */
USHORT rtl8188eu_diag_ev(USHORT n, USHORT which)
{
   return which == 1 ? g_ev_t[n] : (which == 2 ? (USHORT)g_ev_i[n] : (USHORT)g_ev_k[n]);
}

/* First frames addressed to us: n 0..2, i 0..7 (subtype byte, source MAC, length low byte). */
UCHAR rtl8188eu_diag_tm(USHORT n, USHORT i)
{
   return g_dg_tm[n * 8 + i];
}

/* Captured desync context: k 0..1, i 0..35 = bytes (12 before the bad spot + 24 after); 36..39 = prevpl, prevtot, off, accn. */
USHORT rtl8188eu_diag_ctx(USHORT k, USHORT i)
{
   if (i < 36)
      return g_dg_ctx[k * 36 + i];
   return g_dg_inf[k * 4 + (i - 36)];
}

void rtl8188eu_leave(usb_dev_t *dev)
{
   if (!g_joined)
      return;
   tx_deauth(dev, g_jbssid);
   usb_mdelay(20);
   tx_deauth(dev, g_jbssid);
   g_joined = 0;
   g_eqh = g_eqt = g_eqn = 0;
   g_dqh = g_dqt = g_dqn = 0;
   keys_reset(dev, 0);
   tcom_always('L'); tcom_always(10);              /* kept in release: leave */
   tcom_str("LEAVE"); tcom(10);
}

/* One short read of the bulk-IN stream while joined: collects EAPOL frames, notices a
 * deauthentication from the AP. Returns the number of EAPOL frames waiting. */
/* ===== LED ===================================================================
 * Register 0x4E drives the LED in software mode: 0x20 = on, 0x28 = off (verified on the
 * dongle; the Windows driver toggles the same two values). There is no timer thread, so the
 * state machine is stepped from the GenMAC request path (monitor ~1/s, supplicant much more
 * often) under the request lock; a transfer here is a normal register write.
 * Searching: 500 ms on / 500 ms off. Joined: steady on, flickering off for 60 ms after data. */
static UCHAR  g_led_on = 2;                      /* 0 off, 1 on, 2 not written yet */
static ULONG  g_led_next;
static ULONG FAR *g_led_msp;                     /* -> GINFOSEG.msecs (ms since boot) */

static ULONG led_now(void)
{
   static PVOID pv;                              /* static: address passed to DevHelp (SS != DS) */
   union { PVOID p; USHORT w[2]; } u;

   if (!g_led_msp) {
      if (rt_dh_getdosvar(DOSVAR_SYSINFOSEG, 0, (PPVOID)&pv))
         return 0;
      u.p = pv;
      u.w[0] = 4;                                /* offset of msecs in the global info segment  */
      u.w[1] = *(USHORT FAR *)pv;                /* GetDOSVar returns a pointer to the selector */
      g_led_msp = (ULONG FAR *)u.p;
   }
   return *g_led_msp;
}

ULONG rtl8188eu_ms(void)                         /* ms since boot (0 if the clock is unavailable) */
{
   return led_now();
}

static void led_set(usb_dev_t *dev, UCHAR on)
{
   if (g_led_on == on)
      return;
   g_led_on = on;
   rtw_write8(dev, 0x004E, (UCHAR)(on ? 0x20 : 0x28));
}

static UCHAR g_led_dead;                         /* set at shutdown: the LED stays off */

void rtl8188eu_led_off(usb_dev_t *dev)
{
   g_led_dead = 1;
   led_set(dev, 0);
}

void rtl8188eu_led_tick(usb_dev_t *dev)
{
#if RUN_LED
   ULONG now = led_now();
   int   due;

   if (!now || g_led_dead)
      return;
   due = ((long)(now - g_led_next) >= 0L);
   if (!g_joined) {                              /* searching: slow blink */
      g_led_act = 0;
      if (due || g_led_on == 2) {
         led_set(dev, (UCHAR)(g_led_on == 1 ? 0 : 1));
         g_led_next = now + 500UL;
      }
      return;
   }
   if (g_led_on != 1 && (g_led_on == 2 || due)) { /* joined: steady on */
      led_set(dev, 1);
      g_led_next = now + 150UL;
      return;
   }
   if (g_led_act && due) {                       /* traffic: short flicker */
      g_led_act = 0;
      led_set(dev, 0);
      g_led_next = now + 60UL;
   }
#endif
}

USHORT rtl8188eu_poll(usb_dev_t *dev, USHORT tmo_ms)
{
   if (!g_joined)
      return 0;
   if (g_rx_deauth) {
      g_joined = 0;
      tcom_str("[deauth by AP reason+1="); tcom_dec(g_rx_deauth); tcom(']');
      g_rx_deauth = 0;
   }
   return g_eqn;
}

/* Pop the oldest received EAPOL frame (Ethernet format) into the caller's buffer. */
USHORT rtl8188eu_eapol_get(UCHAR FAR *out, USHORT room)
{
   USHORT n, i;

   if (!g_eqn)
      return 0;
   n = g_eqlen[g_eqh];
   if (n <= room)
      for (i = 0; i < n; i++)
         out[i] = g_eq[g_eqh][i];
   else
      n = 0;
   g_eqh = (USHORT)((g_eqh + 1) % EQN);
   rt_atomic_dec(&g_eqn);
   return n;
}

/* Send an Ethernet-format EAPOL frame to the AP as an unencrypted 802.11 data frame. */
/* ===== Keys (chip CAM) ===========================================================
 * CAM entry layout and register protocol from the stock driver (write_cam): six
 * 32-bit words per entry {ctrl|mac0,mac1 / mac2..5 / key[0..15]}, written highest
 * word first through CAMWRITE (0x674) + CAMCMD (0x670, bit31 poll | bit16 write | addr).
 * Pairwise key = entry 4 (station mode) with the AP's address; group keys sit in the
 * default-key entries 0..3 indexed by key id. ctrl = valid(bit15) | algo(AES=4)<<2 | keyid.
 * SECCFG 0xCC = TX encrypt + RX decrypt, unicast looked up by address, broadcast by
 * default key (what the stock driver uses for 802.1X/WPA). */
static USHORT g_pn[3];                            /* TX packet number: PN1:PN0, PN3:PN2, PN5:PN4 */
static UCHAR  g_zero16[16], g_nomac[6];

static void cam_write(usb_dev_t *dev, UCHAR entry, USHORT ctrl, UCHAR *mac, UCHAR *key)
{
   union { ULONG l; UCHAR b[4]; } v;
   union { ULONG l; USHORT w[2]; } c;
   int    j;
   USHORT i, k;

   for (j = 5; j >= 0; j--) {
      if (j == 0) {
         v.b[0] = (UCHAR)(ctrl & 0xFF); v.b[1] = (UCHAR)(ctrl >> 8);
         v.b[2] = mac[0];               v.b[3] = mac[1];
      } else if (j == 1) {
         for (k = 0; k < 4; k++) v.b[k] = mac[2 + k];
      } else {
         i = (USHORT)((j - 2) * 4);
         for (k = 0; k < 4; k++) v.b[k] = key[i + k];
      }
      rtw_write32(dev, 0x0674, v.l);
      c.w[0] = (USHORT)(((USHORT)entry << 3) + (USHORT)j);
      c.w[1] = 0x8001;                            /* poll | write */
      rtw_write32(dev, 0x0670, c.l);
   }
}

/* The packet number belongs to the TX queue lock (see rtl8188eu_ndis_tx_slot): TransmitChain may be using it on another CPU. */
static void pn_reset(void)
{
   USHORT fl = usb_txq_lock();

   g_pn[0] = 1; g_pn[1] = g_pn[2] = 0;
   if (fl != 0xFFFF)
      usb_txq_unlock(fl);
}

static void keys_reset(usb_dev_t *dev, UCHAR secure)
{
   rtw_write32(dev, 0x0670, 0xC0000000UL);        /* invalidate the whole CAM */
   rtw_write8(dev, 0x0680, (UCHAR)(secure ? 0xCC : 0x00));
   g_ptk_ok = g_gtk_ok = 0;
   pn_reset();
}

/* The dongle went away (or is being re-initialised): forget the link and everything bound to the
 * old radio state. Runs in USBD's callback context: assignments only, nothing may block. */
void rtl8188eu_on_detach(void)
{
   g_joined = 0;
   g_ptk_ok = g_gtk_ok = 0;
   g_eqh = g_eqt = g_eqn = 0;
   g_dqh = g_dqt = g_dqn = 0;
   g_acc_reset = 1;
   g_nnet = 0;
   g_rx_deauth = 0;
   g_led_on = 2;                                /* LED state unknown: rewritten after the next init */
}

int rtl8188eu_has_ptk(void)
{
   return g_joined && g_ptk_ok;
}

static UCHAR g_ptk_last[16];

int rtl8188eu_key_add(usb_dev_t *dev, UCHAR idx, UCHAR pairwise, UCHAR *key)
{
   if (!g_joined)
      return -1;
   if (pairwise) {
      USHORT q, same = 1;

      for (q = 0; q < 16; q++)
         if (g_ptk_last[q] != key[q]) { same = 0; break; }
      if (g_ptk_ok && same) {
         /* The supplicant installs the key it already installed (an access point that repeats message 3 of the
          * handshake). Writing it again would restart our packet number at 1, and the access point - whose replay
          * counter for this key is already higher - would drop every frame until the numbers caught up (the link
          * "stalls": ping gets no answer although our frames are acknowledged). Keep the key and the numbering. */
         tcom_str("[key P dup]");
         return 0;
      }
      cam_write(dev, 4, (USHORT)(0x8000 | (4 << 2)), g_jbssid, key);
      pn_reset();                                  /* new key: numbering starts again, before the key is used */
      for (q = 0; q < 16; q++) g_ptk_last[q] = key[q];
      g_ptk_ok = 1;
   } else {
      idx = (UCHAR)(idx & 3);
      cam_write(dev, idx, (USHORT)(0x8000 | (4 << 2) | idx), g_nomac, key);
      g_gtk_ok = 1;
   }
   rtw_write8(dev, 0x0680, 0xCC);
   tcom_str("[key "); tcom(pairwise ? 'P' : 'G'); tcom_dec(idx); tcom(']');
   return 0;
}

void rtl8188eu_key_remove(usb_dev_t *dev, UCHAR idx, UCHAR pairwise)
{
   if (pairwise) {
      cam_write(dev, 4, 0, g_nomac, g_zero16);
      g_ptk_ok = 0;
   } else {
      cam_write(dev, (UCHAR)(idx & 3), 0, g_nomac, g_zero16);
   }
   tcom_str("[key- "); tcom(pairwise ? 'P' : 'G'); tcom_dec(idx); tcom(']');
}

/* ===== Ethernet frame -> 802.11 data frame to the AP ==============================
 * ToDS: A1 = BSSID, A2 = us, A3 = destination. With the pairwise key installed the frame
 * is sent protected: CCMP header (PN, key id 0) after the 802.11 header, hardware adds
 * the MIC and encrypts. Without a key (first handshake frames) it goes out plain on the
 * proven management queue. */
static UCHAR g_ethtx[640];

static UCHAR g_eapol_plain;                       /* set by rtl8188eu_eapol_tx: send this frame unprotected */

int rtl8188eu_eth_tx(usb_dev_t *dev, UCHAR FAR *eth, USHORT len)
{
   static UCHAR f[760];
   static UCHAR llc[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
   USHORT i, n;
   int    enc = g_ptk_ok && !g_eapol_plain;

   g_eapol_plain = 0;
   if (!g_joined || len < 15 || len > sizeof(g_ethtx))
      return -1;
   for (i = 0; i < len; i++)
      g_ethtx[i] = eth[i];
   if (enc) {
      /* Protected: the NDIS data path, so this frame's packet number is taken in the same locked step as its
       * submission and cannot overtake (or be overtaken by) a TransmitChain frame on another CPU. */
      USHORT w;
      int    rc = rtl8188eu_ndis_tx(g_ethtx, len);

      for (w = 0; w < 100 && g_tx_busy; w++)
         usb_mdelay(1);
      return rc;
   }
   for (i = 0; i < 40; i++) f[i] = 0;
   f[0] = 0x08;                                    /* data frame            */
   f[1] = 0x01;                                    /* ToDS                  */
   for (i = 0; i < 6; i++) {
      f[4 + i]  = g_jbssid[i];                     /* A1 = BSSID (AP)       */
      f[10 + i] = g_mymac[i];                      /* A2 = us               */
      f[16 + i] = g_ethtx[i];                      /* A3 = destination      */
   }
   n = 24;
   for (i = 0; i < 6; i++) f[n++] = llc[i];
   f[n++] = g_ethtx[12]; f[n++] = g_ethtx[13];     /* ethertype             */
   for (i = 14; i < len; i++) f[n++] = g_ethtx[i];
   return tx_raw(dev, f, n, 0, 0, 0);              /* plain: the proven management queue, as before */
}

/* NDIS transmit path: an Ethernet frame from the protocol stack -> encrypted 802.11 data
 * frame in a TX slot, submitted asynchronously (never blocks: TransmitChain may be called
 * from the protocol's receive-indication context). Returns 0 queued, -1 dropped (no
 * connection/keys yet or bad size), -2 no free slot. */
/* ===== Rate adaptation =========================================================
 * Chosen from the smoothed signal strength of the joined access point, with 3 dB of
 * hysteresis. The ladder is capped at 36 Mbit/s: measured on this dongle, 48 and 54 Mbit/s
 * were the worst rates at every distance tried (sending 30-100 KB/s versus 300 KB/s at
 * 18-36 Mbit/s), so they are never used. Steps (hardware rate codes) and the minimum
 * signal needed for each: 36M -55 dBm, 24M -65, 18M -72, 12M -77, 6M -82, CCK 11M -86,
 * CCK 1M below. ARP, DHCP and handshake frames always use 1 Mbit/s (choose_rate). */
UCHAR g_txrate_auto = 1;                          /* 1 = adapt; 'rate N' on the debug channel pins a rate */
static UCHAR       g_ra_rate[7] = { 9, 8, 7, 6, 4, 3, 0 };
static signed char g_ra_thr[6]  = { -55, -65, -72, -77, -82, -86 };
static UCHAR       g_ra_idx = 1;                  /* current step (starts at 24M)   */
static USHORT      g_ra_cnt;

static UCHAR adapt_rate(void)
{
   signed char r;
   UCHAR       t;

   if (!g_txrate_auto)
      return g_txrate;
   if ((g_ra_cnt++ & 31) == 0) {                  /* re-evaluate every 32 frames    */
      r = rtl8188eu_rssi_of(g_jbssid);
      if (r != 0) {
         for (t = 0; t < 6; t++)                  /* fastest step whose threshold the signal meets */
            if (r >= g_ra_thr[t])
               break;
         if (t < g_ra_idx) {                      /* faster: needs 3 dB of margin   */
            if (r >= (signed char)(g_ra_thr[t] + 3))
               g_ra_idx = t;
         } else if (t > g_ra_idx) {               /* slower: only once clearly below */
            if (r < (signed char)(g_ra_thr[g_ra_idx] - 3))
               g_ra_idx = t;
         }
      }
   }
   return g_ra_rate[g_ra_idx];
}

/* ARP, EAPOL and DHCP go out at 1 Mbit/s (robust, as the stock driver does); other data at g_txrate. */
static UCHAR choose_rate(UCHAR *eth, USHORT len)
{
   if (eth[12] == 0x08 && eth[13] == 0x06) return g_ctlrate;                      /* ARP  */
   if (eth[12] == 0x88 && eth[13] == 0x8E) return g_ctlrate;                      /* EAPOL */
   if (len > 38 && eth[12] == 0x08 && eth[13] == 0x00 && eth[23] == 17 &&
       ((eth[34] == 0 && (eth[35] == 67 || eth[35] == 68)) ||
        (eth[36] == 0 && (eth[37] == 67 || eth[37] == 68))))
      return g_ctlrate;                                                            /* DHCP */
   return adapt_rate();
}

int rtl8188eu_tx_ready(void)
{
   return g_joined && g_ptk_ok;
}

/* A free TX slot for an outgoing NDIS frame; waits briefly for one (a slot frees within a few ms). 0xFFFF = none. */
USHORT rtl8188eu_ndis_slot(void)
{
   USHORT slot = usb_tx_alloc();

   if (slot == 0xFFFF) {
      ULONG  t0 = rtl8188eu_ms();
      USHORT spin;

      for (spin = 0; spin < 30000U && slot == 0xFFFF; spin++) {
         if ((spin & 255) == 255 && (long)(rtl8188eu_ms() - t0) > 6L)
            break;
         slot = usb_tx_alloc();
      }
   }
   return slot;
}

/* The Ethernet frame (len bytes) is in the slot at TX_ETH_OFF; turn it into a protected 802.11 data frame in place
 * and queue it. The TX descriptor (48) + 802.11 (24) + CCMP (8) + LLC/SNAP (8) headers end exactly where the Ethernet
 * payload already is, so only the 74 header bytes are written. The packet number is taken under the TX queue lock
 * together with the submission: on an SMP kernel TransmitChain runs on several CPUs at once, and an unlocked "++g_pn"
 * could hand out one number twice or let a higher number reach the dongle first (the access point drops both as
 * replays). Returns 0 queued, -1 dropped (no connection/keys, bad size), -2 not queued now (retry). The slot is
 * released on failure. */
int rtl8188eu_ndis_tx_slot(USHORT slot, USHORT len)
{
   static UCHAR llc[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };
   USHORT i, n, fl;
   UCHAR  *b, *f, *eth, rate, et0, et1;
   UCHAR  da[6];
   int    rc;

   b   = usb_tx_buf(slot);
   eth = b + TX_ETH_OFF;
   if (!g_joined || !g_ptk_ok || len < 14 || len > MAX_FRAME_SIZE) {
      usb_tx_release(slot);
      return -1;
   }
   g_led_act = 1;
   f   = b + 48;
   rate = choose_rate(eth, len);                   /* reads the Ethernet header: before it is overwritten */
   for (i = 0; i < 6; i++) da[i] = eth[i];
   et0 = eth[12]; et1 = eth[13];
   for (i = 0; i < 40; i++) f[i] = 0;
   f[0] = 0x08;                                    /* data frame            */
   f[1] = 0x41;                                    /* ToDS | Protected      */
   for (i = 0; i < 6; i++) {
      f[4 + i]  = g_jbssid[i];                     /* A1 = BSSID (AP)       */
      f[10 + i] = g_mymac[i];                      /* A2 = us               */
      f[16 + i] = da[i];                           /* A3 = destination      */
   }
   f[27] = 0x20;                                   /* CCMP: ExtIV, key id 0 (PN bytes are filled in under the lock) */
   for (i = 0; i < 6; i++) f[32 + i] = llc[i];
   f[38] = et0; f[39] = et1;                       /* ethertype; the payload follows in place */
   n = (USHORT)(40 + len - 14);
   txd_fill(b, n, 0, 1, 1, rate);                  /* the descriptor checksum does not cover the frame */

   fl = usb_txq_lock();
   if (fl == 0xFFFF) {                             /* this CPU already holds it (re-entered) or the wait ran out: retry later */
      usb_tx_release(slot);
      return -2;
   }
   f[24] = (UCHAR)(g_pn[0] & 0xFF); f[25] = (UCHAR)(g_pn[0] >> 8);
   f[28] = (UCHAR)(g_pn[1] & 0xFF); f[29] = (UCHAR)(g_pn[1] >> 8);
   f[30] = (UCHAR)(g_pn[2] & 0xFF); f[31] = (UCHAR)(g_pn[2] >> 8);
   if (++g_pn[0] == 0)
      if (++g_pn[1] == 0)
         g_pn[2]++;
   rc = usb_tx_submit_locked(&g_usbdev, 3, slot, (USHORT)(48 + n));
   usb_txq_unlock(fl);
   return rc;
}

/* Same for a frame in driver memory (the DHCP client, protected EAPOL replies). */
int rtl8188eu_ndis_tx(UCHAR *eth, USHORT len)
{
   USHORT slot, i;
   UCHAR  *d;

   if (!g_joined || !g_ptk_ok || len < 14 || len > MAX_FRAME_SIZE)
      return -1;
   slot = rtl8188eu_ndis_slot();
   if (slot == 0xFFFF)
      return -2;
   d = usb_tx_buf(slot) + TX_ETH_OFF;
   for (i = 0; i < len; i++)
      d[i] = eth[i];
   return rtl8188eu_ndis_tx_slot(slot, len);
}

int rtl8188eu_eapol_tx(usb_dev_t *dev, UCHAR FAR *eth, USHORT len)
{
   tcom_str("[eapol tx "); tcom_dec((USHORT)(len - 14)); tcom(']');
   /* The 4-way handshake replies (EAPOL-Key with the Pairwise bit set: messages 2 and 4) are sent
    * UNPROTECTED even when the pairwise key is already installed here: if the AP asks again because it
    * missed our message 4, it has not installed the key yet and cannot read a protected reply. Group-key
    * handshake replies (Pairwise bit clear) are protected as usual. */
   g_eapol_plain = (UCHAR)(len >= 21 && eth[12] == 0x88 && eth[13] == 0x8E && eth[15] == 3 && (eth[20] & 0x08));
   return rtl8188eu_eth_tx(dev, eth, len);
}

/* Diagnostic: one-line counter dump on COM1 (RX data frames seen / encrypted / not decrypted
 * by the chip / dropped, raw frames parsed, USB error counters, engine state). */
void rtl8188eu_dbg(void)
{
   tcom_str("[d="); tcom_dec(g_nd); tcom_str(" p="); tcom_dec(g_np);
   tcom_str(" sw="); tcom_dec(g_nsw); tcom_str(" dr="); tcom_dec(g_ndrop);
   tcom_str(" f="); tcom_dec(g_nfrm); tcom_str(" re="); tcom_dec(g_rx_errtot);
   tcom_str(" te="); tcom_dec(g_tx_err); tcom_str(" run="); tcom_dec(g_rx_run);
   tcom(']');
}

/* dBm of the access point with this BSSID as last heard (smoothed), 0 if unknown. */
signed char rtl8188eu_rssi_of(UCHAR *bssid)
{
   USHORT k, i;
   UCHAR  same;

   for (k = 0; k < g_nnet; k++) {
      same = 1;
      for (i = 0; i < 6; i++)
         if (g_net[k].bssid[i] != bssid[i]) { same = 0; break; }
      if (same)
         return g_net[k].rssi;
   }
   return 0;
}

/* Debug: one line per known network (channel, signal, beacons, SSID) on COM1; '*' marks the joined AP. */
void rtl8188eu_print_nets(void)
{
   USHORT k, i;
   UCHAR  same;
   int    r;

   tcom('N');
   for (k = 0; k < g_nnet; k++) {
      same = 1;
      for (i = 0; i < 6; i++)
         if (g_net[k].bssid[i] != g_jbssid[i]) { same = 0; break; }
      tcom(' ');
      tcom((char)((g_joined && same) ? '*' : '#'));
      tcom_h8(g_net[k].bssid[4]); tcom_h8(g_net[k].bssid[5]);
      tcom(':'); tcom_dec(g_net[k].ch);
      r = g_net[k].rssi;
      tcom(r < 0 ? '-' : '+'); tcom_dec((USHORT)(r < 0 ? -r : r));
      tcom('x'); tcom_dec(g_net[k].cnt);
      tcom('='); tcom_str(g_net[k].len ? g_net[k].ssid : "?");
   }
}

/* Debug: the data rate code the next frame would use (adapting or pinned). */
UCHAR rtl8188eu_cur_rate(void)
{
   return g_txrate_auto ? g_ra_rate[g_ra_idx] : g_txrate;
}
