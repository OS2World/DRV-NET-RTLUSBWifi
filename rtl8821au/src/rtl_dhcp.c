/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL88EU - Realtek RTL8188EU USB WiFi driver for OS/2 and ArcaOS
 * Copyright (C) 2026 the RTL88EU-OS2 project
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
 * rtl_dhcp.c - a small DHCP client inside the driver
 *
 * The IBM dhcpcd on this system is a single instance that only serves lan0, so the
 * WiFi link gets its address from here instead. A program reads the device
 * (\DEV\WRND32$): that read runs DISCOVER / OFFER / REQUEST / ACK over the air and
 * returns the lease as KEY=VALUE text lines, which a REXX script (rtl88eu_ip.cmd)
 * turns into ifconfig / route commands and renews on a timer.
 *
 * Frames are built as Ethernet frames and sent through the normal encrypted data path
 * (rtl8188eu_ndis_tx); replies are taken from the receive path (dhcp_rx) before they
 * would be handed to TCP/IP. No C runtime is available (/nod), so text is built by hand
 * and no 32-bit shifts or divisions are used.
 */
#include "rt_usb.h"


#define DHCP_OFFER  2
#define DHCP_ACK    5

static UCHAR  g_dh[360];               /* outgoing Ethernet frame                       */
static UCHAR  g_rep[320];              /* last accepted reply: BOOTP payload             */
static USHORT g_replen;
static UCHAR  g_want;                  /* message type we are waiting for (0 = idle)     */
static UCHAR  g_got;                   /* a matching reply is in g_rep                   */
static UCHAR  g_xid[4];
static USHORT g_xcount;
static UCHAR  g_ip[4], g_mask[4], g_router[4], g_dns[4], g_srv[4], g_lease[4];
static UCHAR  g_have_router, g_have_dns, g_have_mask;
static USHORT g_rdsel;                 /* GDT selector used to write into the caller's buffer */
static char   g_text[3900];
static USHORT g_tlen;
static UCHAR  g_olen;
static UCHAR  g_opt_len;

void dhcp_init(void)
{
   rt_dh_alloc_gdt(&g_rdsel, 1);
}

static USHORT dh_ipsum(UCHAR *p, USHORT n)
{
   USHORT s = 0, w, i;

   for (i = 0; i < n; i += 2) {
      w = (USHORT)((p[i] << 8) | p[i + 1]);
      s = (USHORT)(s + w);
      if (s < w) s++;
   }
   return (USHORT)~s;
}

/* Find DHCP option 'code' in a BOOTP payload; returns pointer to its data or 0. */
static UCHAR *dh_opt(UCHAR *p, USHORT n, UCHAR code)
{
   USHORT i = 240;

   if (n < 241 || p[236] != 0x63 || p[237] != 0x82 || p[238] != 0x53 || p[239] != 0x63)
      return 0;
   while (i < n) {
      UCHAR c = p[i];
      if (c == 255) break;
      if (c == 0) { i++; continue; }
      if ((USHORT)(i + 2) > n || (USHORT)(i + 2 + p[i + 1]) > n) break;
      if (c == code) {
         g_opt_len = p[i + 1];
         return p + i + 2;
      }
      i = (USHORT)(i + 2 + p[i + 1]);
   }
   return 0;
}

/* Called from the receive path with a converted Ethernet frame. Returns 1 when the frame
 * was a reply to our exchange (it is then not passed on to TCP/IP). */
int dhcp_rx(UCHAR *e, USHORT len)
{
   UCHAR *p, *t;
   USHORT i, pl;

   if (!g_want || g_got)
      return 0;
   if (len < 14 + 20 + 8 + 241 || e[12] != 0x08 || e[13] != 0x00 || e[14] != 0x45 || e[23] != 17)
      return 0;
   if (e[36] != 0 || e[37] != 68)                       /* UDP destination port 68        */
      return 0;
   tcom('<');                                          /* a UDP frame for port 68 arrived */
   p  = e + 42;
   pl = (USHORT)(len - 42);
   if (p[0] != 2) { tcom('a'); return 0; }                /* BOOTREPLY                      */
   for (i = 0; i < 4; i++)
      if (p[4 + i] != g_xid[i]) { tcom('x'); return 0; }
   for (i = 0; i < 6; i++)
      if (p[28 + i] != g_mymac[i]) { tcom('c'); return 0; }
   t = dh_opt(p, pl, 53);
   if (!t) { tcom('o'); return 0; }
   if (*t != g_want) { tcom('t'); tcom((char)('0' + *t)); return 0; }
   tcom('k');
   if (pl > sizeof(g_rep)) pl = sizeof(g_rep);
   for (i = 0; i < pl; i++)
      g_rep[i] = p[i];
   g_replen = pl;
   g_got = 1;
   return 1;
}

/* Build and send one DHCP message (type 1 = DISCOVER, 3 = REQUEST). */
static int dh_send(UCHAR type, int with_req)
{
   USHORT i, n, ck, plen, flen;
   UCHAR  *o;

   for (i = 0; i < sizeof(g_dh); i++) g_dh[i] = 0;
   for (i = 0; i < 6; i++) { g_dh[i] = 0xFF; g_dh[6 + i] = g_mymac[i]; }
   g_dh[12] = 0x08; g_dh[13] = 0x00;
   g_dh[14] = 0x45;
   g_dh[22] = 64; g_dh[23] = 17;
   for (i = 0; i < 4; i++) g_dh[30 + i] = 0xFF;          /* dst 255.255.255.255            */
   g_dh[35] = 68; g_dh[37] = 67;                         /* UDP 68 -> 67                   */
   g_dh[42] = 1; g_dh[43] = 1; g_dh[44] = 6;             /* BOOTREQUEST, ethernet, hlen 6  */
   for (i = 0; i < 4; i++) g_dh[46 + i] = g_xid[i];
   g_dh[52] = 0x80;                                      /* broadcast flag                 */
   for (i = 0; i < 6; i++) g_dh[70 + i] = g_mymac[i];    /* chaddr                         */
   g_dh[278] = 0x63; g_dh[279] = 0x82; g_dh[280] = 0x53; g_dh[281] = 0x63;
   o = g_dh + 282;
   n = 0;
   o[n++] = 53; o[n++] = 1; o[n++] = type;
   if (with_req) {
      o[n++] = 50; o[n++] = 4;                           /* requested address              */
      for (i = 0; i < 4; i++) o[n++] = g_ip[i];
      o[n++] = 54; o[n++] = 4;                           /* server identifier              */
      for (i = 0; i < 4; i++) o[n++] = g_srv[i];
   }
   o[n++] = 55; o[n++] = 5;                              /* parameter request list         */
   o[n++] = 1; o[n++] = 3; o[n++] = 6; o[n++] = 15; o[n++] = 51;
   o[n++] = 255;
   plen = (USHORT)(240 + n);
   if (plen < 300) plen = 300;                           /* classic BOOTP minimum          */
   flen = (USHORT)(42 + plen);
   g_dh[16] = (UCHAR)((flen - 14) >> 8); g_dh[17] = (UCHAR)((flen - 14) & 0xFF);   /* IP total length */
   g_dh[38] = (UCHAR)((plen + 8) >> 8);  g_dh[39] = (UCHAR)((plen + 8) & 0xFF);    /* UDP length      */
   ck = dh_ipsum(g_dh + 14, 20);
   g_dh[24] = (UCHAR)(ck >> 8); g_dh[25] = (UCHAR)(ck & 0xFF);
   tcom('s'); tcom((char)('0' + type));
   return rtl8188eu_ndis_tx(g_dh, flen);
}

static int dh_wait(UCHAR want, USHORT ms)
{
   USHORT t;

   g_got = 0;
   g_want = want;
   for (t = 0; t < ms; t = (USHORT)(t + 20)) {
      if (g_got)
         break;
      usb_mdelay(20);
   }
   g_want = 0;
   tcom(g_got ? '+' : '-');
   return g_got ? 0 : -1;
}

/* Run the exchange. 0 = lease in g_ip/g_mask/..., else an error code. */
static int dh_run(void)
{
   USHORT i, tries;
   UCHAR  *t;

   if (!g_joined)
      return 1;                                          /* no WiFi connection yet         */
   g_xcount++;
   g_xid[0] = g_mymac[2]; g_xid[1] = g_mymac[3];
   g_xid[2] = (UCHAR)(g_xcount >> 8); g_xid[3] = (UCHAR)(g_xcount & 0xFF);
   g_have_router = g_have_dns = g_have_mask = 0;

   for (tries = 0; tries < 4; tries++) {                 /* DISCOVER -> OFFER              */
      if (dh_send(1, 0) != 0)
         return 2;                                       /* link not ready for data        */
      if (dh_wait(DHCP_OFFER, 3000) == 0)
         break;
   }
   if (!g_got)
      return 3;                                          /* no OFFER                       */
   for (i = 0; i < 4; i++) g_ip[i] = g_rep[16 + i];      /* yiaddr                         */
   t = dh_opt(g_rep, g_replen, 54);
   if (t && g_opt_len >= 4) for (i = 0; i < 4; i++) g_srv[i] = t[i];
   else for (i = 0; i < 4; i++) g_srv[i] = g_rep[20 + i];   /* siaddr as a fallback        */

   for (tries = 0; tries < 3; tries++) {                 /* REQUEST -> ACK                 */
      if (dh_send(3, 1) != 0)
         return 2;
      if (dh_wait(DHCP_ACK, 3000) == 0)
         break;
   }
   if (!g_got)
      return 4;                                          /* no ACK                         */
   for (i = 0; i < 4; i++) g_ip[i] = g_rep[16 + i];
   t = dh_opt(g_rep, g_replen, 1);
   if (t && g_opt_len >= 4) { for (i = 0; i < 4; i++) g_mask[i] = t[i]; g_have_mask = 1; }
   t = dh_opt(g_rep, g_replen, 3);
   if (t && g_opt_len >= 4) { for (i = 0; i < 4; i++) g_router[i] = t[i]; g_have_router = 1; }
   t = dh_opt(g_rep, g_replen, 6);
   if (t && g_opt_len >= 4) { for (i = 0; i < 4; i++) g_dns[i] = t[i]; g_have_dns = 1; }
   t = dh_opt(g_rep, g_replen, 51);
   for (i = 0; i < 4; i++) g_lease[i] = (t && g_opt_len >= 4) ? t[i] : 0;
   return 0;
}

/* ---- text output (no C runtime) ---------------------------------------------- */
USHORT        g_dump_base = 0xFFFF;
static char   g_hx[3];                       /* static: SS != DS, no pointers to stack data */
static void put_s(char *s)
{
   while (*s && g_tlen < sizeof(g_text) - 2)
      g_text[g_tlen++] = *s++;
}

static void put_dec(USHORT v)
{
   char b[6];
   USHORT i = 0;

   if (v == 0) b[i++] = '0';
   while (v) { b[i++] = (char)('0' + (v % 10)); v = (USHORT)(v / 10); }
   while (i && g_tlen < sizeof(g_text) - 2) g_text[g_tlen++] = b[--i];
}

static void put_ip(UCHAR *a)
{
   put_dec(a[0]); put_s("."); put_dec(a[1]); put_s("."); put_dec(a[2]); put_s("."); put_dec(a[3]);
}

static void put_h8(UCHAR b)
{
   static char hxd[17] = "0123456789ABCDEF";

   g_hx[0] = hxd[(b >> 4) & 15]; g_hx[1] = hxd[b & 15]; g_hx[2] = 0;
   put_s(g_hx);
}

static void put_h32(ULONG v)                 /* high byte first */
{
   static union { ULONG l; UCHAR b[4]; } u;

   u.l = v;
   put_h8(u.b[3]); put_h8(u.b[2]); put_h8(u.b[1]); put_h8(u.b[0]);
}

/* Run DHCP and write the result text into the caller's read buffer (physical address).
 * Returns the number of bytes written. */
USHORT rtl8188eu_dhcp_read(ULONG phys, USHORT max)
{
   int    rc;
   USHORT n, lease64;

   g_tlen = 0;
   if (!g_r21_tried && g_usbdev.attached && !g_hw_ready && !g_hw_failed) {      /* RTL8821AU bring-up test: first read runs it */
      g_r21_tried = 1;
      rtl8188eu_probe(&g_usbdev);
   }
   /* The lease comes first: the diagnostics below can fill g_text, and the address helper needs these lines.
    * (With the larger 0.1.4 scan table they no longer fitted after the dump and every renewal read as "no lease".) */
   rc = dh_run();
   if (rc != 0) {
      put_s("STATUS=ERR\r\nCODE="); put_dec((USHORT)rc);
      put_s(rc == 1 ? "\r\nWHY=NOLINK\r\n" : (rc == 2 ? "\r\nWHY=NOTREADY\r\n" : "\r\nWHY=NOANSWER\r\n"));
   } else {
      put_s("STATUS=OK\r\nIP=");     put_ip(g_ip);
      put_s("\r\nSERVER=");          put_ip(g_srv);
      if (g_have_mask)   { put_s("\r\nMASK=");   put_ip(g_mask); }
      if (g_have_router) { put_s("\r\nROUTER="); put_ip(g_router); }
      if (g_have_dns)    { put_s("\r\nDNS=");    put_ip(g_dns); }
      /* lease time in units of 64 s: (b1 << 10) | (b2 << 2) | (b3 >> 6), capped */
      if (g_lease[0] != 0 || g_lease[1] >= 0x40)
         lease64 = 0xFFFF;
      else
         lease64 = (USHORT)((g_lease[1] << 10) | (g_lease[2] << 2) | (g_lease[3] >> 6));
      put_s("\r\nLEASE64="); put_dec(lease64); put_s("\r\n");
   }
   {  /* RTL8821AU bring-up results */
      static UCHAR hxd[17] = "0123456789ABCDEF";
      static ULONG rv;
      UCHAR *rb = (UCHAR *)&rv;
      USHORT i, k;

      put_s("BUILD=" DRV_VERSION "-" DRV_BUILD " " __DATE__ " " __TIME__ "\r\n");   /* which driver wrote this file */
      put_s("R21STAGE="); put_dec(g_r21_stage);
      put_s("\r\nR21FWLOAD="); put_dec(g_r21_fwload); put_s(" LEN="); put_dec(g_r21_fwlen); put_s(" SIG=");
      rv = g_r21_fwsig; for (i = 2; i > 0; i--) { g_hx[0] = hxd[rb[i - 1] >> 4]; g_hx[1] = 0; put_s(g_hx); g_hx[0] = hxd[rb[i - 1] & 15]; put_s(g_hx); }
      put_s("\r\nR21PWRFAIL="); put_dec(g_r21_pwr_fail); put_s(" LLTFAIL="); put_dec(g_r21_llt_fail);
      put_s("\r\nR21FWTRIES="); put_dec(g_r21_fwtries); put_s(" CHKSUM="); put_dec(g_r21_chksum); put_s(" READY="); put_dec(g_r21_ready);
      for (k = 0; k < 2; k++) {
         rv = k ? g_r21_mcufwdl : g_r21_cfg;
         put_s(k ? "\r\nR21MCUFWDL=" : "\r\nR21SYSCFG=");
         for (i = 4; i > 0; i--) { g_hx[0] = hxd[rb[i - 1] >> 4]; g_hx[1] = 0; put_s(g_hx); g_hx[0] = hxd[rb[i - 1] & 15]; put_s(g_hx); }
      }
      put_s("\r\nR21PHY="); put_dec(g_r21_phy_stage); put_s(" CH="); put_dec(g_r21_channel);
      for (k = 0; k < 8; k++) {
         put_s(" C"); put_dec(k); put_s("=");
         rv = g_r21_chk[k];
         for (i = 4; i > 0; i--) { g_hx[0] = hxd[rb[i - 1] >> 4]; g_hx[1] = 0; put_s(g_hx); g_hx[0] = hxd[rb[i - 1] & 15]; put_s(g_hx); }
      }
      put_s("\r\n");
   }
   put_s("TX OK="); put_dec(g_r21_txok); put_s(" FAIL="); put_dec(g_r21_txfail); put_s(" LAST="); put_dec(g_r21_txlast);
   put_s("\r\n");
   {  /* eFuse */
      static UCHAR hx3[17] = "0123456789ABCDEF";
      USHORT q;

      put_s("EFOK="); put_dec(g_r21_ef_ok); put_s(" USED="); put_dec(g_r21_ef_used);
      put_s(" XTAL="); put_dec(g_r21_xtal); put_s(" PA2G="); put_dec(g_r21_pa2g); put_s(" LNA2G="); put_dec(g_r21_lna2g);
      put_s(" RFE="); put_dec(g_r21_rfe); put_s(" THERM="); put_dec(g_r21_thermal);
      put_s(" MAC=");
      for (q = 0; q < 6; q++) { g_hx[0] = hx3[g_mymac[q] >> 4]; g_hx[1] = hx3[g_mymac[q] & 15]; g_hx[2] = 0; put_s(g_hx); }
      put_s("\r\n");
   }
   {  /* transmit power: the eFuse power area (logical 0x10-0x21 for 2.4 GHz, 0x22-0x39 for 5 GHz, path A) and the TX AGC
       * registers as the chip has them now (one power index per rate byte: CCK, OFDM 6-18, OFDM 24-54, MCS0-3, VHT 1SS
       * MCS0-3, VHT 1SS MCS8-9 + 2SS MCS0-1) */
      static USHORT tpr[6] = { 0x0C20, 0x0C24, 0x0C28, 0x0C2C, 0x0C3C, 0x0C44 };
      static char  *tpn[6] = { "C20=", " C24=", " C28=", " C2C=", " C3C=", " C44=" };
      USHORT q;

      put_s("EFPWR2G=");
      for (q = 0x10; q < 0x22; q++) put_h8(g_r21_ef[q]);
      put_s(" 5G=");
      for (q = 0x22; q < 0x3A; q++) put_h8(g_r21_ef[q]);
      put_s("\r\nTXAGC ");
      for (q = 0; q < 6; q++) {
         put_s(tpn[q]);
         if (g_hw_ready) put_h32(rtw_read32(&g_usbdev, tpr[q])); else put_s("-");
      }
      put_s("\r\nTXP5="); put_dec(g_r21_txp5);              /* 5 GHz power from the eFuse: switch, channel group, */
      put_s(" GRP="); put_dec(g_r21_txp_grp);                     /* indexes written (0 = BB table values)              */
      put_s(" OFDM="); put_h8(g_r21_txp_ofdm); put_s(" HT="); put_h8(g_r21_txp_ht);
      put_s("\r\nIQK="); put_dec(g_r21_iqk);                 /* firmware IQ calibration: mode, finish report byte,  */
      put_s(" C2H="); put_h8(g_r21_iqk_c2h);                     /* busy mailbox, correction registers 0xC10 / 0xCCC /  */
      put_s(" H2CBUSY="); put_dec(g_r21_h2c_busy);               /* 0xCD4 before / after the last run                   */
      put_s(" IQC=");
      for (q = 0; q < 6; q++) { put_h32(g_r21_iqc[q]); put_s(q == 2 ? "/" : (q == 5 ? "" : " ")); }
      put_s("\r\n");
      put_s("HT="); put_dec(g_r21_ht);                           /* HT / WMM join: switch, what the access point granted  */
      put_s(" GRANT="); put_dec(rtl8188eu_ht_state());           /* (1 HT cap, 2 HT operation, 4 WMM), ADDBA requests /    */
      put_s(" BA="); put_dec(g_ba_req); put_s("/"); put_dec(g_ba_tx); put_s("/"); put_dec(g_ba_fail);   /* declines sent / failed */
      put_s(" FAST="); put_dec(g_ba_fast); put_s(" BAMS="); put_dec(g_ba_ms);   /* declines sent from the hook; last delay, ms */
      put_s(" TID="); put_dec(g_ba_tid);
      put_s(" AMSDU="); put_dec(g_amsdu_n); put_s("/"); put_dec(g_amsdu_sub); put_s("/"); put_dec(g_amsdu_bad);   /* frames / subframes / dropped */
      put_s("\r\n");
      put_s("BAACC="); put_dec(g_r21_ba); put_s(" BAON=");        /* Block Ack receive: switch, TIDs with an agreement */
      put_dec(rtl8188eu_ba_on());                                  /* (bit n = TID n); frames of those TIDs, out of     */
      put_s(" BARX="); put_dec(g_ba_rx); put_s(" OOO="); put_dec(g_ba_ooo);   /* order, sequence gaps, duplicates */
      put_s(" GAP="); put_dec(g_ba_gap); put_s(" DUPW="); put_dec(g_ba_dupw);  /* caught by the window, DELBAs   */
      put_s(" DELBA="); put_dec(g_ba_del);
      put_s("\r\n");
      put_s("AGG ALL="); put_dec(g_agg_all); put_s(" ME="); put_dec(g_agg_me);   /* frames from an A-MPDU (any / to us); */
      put_s(" QOS="); put_dec(g_dq_qos); put_s(" PLAIN="); put_dec(g_dq_plain);   /* data to us: QoS / plain; QoS by TID */
      put_s(" TID=");
      for (q = 0; q < 8; q++) { put_dec(g_dq_tid[q]); if (q < 7) put_s("/"); }
      put_s("\r\n");
      put_s("RO HELD="); put_dec(g_ro_held); put_s(" REL="); put_dec(g_ro_rel);   /* reorder buffer (TID 0): frames held, */
      put_s(" SKIP="); put_dec(g_ro_skip); put_s(" STALE="); put_dec(g_ro_stale); /* released in order, given up, timeouts, */
      put_s(" BAR="); put_dec(g_ro_bar); put_s(" LATE="); put_dec(g_ro_late);     /* BARs, late frames, held copies dropped, */
      put_s(" DROP="); put_dec(g_ro_drop); put_s(" MAX="); put_dec(g_ro_max);     /* most held at once, runs paused for     */
      put_s(" WAIT="); put_dec(g_ro_wait);                                          /* want of a receive buffer,              */
      put_s(" KICK="); put_dec(g_ro_kick); put_s(" FULL="); put_dec(g_ro_full);     /* sent up by the hook, in order but held */
      put_s("\r\n");
   }
   {  /* scan table */
      USHORT n, q;
      static UCHAR hx2[17] = "0123456789ABCDEF";

      put_s("NETS="); put_dec(g_nnet); put_s("\r\n");
      for (n = 0; n < g_nnet; n++) {
         if (n >= 14 && g_net[n].ch <= 14)                 /* the text buffer is small: past 14 only the 5 GHz entries */
            continue;
         put_s("NET=");
         for (q = 0; q < 6; q++) {
            g_hx[0] = hx2[g_net[n].bssid[q] >> 4]; g_hx[1] = hx2[g_net[n].bssid[q] & 15]; g_hx[2] = 0; put_s(g_hx);
            if (q < 5) put_s(":");
         }
         put_s(" CH="); put_dec(g_net[n].ch); put_s(" SEC="); put_dec(g_net[n].sec);
         put_s(" RSSI="); if (g_net[n].rssi < 0) { put_s("-"); put_dec((USHORT)(-g_net[n].rssi)); } else put_dec((USHORT)g_net[n].rssi);
         put_s(" CNT="); put_dec(g_net[n].cnt);
         put_s(" SSID="); put_s(g_net[n].len ? g_net[n].ssid : "?"); put_s("\r\n");
      }
   }
   put_s("LAN="); put_dec(g_lannum); put_s("\r\n");        /* TCP/IP interface number, for the address helper */
   {
      static char *dgn[DIAG_COUNT] = { "JFAIL", "JAUTH", "JASSOC", "JCH", "JRSSI", "JNNET", "JTRIES", "JWANT", "JBSSF",
                                       "RXFRM", "RXCRC", "RXDESYNC", "RXERR", "TXOK", "TXFAIL", "TXLAST", "TXERR",
                                       "FEEDS", "FMAXN", "FFULL", "DSEV", "MULTI", "PAD8", "DSN",
                                       "UCTRL", "UPORT", "MPSIN", "MPSO2", "MPSO3", "NEP", "TOME", "EPA", "EPB", "EPC", "EPD", "IQOK", "IQTX", "IQTY", "IQRX", "IQRY", "ACK", "CTL", "TMN", "EVN", "PRSENT", "PRGOT", "RXRUN", "RXFAIL", "RXRC", "RXSTAT", "RXLEN", "RXDONE", "TGLBAD", "TGLRP", "TGLOUT",
                                       "DNDATA", "DNPROT", "DNSW", "DNDROP", "NTXCALL", "NTXOK", "NTXNOKEY", "NTXNOSLOT", "PTK", "GTK", "JOINED", "NDUP", "DQN", "INDBUSY", "INDOFF", "TXLATAVG", "TXLATMAX", "TXLATN", "NTXARP", "NTXTCP", "NTXACK", "NTXUDP", "NTXICMP", "NTXOTHER",
                                       "XRN", "XRRETRY", "XRRETRYMAX", "XRQAVG", "XRQMAX", "XRRATE", "XROVER", "XRQBIG",
                                       "RXCCK", "RXOFDM6", "RXOFDM18", "RXOFDM36", "RXHT", "RXVHT", "TSFJIT", "TSFN", "XRRTAVG", "XRRTMAX", "ICMPN", "ICMPSTACKAVG", "ICMPSTACKMAX", "RCN", "RCAVG", "RCMAX", "RCSLOW", "IFON", "IFOFF", "RELN", "RELAVG", "RELMAX", "RCWAIT", "RCSUCC", "RCOTHER", "USEHOOK", "HKRUN", "HKMAXQ", "HKARMFAIL", "GAP0", "GAP1_2", "GAP3_9", "GAP10_19", "GAP20", "INDTMO", "INDRE", "INDDROP", "TXQRE", "CPUTX", "CPURX", "KADDP", "KADDG", "KREM", "KDUP", "PN0", "PN1", "DCN", "DC1R", "DC1SUB", "DC1SEC", "DCLR", "DCLSEC", "TXQTMO" ,
                                       "RCV0", "RCV1", "RCV2", "RCV3", "RCC0", "RCC1", "RCC2", "RCC3", "RCLAST", "RCLEN", "RCET", "RCIPP", "RCDST",
                                       "NTXSTUCK", "TXUNSTICK", "MACLATE", "OPEN" };
      USHORT q, k;

      for (q = 0; q < DIAG_COUNT; q++) {
         put_s(dgn[q]); put_s("="); put_dec(rtl8188eu_diag(q)); put_s("\r\n");
      }
      put_s("ULN="); put_dec(rtl8188eu_diag_ul_n()); put_s("\r\nUL=");
      {
         USHORT cnt = rtl8188eu_diag_ul_n(), first = 0, m;

         if (cnt > 20) { first = (USHORT)(cnt - 20); cnt = 20; }
         for (m = 0; m < cnt; m++) {                   /* oldest first: ms r fc0 fc1 len a2 */
            USHORT idx = (USHORT)((first + m) % 20), w, z;

            put_dec(rtl8188eu_diag_ul(idx, 0)); put_s(":");
            for (z = 1; z <= 5; z++) {
               w = rtl8188eu_diag_ul(idx, z);
               g_hx[0] = "0123456789ABCDEF"[(w >> 4) & 15]; g_hx[1] = "0123456789ABCDEF"[w & 15]; put_s(g_hx);
               put_s(z == 5 ? " " : ".");
            }
         }
      }
      put_s("\r\n");
      for (k = 0; k < 22; k++) {                       /* chip registers: high word then low word, hex */
         static char *rn[22] = { "F0", "100", "608", "610", "614", "618", "522", "210", "420", "10C", "800", "E08",
                                 "C80", "C94", "C4C", "C14", "CA0", "C50", "E00", "E04", "86C", "E10" };
         USHORT hi = rtl8188eu_diag_reg(k, 1), lo = rtl8188eu_diag_reg(k, 0), s;

         put_s("R"); put_s(rn[k]); put_s("=");
         for (s = 0; s < 8; s++) {
            USHORT w = (USHORT)(s < 4 ? hi : lo), sh = (USHORT)(12 - 4 * (s & 3));
            g_hx[0] = "0123456789ABCDEF"[(w >> sh) & 15]; g_hx[1] = 0; put_s(g_hx);
         }
         put_s("\r\n");
      }
      put_s("EV=");
      for (k = 0; k < rtl8188eu_diag(43); k++) {      /* join event log: kind@ms */
         USHORT v = rtl8188eu_diag_ev(k, 0);
         g_hx[0] = "0123456789ABCDEF"[v >> 4]; g_hx[1] = "0123456789ABCDEF"[v & 15]; put_s(g_hx);
         v = rtl8188eu_diag_ev(k, 2);
         if (v) { put_s("."); g_hx[0] = "0123456789ABCDEF"[v >> 4]; g_hx[1] = "0123456789ABCDEF"[v & 15]; put_s(g_hx); }
         put_s("@"); put_dec(rtl8188eu_diag_ev(k, 1)); put_s(" ");
      }
      put_s("\r\n");
      for (k = 0; k < 6; k++) {                        /* physical address of each DMA buffer end (control, rx, tx slot 0) */
         USHORT hi = usb_phys(k, 1), lo = usb_phys(k, 0), s;

         put_s("PH"); put_dec(k); put_s("=");
         for (s = 0; s < 8; s++) {
            USHORT w = (USHORT)(s < 4 ? hi : lo), sh = (USHORT)(12 - 4 * (s & 3));
            g_hx[0] = "0123456789ABCDEF"[(w >> sh) & 15]; g_hx[1] = 0; put_s(g_hx);
         }
         put_s("\r\n");
      }
      for (k = 0; k < rtl8188eu_diag(42); k++) {          /* frames addressed to us: subtype, source, length */
         put_s("TM"); put_dec(k); put_s("=");
         for (q = 0; q < 8; q++) {
            USHORT v = rtl8188eu_diag_tm(k, q);
            g_hx[0] = "0123456789ABCDEF"[v >> 4]; g_hx[1] = "0123456789ABCDEF"[v & 15]; put_s(g_hx);
            if (q == 0 || q == 6) put_s(" ");
         }
         put_s("\r\n");
      }
      for (k = 0; k < rtl8188eu_diag(23); k++) {
         put_s("DS"); put_dec(k); put_s("=");
         for (q = 36; q < 40; q++) { put_dec(rtl8188eu_diag_ctx(k, q)); put_s(" "); }
         for (q = 0; q < 36; q++) {
            USHORT v = rtl8188eu_diag_ctx(k, q);
            put_s(q == 12 ? " | " : "");
            g_hx[0] = "0123456789ABCDEF"[v >> 4]; g_hx[1] = "0123456789ABCDEF"[v & 15]; put_s(g_hx);
         }
         put_s("\r\n");
      }
   }
   if ((g_dump_base & 0xFF00) == 0xF000) {                    /* RF registers 0x00..0x3F through the path-A 3-wire read-back (PI mode) */
      USHORT n, s, hi, lo, w, sh;
      static union { ULONG l; USHORT w[2]; } ru, ra;
      static USHORT keep_hi, keep_lo;

      ru.l = rtw_read32(&g_usbdev, 0x0824);
      keep_hi = ru.w[1]; keep_lo = ru.w[0];
      for (n = 0; n < 64; n++) {
         if ((n & 7) == 0) {
            put_s("RF");
            g_hx[0] = "0123456789ABCDEF"[((g_dump_base + n) >> 4) & 15]; g_hx[1] = "0123456789ABCDEF"[(g_dump_base + n) & 15]; put_s(g_hx);
            put_s(":");
         }
         ra.w[0] = keep_lo;
         ra.w[1] = (USHORT)(keep_hi & 0x7FFF);               /* read edge low */
         rtw_write32(&g_usbdev, 0x0824, ra.l);
         ra.w[1] = (USHORT)((keep_hi & 0x007F) | ((((g_dump_base & 0xFF) + n) << 7) & 0x7F80) | 0x8000);   /* register number + edge */
         rtw_write32(&g_usbdev, 0x0824, ra.l);
         ra.w[1] = (USHORT)(keep_hi | 0x8000);               /* edge high again */
         ra.w[0] = keep_lo;
         rtw_write32(&g_usbdev, 0x0824, ra.l);
         for (s = 0; s < 2; s++) {                           /* PI read-back (0x8B8), then SI read-back (0x8A0) */
            USHORT k;
            ru.l = rtw_read32(&g_usbdev, (USHORT)(s ? 0x08A0 : 0x08B8));
            hi = (USHORT)(ru.w[1] & 0x000F); lo = ru.w[0];
            put_s(s ? "/" : " ");
            for (k = 0; k < 5; k++) {
               w = (USHORT)(k < 1 ? hi : lo); sh = (USHORT)(k < 1 ? 0 : 12 - 4 * (k - 1));
               g_hx[0] = "0123456789ABCDEF"[(w >> sh) & 15]; g_hx[1] = 0; put_s(g_hx);
            }
         }
         if ((n & 7) == 7) put_s("\r\n");
      }
      ra.w[1] = keep_hi; ra.w[0] = keep_lo;
      rtw_write32(&g_usbdev, 0x0824, ra.l);                   /* leave the register as it was */
   } else if (g_dump_base != 0xFFFF) {                    /* register window: 64 dwords, 8 per line */
      USHORT n, s, hi, lo, w, sh;
      static union { ULONG l; USHORT w[2]; } du;

      for (n = 0; n < 64; n++) {
         if ((n & 7) == 0) {
            put_s("D");
            for (s = 0; s < 4; s++) { g_hx[0] = "0123456789ABCDEF"[((USHORT)(g_dump_base + n * 4) >> (12 - 4 * s)) & 15]; g_hx[1] = 0; put_s(g_hx); }
            put_s(":");
         }
         du.l = rtw_read32(&g_usbdev, (USHORT)(g_dump_base + n * 4));
         hi = du.w[1]; lo = du.w[0];
         put_s(" ");
         for (s = 0; s < 8; s++) {
            w = (USHORT)(s < 4 ? hi : lo); sh = (USHORT)(12 - 4 * (s & 3));
            g_hx[0] = "0123456789ABCDEF"[(w >> sh) & 15]; g_hx[1] = 0; put_s(g_hx);
         }
         if ((n & 7) == 7) put_s("\r\n");
      }
   }
   n = g_tlen;
   if (n > max) n = max;
   if (n) {
      rt_dh_phys_to_gdt(phys, n, g_rdsel);
      rt_movmem((PSZ)MAKEP(g_rdsel, 0), (PSZ)g_text, n);
   }
   return n;
}
