/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8821AU driver for OS/2 and ArcaOS.
 *
 * Derived from the Realtek Linux driver (morrownr/8821au-20210708: include/rtl8812a_xmit.h,
 * hal/rtl8812a/rtl8812a_xmit.c, hal/rtl8812a/usb/usb_halinit.c), Copyright(c) 2007 - 2017 Realtek Corporation, GPL-2.0-only.
 *
 * rtl8821_tx.c - transmit: the 40 byte Jaguar TX descriptor and the bulk-OUT queue mapping. Phase 5 starts here; for now it
 * is used by a debug command that sends beacons, to prove that the radio transmits.
 *
 * A bulk-OUT transfer is [40-byte TX descriptor][802.11 frame]:
 *   dw0  PKT_SIZE[15:0]  OFFSET[23:16] = 40  BMC[24]  LS[26]  FS[27]  OWN[31]
 *   dw1  MACID[6:0]  QUEUE_SEL[12:8]  RATE_ID[20:16]
 *   dw3  USE_RATE[8]
 *   dw4  TX_RATE[6:0]  DATA_RATE_FB_LIMIT[12:8]  RETRY_LIMIT_ENABLE[17]  DATA_RETRY_LIMIT[23:18]
 *   dw7  checksum[15:0]: XOR of the first 16 words (the checksum field zero)
 *   dw8  HWSEQ_EN[15]
 * Management frames use queue select 0x12 and the 4th bulk-OUT endpoint (typical queue to pipe mapping of the vendor driver:
 * endpoints in descriptor order 05 06 08 09 = high, normal, low, extra; MGT -> extra).
 */
#include "rt_usb.h"

#define TXDESC_SIZE    40
#define QSLT_MGNT      0x12
#define RATEID_IDX_G   7
#define RATEID_IDX_B   8

USHORT g_r21_txok, g_r21_txfail, g_r21_txlast;

static UCHAR  g_r21_txbuf[400];
static USHORT g_r21_txact;

static void put32le(UCHAR *p, ULONG v)
{
   static ULONG t;                                 /* static: byte access through a union-less pointer */
   UCHAR *q = (UCHAR *)&t;

   t = v;
   p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; p[3] = q[3];
}

/* Build the descriptor at b for an n-byte frame that follows it at b + 40.
 *   qsel = 0x12 management, 0 = best effort data;  sec = 1: hardware AES (CCMP);  rpt = ask for a TX report packet. */
void rtl8821_fill_desc(UCHAR *b, USHORT n, int bcast, UCHAR qsel, int sec, UCHAR rate, int rpt)
{
   USHORT i, ck = 0;
   ULONG  v;

   for (i = 0; i < TXDESC_SIZE; i++) b[i] = 0;
   v = (ULONG)n | ((ULONG)TXDESC_SIZE << 16) | 0x0C000000UL | 0x80000000UL;        /* LS | FS | OWN */
   if (bcast) v |= 0x01000000UL;                                                    /* BMC */
   put32le(b + 0, v);
   v = (ULONG)(qsel == QSLT_MGNT ? 1 : 1) | ((ULONG)qsel << 8) | ((ULONG)(rate < 4 ? RATEID_IDX_B : RATEID_IDX_G) << 16);
   if (sec) v |= 0x00C00000UL;                                                      /* SEC_TYPE = AES */
   put32le(b + 4, v);
   if (rpt) put32le(b + 8, 0x00080000UL);                                           /* SPE_RPT: TX report (C2H) after sending */
   put32le(b + 12, 0x00000500UL);                                                   /* USE_RATE | DISABLE_FB */
   put32le(b + 16, (ULONG)(rate & 0x7F) | 0x00000800UL | 0x00020000UL | 0x00300000UL);   /* rate, fallback limit 8, retry limit 12 */
   put32le(b + 32, 0x00008000UL);                                                   /* HWSEQ_EN: the chip numbers the frame */
   for (i = 0; i < 16; i++)
      ck ^= (USHORT)(b[2 * i] | (b[2 * i + 1] << 8));
   b[28] = (UCHAR)(ck & 0xFF);
   b[29] = (UCHAR)(ck >> 8);
}

/* Send one management frame (header + body in f, n bytes) on the management queue. 0 = the transfer completed. */
int rtl8821_tx_mgmt(usb_dev_t *dev, UCHAR *f, USHORT n, int bcast, UCHAR rate, int cck)
{
   USHORT i;
   int    rc;

   if (n > sizeof(g_r21_txbuf) - TXDESC_SIZE)
      return -1;
   for (i = 0; i < n; i++) g_r21_txbuf[TXDESC_SIZE + i] = f[i];
   (void)cck;
   rtl8821_fill_desc(g_r21_txbuf, n, bcast, QSLT_MGNT, 0, rate, 1);
   g_r21_txact = 0;
   rc = usb_bulk_msg(dev, 9, USB_DIR_OUT, g_r21_txbuf, (USHORT)(TXDESC_SIZE + n), &g_r21_txact);
   if (rc == 0) g_r21_txok++;
   else         { g_r21_txfail++; g_r21_txlast = (USHORT)rc; }
   return rc;
}

/* Debug: a beacon frame with the SSID "RTL8821TEST" on the channel the receiver is tuned to. */
int rtl8821_tx_beacon(usb_dev_t *dev, UCHAR ch)
{
   static UCHAR f[80];
   static UCHAR ssid[11] = { 'R', 'T', 'L', '8', '8', '2', '1', 'T', 'E', 'S', 'T' };
   static UCHAR r2g[8]  = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24 };
   static UCHAR r5g[8]  = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
   USHORT i, n;
   int    five = ch > 14;

   for (i = 0; i < sizeof(f); i++) f[i] = 0;
   f[0] = 0x80;                                   /* beacon */
   for (i = 0; i < 6; i++) {
      f[4 + i]  = 0xFF;                           /* DA */
      f[10 + i] = g_mymac[i];                     /* SA */
      f[16 + i] = g_mymac[i];                     /* BSSID */
   }
   n = 24;
   n += 8;                                        /* timestamp (the chip fills it) */
   f[n++] = 0x64; f[n++] = 0x00;                  /* beacon interval 100 TU */
   f[n++] = 0x01; f[n++] = 0x00;                  /* capability: ESS */
   f[n++] = 0;  f[n++] = 11;
   for (i = 0; i < 11; i++) f[n++] = ssid[i];
   f[n++] = 1;  f[n++] = 8;
   for (i = 0; i < 8; i++) f[n++] = five ? r5g[i] : r2g[i];
   if (!five) { f[n++] = 3; f[n++] = 1; f[n++] = ch; }
   return rtl8821_tx_mgmt(dev, f, n, 1, (UCHAR)(five ? 0x04 : 0x00), !five);   /* 6 Mbit/s OFDM on 5 GHz, 1 Mbit/s CCK on 2.4 GHz */
}
