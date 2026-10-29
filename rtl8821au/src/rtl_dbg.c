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

#if !DBG_ADMIN
/* The commands a release build still accepts: they only read (chip registers, status, network list, eFuse, register dump). */
static int dbg_readonly(char *c)
{
   if (c[0] == 's' && c[1] == 't' && (c[2] == 0 || c[2] == ' ')) return 1;
   if (c[0] == 'n' && (c[1] == 0 || c[1] == ' ')) return 1;
   if (c[0] == 'e' && (c[1] == 0 || c[1] == ' ')) return 1;
   if (c[0] == 'd' && c[1] == 'u' && c[2] == 'm' && c[3] == 'p') return 1;
   return 0;
}
#endif

static char *p;                        /* static: its address is passed to hexval (SS != DS) */
static UCHAR v;                        /* static: efuse_byte() writes through a near pointer */

void rtl8188eu_dbg_write(ULONG phys, USHORT n)
{
   USHORT a, b, c, i;
   union { ULONG l; USHORT w[2]; } u;
   if (n == 0)
      return;
   if (rtl8821_fw_write(phys, n))
      return;                                    /* the firmware image, not a command */
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
   } else if (!dbg_readonly(p)) {
      tcom_always('-');                           /* only the read-only commands (r, st, n, e, dump) exist in this build */
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
   } else if (p[0] == 'r' && p[1] == 'x' && p[2] == 'e') {
      tcom_always('E');                           /* start the asynchronous receive engine */
      rtl8188eu_rx_engine_start(&g_usbdev);
   } else if (p[0] == 'r' && p[1] == 'x' && p[2] == 's') {
      static UCHAR sbuf[1024];
      static USHORT sact;
      int src;

      for (i = 0; i < 1024; i++) sbuf[i] = 0;
      sact = 0;
      src = usb_bulk_msg(&g_usbdev, 4, USB_DIR_IN, sbuf, 1024, &sact);   /* one blocking bulk-IN read on endpoint 4 */
      tcom_always('S'); d16((USHORT)src); tcom_always(':'); d16(sact); tcom_always(':');
      d16(g_u_acc); tcom_always('/'); d16(g_u_done); tcom_always('/'); d16(g_u_stat); tcom_always('/'); d16(g_u_wait); tcom_always(':');
      for (i = 0; i < 40; i++) d8(sbuf[i]);
   } else if (p[0] == 't' && p[1] == 'x' && p[2] == 'b') {
      static USHORT cnt;
      p += 3;
      a = hexval(&p);                             /* number of beacons (default 20, 50 ms apart) */
      if (a == 0 || a > 200) a = 20;
      tcom_always('B');
      for (cnt = 0; cnt < a; cnt++) {
         i = (USHORT)rtl8821_tx_beacon(&g_usbdev, (UCHAR)g_r21_channel);
         if (cnt == 0) d16(i);
         usb_mdelay(50);
      }
      tcom_always('b'); d16(g_r21_txok); d16(g_r21_txfail); d16(g_r21_txlast);
   } else if (p[0] == 's' && p[1] == 's' && p[2] == 'i' && p[3] == 'd') {
      static UCHAR sidb[33];
      USHORT sl = 0;

      p += 4;
      while (*p == ' ') p++;
      while (*p && sl < 32) sidb[sl++] = (UCHAR)*p++;
      while (sl && sidb[sl - 1] <= 0x20) sl--;                     /* the shell's echo leaves a blank before the redirect */
      rtl8188eu_set_want_ssid((UCHAR FAR *)sidb, (UCHAR)sl);       /* the network "join" will look for */
      tcom_always('I'); d8((UCHAR)sl);
   } else if (p[0] == 'd' && p[1] == 'i' && p[2] == 'g') {
      p += 3;
      g_dig_auto = (UCHAR)hexval(&p);              /* 1 = automatic receiver gain, 0 = off ("w c50 N" then sets it) */
      tcom_always('G'); d8(g_dig_auto);
   } else if (p[0] == 'u' && p[1] == 'h') {
      p += 2;
      g_use_hook = hexval(&p);                     /* 1 = frames are delivered to the protocol from a context hook */
      tcom_always('U'); d8((UCHAR)g_use_hook);
   } else if (p[0] == 'i' && p[1] == 'c' && p[2] == 'l' && p[3] == 'i') {
      p += 4;
      g_ind_cli = hexval(&p);                      /* 1 = call the protocol's receive upcalls with interrupts disabled */
      tcom_always('K'); d8((UCHAR)g_ind_cli);
   } else if (p[0] == 's' && p[1] == 't' && (p[2] == 0 || p[2] == ' ')) {
      tcom_always('T'); d8((UCHAR)g_joined); d8((UCHAR)rtl8188eu_has_ptk());          /* light status: joined, pairwise key */
      tcom_always(' '); d16(rtl8188eu_diag(9)); d16(rtl8188eu_diag(55)); d16(rtl8188eu_diag(59)); d16(rtl8188eu_diag(66));
   } else if (p[0] == 'h' && p[1] == 'o' && p[2] == 'l' && p[3] == 'd') {
      p += 4;
      g_r21_hold = hexval(&p);                    /* 1 = background scans do nothing */
      tcom_always('H'); d8((UCHAR)g_r21_hold);
   } else if (p[0] == 'l' && p[1] == 'c' && p[2] == 'k') {
      rtl8821_lck(&g_usbdev);                     /* LC calibration */
      tcom_always('L');
   } else if (p[0] == 'c' && p[1] == 'h' && (p[2] == ' ' || p[2] == 0)) {
      p += 2;
      a = hexval(&p);
      rtl8821_set_channel(&g_usbdev, (UCHAR)a);   /* tune to channel a (hex) */
      tcom_always('c'); d8((UCHAR)a);
   } else if (p[0] == 'r' && p[1] == 'r') {
      static ULONG rfv;
      UCHAR *rb = (UCHAR *)&rfv;
      p += 2;
      a = hexval(&p);
      rfv = rtl8821_rf_read(&g_usbdev, (UCHAR)a);   /* RF path A register a */
      tcom_always('F'); d8((UCHAR)a); tcom_always('='); d8(rb[2]); d8(rb[1]); d8(rb[0]);
   } else if (p[0] == 'r' && p[1] == 'w') {
      static ULONG rfv;
      USHORT *rw16 = (USHORT *)&rfv;
      p += 2;
      a = hexval(&p);
      b = hexval(&p);                              /* data bits 19..16 */
      c = hexval(&p);                              /* data bits 15..0 */
      rw16[0] = c; rw16[1] = b;
      rtl8821_rf_write(&g_usbdev, (UCHAR)a, rfv);
      tcom_always('f'); d8((UCHAR)a);
   } else if (p[0] == 's' && p[1] == 'c') {
      tcom_always('S');                           /* one scan pass over channels 1..13, then the list */
      rtl8188eu_nic_lock();
      rtl8188eu_scan_once(&g_usbdev);
      rtl8188eu_nic_unlock();
      tcom_always('s');
      rtl8188eu_print_nets();
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
