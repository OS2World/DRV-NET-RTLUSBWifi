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
 * rtw_reg.c - RTL8188EU register access over the OS/2 USB HAL (Phase 1)
 *
 * Implements the Realtek register read/write primitives as USB vendor control
 * transfers, per hal/usb_ops_linux.c + include/usb_ops.h of the Linux driver:
 *   read : bmRequestType 0xC0, bRequest 0x05, wValue=addr, wIndex=0, len 1/2/4
 *   write: bmRequestType 0x40, bRequest 0x05, wValue=addr, wIndex=0, len 1/2/4
 *
 * These are THE primitive the whole ported rtw core funnels register I/O
 * through (rtw_read8/16/32, rtw_write8/16/32), so getting this right + the chip
 * version read below is the core of Phase 1 milestone 3.
 */
#include "rt_usb.h"

/* Realtek vendor request constants (include/usb_ops.h). */
#define REALTEK_USB_VENQT_READ    0xC0
#define REALTEK_USB_VENQT_WRITE   0x40
#define REALTEK_USB_VENQT_CMD_REQ 0x05

/* Chip version register (include/rtl8188e_spec.h). */
#define REG_SYS_CFG               0x00F0

/* ---- register reads --------------------------------------------------- */
static int reg_read(usb_dev_t *dev, USHORT addr, void *buf, USHORT len)
{
   return usb_ctrl_msg(dev, REALTEK_USB_VENQT_READ, REALTEK_USB_VENQT_CMD_REQ,
                       addr /*wValue*/, 0 /*wIndex*/, buf, len);
}

static int reg_write(usb_dev_t *dev, USHORT addr, void *buf, USHORT len)
{
   return usb_ctrl_msg(dev, REALTEK_USB_VENQT_WRITE, REALTEK_USB_VENQT_CMD_REQ,
                       addr /*wValue*/, 0 /*wIndex*/, buf, len);
}

/* Register values live in static (DS-based) storage, not on the stack: the HAL
 * copies them via the DS-assuming movmem, so a stack (SS) address would be
 * misread. Static is safe here because Phase 1 register I/O is synchronous. */
UCHAR rtw_read8(usb_dev_t *dev, USHORT addr)
{
   static UCHAR v;
   UCHAR r;

   usb_xfer_lock();
   v = 0;
   reg_read(dev, addr, &v, 1);
   r = v;
   usb_xfer_unlock();
   return r;
}

USHORT rtw_read16(usb_dev_t *dev, USHORT addr)
{
   static USHORT v;              /* device is little-endian; OS/2 x86 is too  */
   USHORT r;

   usb_xfer_lock();
   v = 0;
   reg_read(dev, addr, &v, 2);
   r = v;
   usb_xfer_unlock();
   return r;
}

ULONG rtw_read32(usb_dev_t *dev, USHORT addr)
{
   static ULONG v;
   ULONG r;

   usb_xfer_lock();
   v = 0;
   reg_read(dev, addr, &v, 4);
   r = v;
   usb_xfer_unlock();
   return r;
}

void rtw_write8(usb_dev_t *dev, USHORT addr, UCHAR val)
{
   static UCHAR v;

   usb_xfer_lock();
   v = val;
   reg_write(dev, addr, &v, 1);
   usb_xfer_unlock();
}

void rtw_write16(usb_dev_t *dev, USHORT addr, USHORT val)
{
   static USHORT v;

   usb_xfer_lock();
   v = val;
   reg_write(dev, addr, &v, 2);
   usb_xfer_unlock();
}

void rtw_write32(usb_dev_t *dev, USHORT addr, ULONG val)
{
   static ULONG v;

   usb_xfer_lock();
   v = val;
   reg_write(dev, addr, &v, 4);
   usb_xfer_unlock();
}

/* Multi-byte register write (rtw_writeN) - the firmware-download primitive: one
 * vendor control-OUT of up to ~254 bytes. len must be <= the HAL bounce buffer
 * (g_databuf, 260). pdata is a DS-near pointer (into the firmware blob), which
 * usb_ctrl_msg copies through the bounce buffer, so no static temp needed. */
int rtw_writeN(usb_dev_t *dev, USHORT addr, USHORT len, UCHAR *pdata)
{
   int rc;

   usb_xfer_lock();
   rc = usb_ctrl_msg(dev, REALTEK_USB_VENQT_WRITE, REALTEK_USB_VENQT_CMD_REQ,
                     addr /*wValue*/, 0 /*wIndex*/, pdata, len);
   usb_xfer_unlock();
   return rc;
}

/* ---- Phase 1 milestone 3: read the chip version ----------------------- */
/*
 * Reading REG_SYS_CFG (0xF0) proves the whole control path: setup packet ->
 * ACCIO -> USBD -> device -> completion -> data back. A plausible value
 * (non-zero, non-0xFFFFFFFF) means the dongle answered us over USB.
 *
 * The full _ReadChipVersion in hal/rtl8188e_hal_init.c decodes trailer/cut/vendor
 * bits from this word; for Phase 1 we just confirm we can read it.
 */
ULONG rtl8188eu_read_chip_version(usb_dev_t *dev)
{
   return rtw_read32(dev, REG_SYS_CFG);
}

/* ---- Phase 1 probe: the milestone sequence ---------------------------- */
/*
 * Run after the dongle is matched (rt_idc CHKSERV) AND configured
 * (usb_set_configuration). Sequence:
 *   1. read the chip version (proves the control path works),
 *   2. upload rtl8188eufw.bin (fwdl.c - TODO next increment),
 * logging each step over COM (dsPrint / the driver's debug output) so we can
 * watch it on the VM the way we watched the VBox driver.
 *
 * Returns 0 on success. This is the Phase 1 pass/fail gate.
 */
/* Output one byte to COM1 as two hex digits (8-bit shifts only - avoids the
 * 32-bit shift runtime helper __aNulshr, which we don't link with /nod). */
static void tcom_hex8(UCHAR b)
{
   static char hex[] = "0123456789ABCDEF";
   tcom(hex[(b >> 4) & 0x0F]);
   tcom(hex[b & 0x0F]);
}

/* Output the chip version to COM1 as 'V=xxxxxxxx '. tcom is the reliable COM
 * writer (rt_strat.c); dprintf may be debug-gated. ver is static so its bytes are
 * DS-addressable (a stack local would be SS != DS). */
int g_hw_ready  = 0;
int g_hw_failed = 0;

/*
 * Bring the chip fully up, once: chip id -> power-on -> queues -> firmware ->
 * MAC / baseband / RF tables -> RX enable (LLT, filters, demod) -> IQ calibration
 * -> TX prerequisites. COM1 trace: V=.. K W P Q q D..C+FK H h B b G g L l E I..i.
 * Returns 0 on success; on failure the chip needs a physical re-plug.
 */
/* ===== eFuse: read the factory MAC address ======================================
 * Sequence and record format follow the stock driver (Efuse_PowerSwitch,
 * efuse_OneByteRead, Hal_EfuseReadEFuse88E): the 256-byte physical eFuse holds
 * variable records [header][words...]; header = (section<<4)|word-enable-mask (bit
 * set = word absent), or an extended header when the low 5 bits are 0x0F. The
 * decoded "logical map" has the MAC address at offset 0xD7. */
int efuse_byte(usb_dev_t *dev, USHORT addr, UCHAR *out)
{
   /* Exactly the stock driver's byte sequence (Windows USB capture): write address, rewrite 0x32,
    * clear ctrl bit 31 via 0x33, poll 0x33 bit 7 (ready), read the data byte at 0x30. */
   USHORT i;
   UCHAR  b;

   rtw_write8(dev, 0x0031, (UCHAR)(addr & 0xFF));
   b = rtw_read8(dev, 0x0032);
   rtw_write8(dev, 0x0032, (UCHAR)(((addr >> 8) & 0x03) | (b & 0xFC)));
   b = rtw_read8(dev, 0x0033);
   rtw_write8(dev, 0x0033, (UCHAR)(b & 0x7F));
   for (i = 0; i < 200; i++)
      if (rtw_read8(dev, 0x0033) & 0x80)
         break;
   if (i >= 200)
      return -1;
   *out = rtw_read8(dev, 0x0030);
   return 0;
}

void efuse_power(usb_dev_t *dev, int on)
{
   USHORT v;

   if (on) {
      /* the stock driver's opening moves: look at 0xFE66/0xFE67 and 0x0A, unlock with 0xCF = 0x69,
       * then check (and only if needed fix) FEN_ELDR and the loader clocks */
      rtw_read8(dev, 0xFE66); rtw_read8(dev, 0xFE67);
      rtw_read8(dev, 0x000A);
      rtw_write8(dev, 0x00CF, 0x69);
      v = rtw_read16(dev, 0x0002);                /* SYS_FUNC_EN: FEN_ELDR */
      if (!(v & 0x1000)) rtw_write16(dev, 0x0002, (USHORT)(v | 0x1000));
      v = rtw_read16(dev, 0x0008);                /* SYS_CLKR: LOADER_CLK_EN | ANA8M */
      if ((v & 0x0022) != 0x0022) rtw_write16(dev, 0x0008, (USHORT)(v | 0x0022));
#if TRACE
      tcom('D'); tcom_hex8((UCHAR)(rtw_read16(dev, 0x0000) >> 8)); tcom_hex8((UCHAR)rtw_read16(dev, 0x0000));
      tcom(':'); tcom_hex8((UCHAR)(rtw_read16(dev, 0x0002) >> 8)); tcom_hex8((UCHAR)rtw_read16(dev, 0x0002));
      tcom(':'); tcom_hex8((UCHAR)(rtw_read16(dev, 0x0008) >> 8)); tcom_hex8((UCHAR)rtw_read16(dev, 0x0008));
      tcom(':'); tcom_hex8(rtw_read8(dev, 0x000A)); tcom(' ');
#endif
   } else {
      rtw_write8(dev, 0x00CF, 0x00);
   }
}

/* Decode just enough of the eFuse to fetch the 6-byte MAC (logical 0xD7). 0 = ok. */
static UCHAR g_efmap[0xE0];
static UCHAR g_efvalid;                /* g_efmap holds a decoded eFuse (TX power bases etc.) */

int rtl8188eu_efuse_mac(usb_dev_t *dev, UCHAR *mac)
{
   static UCHAR hdr, ext, b0, b1;                 /* static: efuse_byte() writes through a near pointer (SS != DS) */
   USHORT addr = 0, off, i, w, base;
   UCHAR  wden;
   int    rc = -1;

   for (i = 0; i < sizeof(g_efmap); i++) g_efmap[i] = 0xFF;
   efuse_power(dev, 1);
#if TRACE
   tcom('R');                                    /* raw dump of the first 12 physical eFuse bytes */
   for (i = 0; i < 12; i++) { if (efuse_byte(dev, i, &b0) == 0) tcom_hex8(b0); else tcom('-'); }
   tcom('.');
#endif
   if (efuse_byte(dev, addr++, &hdr) != 0)
      goto out;
   while (hdr != 0xFF && addr < 256) {
      if ((hdr & 0x1F) == 0x0F) {                 /* extended header */
         off = (USHORT)((hdr & 0xE0) >> 5);
         if (efuse_byte(dev, addr++, &ext) != 0) goto out;
         if ((ext & 0x0F) == 0x0F) {              /* all words disabled: next header */
            if (efuse_byte(dev, addr++, &hdr) != 0) goto out;
            continue;
         }
         off |= (USHORT)((ext & 0xF0) >> 1);
         wden = (UCHAR)(ext & 0x0F);
      } else {
         off  = (USHORT)((hdr >> 4) & 0x0F);
         wden = (UCHAR)(hdr & 0x0F);
      }
      for (w = 0; w < 4; w++) {
         if (wden & (1 << w))                     /* word not present */
            continue;
         if (efuse_byte(dev, addr++, &b0) != 0 || efuse_byte(dev, addr++, &b1) != 0)
            goto out;
         base = (USHORT)(off * 8 + w * 2);
         if (base + 1 < sizeof(g_efmap)) {                /* extended headers reach sections 26/27 (MAC) */
            g_efmap[base]     = b0;
            g_efmap[base + 1] = b1;
         }
      }
      if (efuse_byte(dev, addr++, &hdr) != 0) goto out;
   }
   for (i = 0; i < 6; i++)
      mac[i] = g_efmap[0xD7 + i];
   g_efvalid = (g_efmap[0] == 0x29 && g_efmap[1] == 0x81);   /* EEPROM ID 0x8129 */
   rc = 0;
out:
   efuse_power(dev, 0);
   return rc;
}

/* ===== TX power per rate =======================================================
 * The init table leaves every rate at index 0x2D. The stock driver sets per-rate indices
 * from the eFuse: CCK base per channel group (eFuse 0x10..0x15), OFDM/HT base per group
 * (0x16..0x1A) and a diff byte (0x1B: high nibble HT20, low nibble OFDM, signed). Per-rate
 * offsets on top are the ones the Windows driver wrote for this dongle (USB capture):
 *   OFDM  6/9/12/18/24/36/48/54 : +18 +17 +15 +13 +11 +9 +5 +1  (relative to the OFDM level)
 *   MCS0..7                    : +19 +17 +15 +13 +11 +9 +5 +1  (relative to the HT20 level)
 *   CCK   1/2/5.5/11           : +7  +5  +3  +1                 (relative to the CCK base)
 * Written to 0xE00 / 0xE04 / 0xE10 / 0xE14 (whole words), 0xE08 byte 1 and 0x86C bytes 1..3. */
static UCHAR g_ofdm_off[8] = { 18, 17, 15, 13, 11, 9, 5, 1 };
static UCHAR g_ht_off[8]   = { 19, 17, 15, 13, 11, 9, 5, 1 };
static UCHAR g_cck_off[4]  = { 7, 5, 3, 1 };

static int sx4(UCHAR n)
{
   n &= 0x0F;
   return (n & 8) ? (int)n - 16 : (int)n;
}

static UCHAR txp_byte(int v)
{
   if (v < 0) v = 0;
   if (v > 0x3F) v = 0x3F;
   return (UCHAR)v;
}

void rtl8188eu_set_txpower(usb_dev_t *dev, UCHAR ch)
{
   int  g, gb, cck, base, ofdm, ht, i;
   UCHAR d;
   union { ULONG l; UCHAR b[4]; } u;

   if (!g_efvalid)
      return;                                      /* no eFuse data: keep the init-table defaults */
   g  = (ch < 3) ? 0 : (ch < 6) ? 1 : (ch < 9) ? 2 : (ch < 12) ? 3 : (ch < 14) ? 4 : 5;
   gb = (g > 4) ? 4 : g;
   cck  = g_efmap[0x10 + g];  if (cck  == 0xFF) cck  = 0x2D;
   base = g_efmap[0x16 + gb]; if (base == 0xFF) base = 0x2D;
   d    = g_efmap[0x1B];
   ofdm = base + (d == 0xFF ? 0 : sx4(d));
   ht   = base + (d == 0xFF ? 0 : sx4((UCHAR)(d >> 4)));

   for (i = 0; i < 4; i++) u.b[i] = txp_byte(ofdm + g_ofdm_off[i]);       /* 6, 9, 12, 18 */
   rtw_write32(dev, 0x0E00, u.l);
   for (i = 0; i < 4; i++) u.b[i] = txp_byte(ofdm + g_ofdm_off[4 + i]);   /* 24, 36, 48, 54 */
   rtw_write32(dev, 0x0E04, u.l);
   for (i = 0; i < 4; i++) u.b[i] = txp_byte(ht + g_ht_off[i]);           /* MCS0..3 */
   rtw_write32(dev, 0x0E10, u.l);
   for (i = 0; i < 4; i++) u.b[i] = txp_byte(ht + g_ht_off[4 + i]);       /* MCS4..7 */
   rtw_write32(dev, 0x0E14, u.l);
   u.l = rtw_read32(dev, 0x0E08);                                           /* CCK 1 Mbit/s: byte 1 */
   u.b[1] = txp_byte(cck + g_cck_off[0]);
   rtw_write32(dev, 0x0E08, u.l);
   u.l = rtw_read32(dev, 0x086C);                                           /* CCK 2 / 5.5 / 11: bytes 1..3 */
   u.b[1] = txp_byte(cck + g_cck_off[1]);
   u.b[2] = txp_byte(cck + g_cck_off[2]);
   u.b[3] = txp_byte(cck + g_cck_off[3]);
   rtw_write32(dev, 0x086C, u.l);
   tcom('P'); tcom_hex8((UCHAR)cck); tcom_hex8((UCHAR)ofdm); tcom_hex8((UCHAR)ht);
}

/* True if b[0..5] looks like a real unicast MAC (not all-0 / all-FF / multicast). */
static int mac_valid(UCHAR *b)
{
   USHORT i;
   UCHAR  z = 0, f = 0xFF;

   for (i = 0; i < 6; i++) { z |= b[i]; f &= b[i]; }
   return z != 0 && f != 0xFF && !(b[0] & 1);
}

/* Early (bind-time) address discovery: power the chip and read the eFuse so the NDIS
 * tables carry the real station address before the protocol copies it. Only runs when
 * the dongle is attached and the full init has not happened yet; harmless otherwise. */
int rtl8188eu_early_mac(void)
{
   ULONG ver;

   if (!g_usbdev.attached || g_hw_ready)
      return -1;
   ver = rtw_read32(&g_usbdev, 0x00F0);           /* SYS_CFG: does the control path answer? */
   if (ver == 0 || ver == 0xFFFFFFFFUL)
      return -2;                                  /* USB not serviced (yet): give up quickly */
   if (rtl8821_early_power(&g_usbdev) != 0)       /* the eFuse needs the MAC powered */
      return -3;
   if (rtl8821_efuse_load(&g_usbdev) != 0 || !g_r21_ef_ok)
      return -4;                                  /* g_mymac now holds the programmed address */
   tcom('F');
   return 0;
}

int rtl8188eu_hw_init(usb_dev_t *dev)
{
   return rtl8821_hw_init(dev);                   /* RTL8821AU port: the phases so far end after the firmware download */
}

/* Open handler entry: initialise the chip if it is not up yet, then (optionally)
 * run the serial-output diagnostics once. */
int rtl8188eu_probe(usb_dev_t *dev)
{
   if (g_hw_ready)
      return 0;
   if (g_hw_failed)
      return -1;
   if (rtl8188eu_hw_init(dev) != 0) {
      g_hw_failed = 1;                             /* (no warm-chip retry yet: the 8821 power-off flow is not ported) */
      return -1;
   }
   return 0;
}
