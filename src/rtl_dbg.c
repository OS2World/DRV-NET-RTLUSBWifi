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
 * rtl_dbg.c - register-level debug channel on the device
 *
 * A line written to \DEV\WRND32$ (for example  echo r 00F0 > \DEV\WRND32$ ) is run as a
 * command; the result goes to the COM1 serial log, prefixed with 'X'. Used for bring-up
 * experiments (efuse access in the cold chip state) without a reboot per guess.
 *
 *   r AAAA          read 8-bit register         r2 AAAA   16-bit        r4 AAAA   32-bit
 *   w AAAA VV       write 8-bit register        w2 AAAA VVVV            w4 AAAA HHHH LLLL
 *   ep 1|0          efuse power on / off (0xCF unlock + loader clocks)
 *   e A N           read N physical efuse bytes from address A with the stock byte sequence
 *   init            run the full chip initialisation now
 * Numbers are hexadecimal.
 */
#include "rt_usb.h"

extern UCHAR g_txrate;                  /* rtl_rx.c: data rate code for NDIS data frames */
extern UCHAR g_txrate_auto;
extern UCHAR g_txasync;                 /* rtl_rx.c */
extern UCHAR g_mgmtrate;                /* rtl_rx.c */

extern USHORT g_dump_base;              /* rtl_dhcp.c */
static USHORT g_wrsel;                 /* GDT selector to read the caller's buffer */
static char   g_cmd[72];

void dbg_init(void)
{
   rt_dh_alloc_gdt(&g_wrsel, 1);
}

static void d8(UCHAR b)
{
   static char hx[] = "0123456789ABCDEF";
   tcom_always(hx[(b >> 4) & 0x0F]);
   tcom_always(hx[b & 0x0F]);
}

static void d16(USHORT v)
{
   d8((UCHAR)(v >> 8));
   d8((UCHAR)(v & 0xFF));
}

static USHORT hexval(char **pp)
{
   USHORT v = 0;
   char   c;
   char  *p = *pp;

   while (*p == ' ') p++;
   for (;;) {
      c = *p;
      if (c >= '0' && c <= '9')      v = (USHORT)((v << 4) | (c - '0'));
      else if (c >= 'a' && c <= 'f') v = (USHORT)((v << 4) | (c - 'a' + 10));
      else if (c >= 'A' && c <= 'F') v = (USHORT)((v << 4) | (c - 'A' + 10));
      else break;
      p++;
   }
   *pp = p;
   return v;
}

static char *p;                        /* static: its address is passed to hexval (SS != DS) */
static UCHAR v;                        /* static: efuse_byte() writes through a near pointer */

void rtl8188eu_dbg_write(ULONG phys, USHORT n)
{
   USHORT a, b, c, i;
   union { ULONG l; USHORT w[2]; } u;
   if (n == 0)
      return;
   if (n > sizeof(g_cmd) - 1) n = sizeof(g_cmd) - 1;
   rt_dh_phys_to_gdt(phys, n, g_wrsel);
   rt_movmem((PSZ)g_cmd, (PSZ)MAKEP(g_wrsel, 0), n);
   g_cmd[n] = 0;
   for (i = 0; i < n; i++)                       /* stop at CR / LF / EOF */
      if (g_cmd[i] == '\r' || g_cmd[i] == '\n' || g_cmd[i] == 0x1A) { g_cmd[i] = 0; break; }

   tcom_always('X');
   p = g_cmd;
   while (*p == ' ') p++;
   if (p[0] == 'r' && (p[1] == ' ' || p[1] == '2' || p[1] == '4')) {
      char w = p[1];
      p += (w == ' ') ? 1 : 2;
      a = hexval(&p);
      tcom_always('r'); d16(a); tcom_always('=');
      if (w == '2')      d16(rtw_read16(&g_usbdev, a));
      else if (w == '4') { u.l = rtw_read32(&g_usbdev, a); d16(u.w[1]); d16(u.w[0]); }
      else               d8(rtw_read8(&g_usbdev, a));
#if !DBG_ADMIN
   } else if ((p[0] == 'w' && (p[1] == ' ' || p[1] == '2' || p[1] == '4')) ||
              (p[0] == 'c' && p[1] == 'd') || (p[0] == 'w' && p[1] == 'a') || (p[0] == 'i' && p[1] == 'n') ||
              (p[0] == 'j' && p[1] == 'o') || (p[0] == 'm' && p[1] == 'r') || (p[0] == 'p' && p[1] == 'r') ||
              (p[0] == 't' && p[1] == 'x' && p[2] == 'a')) {
      tcom_always('-');                           /* admin commands are disabled in this build */
#endif
   } else if (p[0] == 'w' && (p[1] == ' ' || p[1] == '2' || p[1] == '4')) {
      char w = p[1];
      p += (w == ' ') ? 1 : 2;
      a = hexval(&p);
      b = hexval(&p);
      tcom_always('w'); d16(a); tcom_always('=');
      if (w == '2')      { rtw_write16(&g_usbdev, a, b); d16(b); }
      else if (w == '4') { c = hexval(&p); u.w[1] = b; u.w[0] = c; rtw_write32(&g_usbdev, a, u.l); d16(b); d16(c); }
      else               { rtw_write8(&g_usbdev, a, (UCHAR)b); d8((UCHAR)b); }
   } else if (p[0] == 'j' && p[1] == 'o' && p[2] == 'i' && p[3] == 'n') {
      tcom_always('J');
      rtl8188eu_nic_lock();
      rtl8188eu_join(&g_usbdev, 0);               /* join the wanted SSID now (the supplicant is not involved) */
      rtl8188eu_nic_unlock();
      tcom_always('j');
   } else if (p[0] == 'm' && p[1] == 'r' && p[2] == 'a' && p[3] == 't' && p[4] == 'e') {
      p += 5;
      g_mgmtrate = (UCHAR)hexval(&p);             /* rate code for management frames */
      tcom_always('M'); d8(g_mgmtrate);
   } else if (p[0] == 'p' && p[1] == 'r' && (p[2] == ' ' || p[2] == 0)) {
      USHORT cnt, pad;
      p += 2;
      cnt = hexval(&p);
      pad = hexval(&p);
      rtl8188eu_nic_lock();
      rtl8188eu_dbg_probe(cnt, pad);
      rtl8188eu_nic_unlock();
      tcom_always('P');
   } else if (p[0] == 't' && p[1] == 'x' && p[2] == 'a') {
      p += 3;
      g_txasync = (UCHAR)hexval(&p);              /* 1 = management frames go through the async TX slots */
      tcom_always('T'); d8(g_txasync);
   } else if (p[0] == 'd' && p[1] == 'u' && p[2] == 'm' && p[3] == 'p') {
      p += 4;
      g_dump_base = hexval(&p);                   /* next read of the device also lists 64 dwords from here (0xFFFF = off) */
      tcom_always('d'); d16(g_dump_base);
   } else if (p[0] == 'e' && p[1] == 'p') {
      p += 2;
      a = hexval(&p);
      efuse_power(&g_usbdev, a ? 1 : 0);
      tcom_always('e'); tcom_always('p'); d8((UCHAR)a);
   } else if (p[0] == 'e' && (p[1] == ' ' || p[1] == 0)) {
      p += 1;
      a = hexval(&p);
      b = hexval(&p);
      if (b == 0 || b > 64) b = 16;
      tcom_always('e'); d16(a); tcom_always(':');
      for (i = 0; i < b; i++) {
         if (efuse_byte(&g_usbdev, (USHORT)(a + i), &v) == 0) d8(v); else tcom_always('-');
         tcom_always(' ');
      }
   } else if (p[0] == 't' && p[1] == 'x' && p[2] == 'p') {
      p += 3;
      a = hexval(&p);
      rtl8188eu_set_txpower(&g_usbdev, (UCHAR)a);   /* apply the eFuse-based per-rate power for channel a */
   } else if (p[0] == 'r' && p[1] == 'a' && p[2] == 't' && p[3] == 'e') {
      p += 4;
      a = hexval(&p);
      if (a == 0xFF) { g_txrate_auto = 1; }       /* rate ff = adapt again */
      else           { g_txrate = (UCHAR)a; g_txrate_auto = 0; }   /* any other code pins that rate */
      tcom_always('R'); d8(g_txrate);
   } else if (p[0] == 'c' && p[1] == 't' && p[2] == 'l') {
      p += 3;
      g_ctlrate = (UCHAR)hexval(&p);              /* rate code for handshake/ARP/DHCP frames */
      tcom_always('C'); d8(g_ctlrate);
   } else if (p[0] == 'r' && p[1] == 'a' && (p[2] == 0 || p[2] == ' ')) {
      tcom_always('A'); d8(rtl8188eu_cur_rate()); tcom_always(g_txrate_auto ? 'a' : 'm');
   } else if (p[0] == 'n' && (p[1] == 0 || p[1] == ' ')) {
      rtl8188eu_print_nets();
   } else if (p[0] == 'c' && p[1] == 'd' && p[2] == 'i' && p[3] == 's') {
      tcom_always('D');                           /* power the chip down (card disable); "warm" afterwards re-initialises from that state */
      rtl8188eu_nic_detach();
      rtl8188eu_card_disable(&g_usbdev);
      tcom_always('d');
   } else if (p[0] == 'w' && p[1] == 'a' && p[2] == 'r' && p[3] == 'm') {
      tcom_always('W');                           /* simulate a warm chip: forget everything, init again over a running chip */
      rtl8188eu_nic_detach();
      g_hw_failed = 0;
      rtl8188eu_on_open();
      tcom_always(g_hw_ready ? '+' : '-');
   } else if (p[0] == 'i' && p[1] == 'n') {
      tcom_always('i');
      rtl8188eu_on_open();
   } else {
      tcom_always('?');
   }
   tcom_always('.');
}
