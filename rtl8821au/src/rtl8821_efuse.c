/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8821AU driver for OS/2 and ArcaOS.
 *
 * Derived from the Realtek Linux driver (morrownr/8821au-20210708: hal/rtl8812a/rtl8812a_hal_init.c,
 * include/hal_pg.h), Copyright(c) 2007 - 2017 Realtek Corporation, GPL-2.0-only.
 *
 * rtl8821_efuse.c - the RTL8821A eFuse: 1024 bytes of physical records decoded into the 512 byte logical map, and the
 * few fields the driver needs from it (MAC address, crystal trim, external PA / LNA flags, RFE type, thermal reference).
 * The byte access sequence is the same as for the 8188EU (efuse_byte / efuse_power in rtw_reg.c).
 */
#include "rt_usb.h"

#define EF_PHYS_LEN        1024         /* EFUSE_REAL_CONTENT_LEN_JAGUAR */
#define EF_MAP_LEN         512          /* EFUSE_MAP_LEN_JAGUAR */
#define EE_XTAL            0xB9         /* EEPROM_XTAL_8821 */
#define EE_THERMAL         0xBA
#define EE_PA_TYPE         0xBC         /* EEPROM_PA_TYPE_8821AU */
#define EE_LNA_2G          0xBD         /* EEPROM_LNA_TYPE_2G_8812AU */
#define EE_RFE             0xCA         /* EEPROM_RFE_OPTION_8812 */
#define EE_MAC             0x107        /* EEPROM_MAC_ADDR_8821AU */

UCHAR  g_r21_ef[EF_MAP_LEN];            /* logical map (0xFF where nothing is programmed) */
UCHAR  g_r21_ef_ok;                     /* 1 = decoded, 0 = no (or empty) eFuse: defaults in use */
UCHAR  g_r21_xtal = 0x20;               /* EEPROM_Default_CrystalCap_8812 */
UCHAR  g_r21_lna2g, g_r21_pa2g;         /* external 2.4 GHz LNA / PA present */
UCHAR  g_r21_rfe, g_r21_thermal = 0xFF;
USHORT g_r21_ef_used;                   /* physical bytes consumed by the records */

/* 0 = decoded; -1 = read error. A map that is entirely 0xFF is reported as "empty" (g_r21_ef_ok stays 0). */
int rtl8821_efuse_load(usb_dev_t *dev)
{
   static UCHAR hdr, ext, b0, b1;                 /* static: efuse_byte() writes through a near pointer (SS != DS) */
   static UCHAR m[6];
   USHORT addr = 0, off, i, w, base;
   UCHAR  wden, any, ff;
   int    rc = -1;

   for (i = 0; i < EF_MAP_LEN; i++) g_r21_ef[i] = 0xFF;
   g_r21_ef_ok = 0;
   efuse_power(dev, 1);
   if (efuse_byte(dev, addr++, &hdr) != 0)
      goto out;
   while (hdr != 0xFF && addr < EF_PHYS_LEN) {
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
         if (base + 1 < EF_MAP_LEN) {
            g_r21_ef[base]     = b0;
            g_r21_ef[base + 1] = b1;
         }
      }
      if (efuse_byte(dev, addr++, &hdr) != 0) goto out;
   }
   g_r21_ef_used = addr;
   rc = 0;
   if (g_r21_ef[0] == 0xFF && g_r21_ef[1] == 0xFF)
      goto out;                                   /* nothing programmed */
   g_r21_ef_ok = 1;
   if (g_r21_ef[EE_XTAL] != 0xFF)
      g_r21_xtal = g_r21_ef[EE_XTAL];
   g_r21_thermal = g_r21_ef[EE_THERMAL];
   g_r21_pa2g  = (UCHAR)((g_r21_ef[EE_PA_TYPE] != 0xFF && (g_r21_ef[EE_PA_TYPE] & 0x10)) ? 1 : 0);
   g_r21_lna2g = (UCHAR)((g_r21_ef[EE_LNA_2G] != 0xFF && (g_r21_ef[EE_LNA_2G] & 0x08)) ? 1 : 0);
   g_r21_rfe   = g_r21_ef[EE_RFE];
   any = 0;
   ff = 1;
   for (i = 0; i < 6; i++) {
      m[i] = g_r21_ef[EE_MAC + i];
      any |= m[i];
      if (m[i] != 0xFF) ff = 0;
   }
   if (any && !ff && !(m[0] & 1))                 /* a unicast, programmed address */
      for (i = 0; i < 6; i++) g_mymac[i] = m[i];
out:
   efuse_power(dev, 0);
   return rc;
}
