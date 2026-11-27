/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtw_sta.c - the station operations of wlan/hw.h for the RTL8821AE: scan, join, leave, management and data frames,
 * keys, transmit rates, link supervision.
 *
 * The RTL8821A is a "soft MAC": unlike the Intel firmware it has no scan or connection logic of its own, so this is
 * what the RTL8821AU driver does in rtl8821au/src/rtl_rx.c (rtl8188eu_scan_once, rtl8188eu_join, key_add, cam_write,
 * adapt_rate, choose_rate) reduced to the hardware steps; the 802.11 state machine itself (authentication,
 * association, EAPOL, Block Ack, Ethernet conversion) is wlan/sta.c.
 *
 * Keys (cam_write, from the stock driver's write_cam): six 32-bit words per CAM entry {ctrl|mac0,mac1 / mac2..5 /
 * key[0..15]}, written highest word first through CAMWRITE + CAMCMD (bit 31 poll | bit 16 write | entry << 3 + word).
 * Pairwise key = entry 4 with the AP's address; group keys = the default-key entries 0..3 by key id.
 * ctrl = valid (bit 15) | algorithm AES (4) << 2 | key id. SECCFG 0xCC = TX encrypt + RX decrypt, unicast looked up by
 * address, broadcast by default key.
 */
#include "drv.h"
#include "rtw.h"

UCHAR  g_curch = 1;
UCHAR  g_ctlrate;                           /* handshake / ARP / DHCP frames: 0 = the band's lowest rate           */
UCHAR  g_txrate = 8, g_txrate_auto = 1;
USHORT g_keys_p, g_keys_g, g_keys_dup, g_scan_n;
UCHAR  g_hw_nss = 1;                        /* one spatial stream (1T1R)                                         */

static USHORT g_pn[3];                      /* TX packet number: PN1:PN0, PN3:PN2, PN5:PN4                        */
static UCHAR  g_ptk_last[16];
static UCHAR  g_ptk_ok;
static UCHAR  g_zero16[16];
static ULONG  g_next_chk;

/* ---- rates ---------------------------------------------------------------------------------------------------
 * From the smoothed signal strength of the joined AP, 3 dB of hysteresis, capped at 36 Mbit/s (rtl8821au/src/rtl_rx.c
 * adapt_rate: 48 and 54 Mbit/s measured worst on the dongle). Steps (hardware rate codes) and the signal they need:
 * 36M -55 dBm, 24M -65, 18M -72, 12M -77, 6M -82, CCK 11M -86, CCK 1M below (5 GHz stops at 6M). */
static const UCHAR       g_ra_rate[7] = { 9, 8, 7, 6, 4, 3, 0 };
static const signed char g_ra_thr[6]  = { -55, -65, -72, -77, -82, -86 };
static UCHAR  g_ra_idx = 1;
static USHORT g_ra_cnt;

static UCHAR adapt_rate(void)
{
   short r;
   UCHAR t;

   if (!g_txrate_auto)
      return g_txrate;
   if ((g_ra_cnt++ & 31) == 0) {             /* re-evaluate every 32 frames                                     */
      r = sta_rssi();
      for (t = 0; t < 6; t++)
         if (r >= g_ra_thr[t])
            break;
      if (t < g_ra_idx) {
         if (r >= g_ra_thr[t] + 3)
            g_ra_idx = t;
      } else if (t > g_ra_idx) {
         if (r < g_ra_thr[g_ra_idx] - 3)
            g_ra_idx = t;
      }
   }
   if (g_curch > 14 && g_ra_idx > 4)
      return RATE_6M;
   return g_ra_rate[g_ra_idx];
}

static UCHAR ctl_rate(void)
{
   if (g_ctlrate)
      return g_ctlrate;
   return (UCHAR)(g_curch > 14 ? RATE_6M : RATE_1M);
}

/* ARP, EAPOL and DHCP go at the robust rate (as the stock driver does), the rest at the adapted rate.
 * body: LLC/SNAP header + payload. */
UCHAR rtw_rate_for(UCHAR FAR *b, USHORT bl, USHORT flags)
{
   if (flags & HWTX_EAPOL)
      return ctl_rate();
   if (bl >= 8 && b[0] == 0xAA && b[1] == 0xAA) {
      if (b[6] == 0x08 && b[7] == 0x06) return ctl_rate();                       /* ARP                    */
      if (b[6] == 0x88 && b[7] == 0x8E) return ctl_rate();                       /* EAPOL                  */
      if (bl > 8 + 24 && b[6] == 0x08 && b[7] == 0x00 && b[8 + 9] == 17 &&
          ((b[8 + 20] == 0 && (b[8 + 21] == 67 || b[8 + 21] == 68)) ||
           (b[8 + 22] == 0 && (b[8 + 23] == 67 || b[8 + 23] == 68))))
         return ctl_rate();                                                       /* DHCP                   */
   }
   return adapt_rate();
}

/* ---- keys ------------------------------------------------------------------------------------------------------ */
static void cam_write(UCHAR entry, USHORT ctrl, UCHAR FAR *mac, UCHAR FAR *key)
{
   U32    v, c;
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
      rtw_w32(REG_CAMWRITE, v.l);
      c.w[0] = (USHORT)(((USHORT)entry << 3) + (USHORT)j);
      c.w[1] = 0x8001;                       /* poll | write                                                     */
      rtw_w32(REG_CAMCMD, c.l);
      for (k = 0; k < 100 && (rtw_r32(REG_CAMCMD) & 0x80000000UL); k++)
         dd_udelay(2);
   }
}

static void pn_reset(void)
{
   USHORT fl = rt_irq_save();

   g_pn[0] = 1; g_pn[1] = g_pn[2] = 0;
   rt_irq_restore(fl);
}

void rtw_keys_reset(UCHAR secure)
{
   rtw_w32(REG_CAMCMD, 0xC0000000UL);        /* invalidate the whole CAM                                          */
   rtw_w8(REG_SECCFG, (UCHAR)(secure ? 0xCC : 0x00));
   g_ptk_ok = 0;
   fmemset((UCHAR FAR *)g_ptk_last, 0, 16);
   pn_reset();
}

int hw_key_set(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   static UCHAR k[16], nomac[6];
   USHORT i;

   if (klen != 16)
      return -1;
   fmemcpy((UCHAR FAR *)k, key, 16);
   if (pairwise) {
      g_keys_p++;
      if (g_ptk_ok && fmemeq((UCHAR FAR *)g_ptk_last, (UCHAR FAR *)k, 16)) {
         /* The supplicant installs the key it already installed (an AP that repeats message 3): writing it again
          * would restart the packet numbers at 1 and the AP would drop every frame (rtl8821au, key_add). */
         g_keys_dup++;
         return 0;
      }
      cam_write(4, (USHORT)(0x8000 | (4 << 2)), (UCHAR FAR *)g_jbssid, (UCHAR FAR *)k);
      pn_reset();
      for (i = 0; i < 16; i++) g_ptk_last[i] = k[i];
      g_ptk_ok = 1;
   } else {
      g_keys_g++;
      idx = (UCHAR)(idx & 3);
      cam_write(idx, (USHORT)(0x8000 | (4 << 2) | idx), (UCHAR FAR *)nomac, (UCHAR FAR *)k);
   }
   rtw_w8(REG_SECCFG, 0xCC);
   return 0;
}

int hw_key_clear(USHORT pairwise, UCHAR idx)
{
   static UCHAR nomac[6];

   if (pairwise) {
      cam_write(4, 0, (UCHAR FAR *)nomac, (UCHAR FAR *)g_zero16);
      g_ptk_ok = 0;
   } else
      cam_write((UCHAR)(idx & 3), 0, (UCHAR FAR *)nomac, (UCHAR FAR *)g_zero16);
   return 0;
}

/* ---- frames ------------------------------------------------------------------------------------------------------ */
int hw_mgmt_tx(UCHAR FAR *f, USHORT len)
{
   USHORT fl = TXF_RPT;

   if (len < 24)
      return -1;
   if (f[4] & 1)
      fl |= TXF_BCAST;
   return rtw_tx_frame(QSLT_MGNT, f, 24, 0, f + 24, (USHORT)(len - 24), ctl_rate(), fl) ? -1 : 0;
}

/* Runs under the station's transmit lock with interrupts off (wlan/sta.c tx_eth), so the packet number and the ring
 * entry are taken in one step and frames cannot overtake each other. */
int hw_data_tx(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT flags)
{
   static UCHAR iv[8];
   USHORT tf = 0;
   UCHAR  rate = rtw_rate_for(body, blen, flags);
   int    rc;

   if (hdr[4] & 1)
      tf |= TXF_BCAST;
   if (flags & HWTX_PROTECT) {
      iv[0] = (UCHAR)g_pn[0];        iv[1] = (UCHAR)(g_pn[0] >> 8);
      iv[2] = 0;                     iv[3] = 0x20;                  /* ExtIV, key id 0                      */
      iv[4] = (UCHAR)g_pn[1];        iv[5] = (UCHAR)(g_pn[1] >> 8);
      iv[6] = (UCHAR)g_pn[2];        iv[7] = (UCHAR)(g_pn[2] >> 8);
      rc = rtw_tx_frame(QSLT_BE, hdr, hlen, (UCHAR FAR *)iv, body, blen, rate, (USHORT)(tf | TXF_SEC));
      if (rc == 0 && ++g_pn[0] == 0 && ++g_pn[1] == 0)
         g_pn[2]++;
   } else
      rc = rtw_tx_frame(QSLT_BE, hdr, hlen, 0, body, blen, rate, tf);
   return rc;
}

/* ---- scan ----------------------------------------------------------------------------------------------------------
 * The channels of the USB driver plus the 5 GHz DFS channels (heard passively). An active probe (wildcard SSID) on the
 * others; 110 ms on each, long enough for a beacon at the usual 102.4 ms interval. Beacons and probe responses reach
 * sta_beacon() from the receive path. While joined, the radio stays on the AP's channel (the table is kept fresh by
 * its beacons): leaving it would drop the link. */
static const UCHAR g_chlist[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                                  36, 40, 44, 48, 52, 56, 60, 64,
                                  100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140,
                                  149, 153, 157, 161, 165 };

static int ch_passive(UCHAR ch)
{
   return ch >= 52 && ch <= 144;
}

static void tx_probe(UCHAR ch)
{
   static UCHAR f[64];
   static const UCHAR r24[8] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
   static const UCHAR x24[4] = { 0x30, 0x48, 0x60, 0x6C };
   static const UCHAR r5[8]  = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
   static USHORT seq;
   USHORT i, n;

   fmemset((UCHAR FAR *)f, 0, sizeof(f));
   f[0] = 0x40;                                          /* probe request                                    */
   for (i = 0; i < 6; i++) {
      f[4 + i]  = 0xFF;
      f[10 + i] = g_mymac[i];
      f[16 + i] = 0xFF;
   }
   seq = (USHORT)((seq + 1) & 0x0FFF);
   f[22] = (UCHAR)(seq << 4);
   f[23] = (UCHAR)(seq >> 4);
   n = 24;
   f[n++] = 0; f[n++] = 0;                               /* SSID: wildcard                                   */
   f[n++] = 1; f[n++] = 8;
   for (i = 0; i < 8; i++) f[n++] = ch > 14 ? r5[i] : r24[i];
   if (ch <= 14) {
      f[n++] = 50; f[n++] = 4;
      for (i = 0; i < 4; i++) f[n++] = x24[i];
      f[n++] = 3; f[n++] = 1; f[n++] = ch;               /* DS parameter set                                 */
   }
   hw_mgmt_tx((UCHAR FAR *)f, n);
}

int hw_scan(void)
{
   USHORT i;
   UCHAR  ch;

   if (!g_hw_ready)
      return -1;
   g_scan_n++;
   if (g_joined)
      return 0;
   rtw_dig_set(0x20);                                    /* the most sensitive receiver while scanning       */
   rtw_w32(REG_RCR, RCR_SCAN);
   for (i = 0; i < sizeof(g_chlist); i++) {
      ch = g_chlist[i];
      rtw_set_channel(ch);
      g_curch = ch;
      hw_service();                                     /* frames still queued from the previous channel    */
      if (!ch_passive(ch)) {
         dd_msleep(5);
         tx_probe(ch);
      }
      {
         ULONG t0 = dd_ms();

         while ((long)(dd_ms() - t0) < 110L) {
            hw_service();
            dd_msleep(4);
         }
      }
   }
   sta_age();
   return 0;
}

/* ---- join --------------------------------------------------------------------------------------------------------- */
int hw_chan_ht40(UCHAR ch)  { (void)ch; return 0; }      /* 20 MHz only (as the RTL8821AU driver)             */
int hw_chan_vht80(UCHAR ch) { (void)ch; return 0; }

int hw_join_prepare(struct net_s *n)
{
   USHORT i;

   if (!g_hw_ready)
      return -1;
   for (i = 0; i < 6; i++)
      rtw_w8(REG_BSSID + i, 0);
   rtw_w32(REG_RCR, RCR_SCAN);
   rtw_keys_reset(0);
   rtw_set_channel(n->ch);
   g_curch = n->ch;
   rtw_iqk(n->ch);                                       /* firmware IQ calibration on the AP's channel      */
   g_ra_idx = 1;
   g_ra_cnt = 0;
   dd_msleep(10);
   return 0;
}

int hw_join_complete(void)
{
   USHORT i;

   for (i = 0; i < 6; i++)
      rtw_w8(REG_BSSID + i, g_jbssid[i]);                /* the MAC now drops other networks' frames          */
   rtw_w32(REG_RCR, RCR_LINKED);
   rtw_w16(REG_BCN_INTERVAL, g_jbcn);
   rtw_w8(REG_SECCFG, (UCHAR)(g_open_net ? 0x00 : 0xCC));
   g_jnss = 1;
   g_next_chk = dd_ms() + 1000UL;
   return 0;
}

void hw_leave(void)
{
   USHORT i;

   if (!g_hw_ready)
      return;
   for (i = 0; i < 6; i++)
      rtw_w8(REG_BSSID + i, 0);
   rtw_w32(REG_RCR, RCR_SCAN);
   rtw_keys_reset(0);
   rtw_dig_set(0x20);
}

void rtw_sta_reset(void)
{
   g_ptk_ok = 0;
   pn_reset();
   g_ra_idx = 1;
}

/* Called from the service path (never blocks): beacon supervision and receiver gain, once a second. */
void rtw_link_tick(void)
{
   ULONG  now = dd_ms();
   USHORT k, seen = 0;

   if ((long)(now - g_next_chk) < 0L)
      return;
   g_next_chk = now + 1000UL;
   rtw_dig_tick();
   if (!g_joined)
      return;
   for (k = 0; k < g_nnet; k++)
      if (fmemeq((UCHAR FAR *)g_net[k].bssid, (UCHAR FAR *)g_jbssid, 6)) {
         if ((long)(now - g_net[k].seen) < 10000L)
            seen = 1;
         break;
      }
   if (!seen)
      sta_lost();                                       /* no beacon for 10 s: the AP is gone               */
}

/* ---- aggregation --------------------------------------------------------------------------------------------------
 * Receive: the MAC answers A-MPDUs with block acks by itself (the RTL8821AU driver accepts ADDBA requests without
 * telling the chip anything); wlan/sta.c reorders. Transmit aggregation is not offered. */
int    hw_ba_start(UCHAR tid, USHORT ssn, USHORT win) { (void)tid; (void)ssn; (void)win; return 0; }
int    hw_ba_stop(UCHAR tid)                         { (void)tid; return 0; }
int    hw_txagg_active(void)                          { return 0; }
int    hw_txagg_ok(void)                              { return 0; }
USHORT hw_qos_sn(void)                                { return 0; }
void   hw_txagg_offer(UCHAR tid, USHORT win)          { (void)tid; (void)win; }
void   hw_txagg_stop(UCHAR tid)                       { (void)tid; }
