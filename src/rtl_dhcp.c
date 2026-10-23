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
static char   g_text[3000];
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

/* Run DHCP and write the result text into the caller's read buffer (physical address).
 * Returns the number of bytes written. */
USHORT rtl8188eu_dhcp_read(ULONG phys, USHORT max)
{
   int    rc;
   USHORT n, lease64;

   g_tlen = 0;
   put_s("LAN="); put_dec(g_lannum); put_s("\r\n");        /* TCP/IP interface number, for the address helper */
   {
      static char *dgn[DIAG_COUNT] = { "JFAIL", "JAUTH", "JASSOC", "JCH", "JRSSI", "JNNET", "JTRIES", "JWANT", "JBSSF",
                                       "RXFRM", "RXCRC", "RXDESYNC", "RXERR", "TXOK", "TXFAIL", "TXLAST", "TXERR",
                                       "FEEDS", "FMAXN", "FFULL", "DSEV", "MULTI", "PAD8", "DSN",
                                       "UCTRL", "UPORT", "MPSIN", "MPSO2", "MPSO3", "NEP", "TOME", "EPA", "EPB", "EPC", "EPD", "IQOK", "IQTX", "IQTY", "IQRX", "IQRY", "ACK", "CTL", "TMN", "EVN", "PRSENT", "PRGOT", "RXRUN", "RXFAIL", "RXRC", "RXSTAT", "RXLEN", "RXDONE", "TGLBAD", "TGLRP", "TGLOUT" };
      USHORT q, k;

      for (q = 0; q < DIAG_COUNT; q++) {
         put_s(dgn[q]); put_s("="); put_dec(rtl8188eu_diag(q)); put_s("\r\n");
      }
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
   n = g_tlen;
   if (n > max) n = max;
   if (n) {
      rt_dh_phys_to_gdt(phys, n, g_rdsel);
      rt_movmem((PSZ)MAKEP(g_rdsel, 0), (PSZ)g_text, n);
   }
   return n;
}
