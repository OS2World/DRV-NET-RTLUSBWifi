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
 * rtl_rf.c - RTL8188EU RF (radio) config (Phase 2 milestone 2.5). AUTO-GENERATED
 * from HalHWImg8188E_RF.c Array_RadioA_1T_8188E (conditions pre-resolved for USB =
 * apply-all; markers dropped). RF regs are indirect: each write shifts (addr<<20)|
 * (data&0xFFFFF) into the path-A LSSI register 0x840 (rf3wireOffset). daa values are
 * PRE-SHIFTED here so the driver does no 32-bit shifts. Preceded by the RF-enable
 * preamble from phy_RF6052_Config_ParaFile. cmd 0=LSSI write, 1=delay(ms).
 */
#include "rt_usb.h"

#define RF_LSSI   0x0840   /* rFPGA0_XA_LSSIParameter (path-A 3-wire write) */

typedef struct { UCHAR cmd; ULONG arg; } rfcmd_t;
static rfcmd_t g_rf[] = {
  {0,0x0030000UL},{0,0x0884000UL},{0,0x1800407UL},{0,0x1900012UL},{0,0x1E80009UL},
  {0,0x1F00880UL},{0,0x2F1A060UL},{0,0x3F00000UL},{0,0x42060C0UL},{0,0x57D0000UL},
  {0,0x58BE180UL},{0,0x6701552UL},{0,0x8300000UL},{0,0xB0FF8FCUL},{0,0xB154400UL},
  {0,0xB2CCC19UL},{0,0xB443003UL},{0,0xB64953EUL},{0,0xB71C718UL},{0,0xB8060FFUL},
  {0,0xB980001UL},{0,0xBA40000UL},{0,0xBB00400UL},{0,0xBFC0000UL},{0,0xC202400UL},
  {0,0xC300009UL},{0,0xC440C91UL},{0,0xC599999UL},{0,0xC6000A3UL},{0,0xC788820UL},
  {0,0xC876C06UL},{0,0xC900000UL},{0,0xCA80000UL},{0,0xDF00180UL},{0,0xEF001A0UL},
  {0,0x516B27DUL},{0,0x527E4DDUL},{0,0x527E49DUL},{0,0x5300073UL},{0,0x5651FF3UL},
  {0,0x3500086UL},{0,0x3500186UL},{0,0x3500286UL},{0,0x3601C25UL},{0,0x3609C25UL},
  {0,0x3611C25UL},{0,0x3619C25UL},{0,0xB648538UL},{0,0x1800C07UL},{0,0x5A4BD00UL},
  {0,0x19739D0UL},{0,0x340ADF3UL},{0,0x3409DF0UL},{0,0x3408DEDUL},{0,0x3407DEAUL},
  {0,0x3406DE7UL},{0,0x34054EEUL},{0,0x34044EBUL},{0,0x34034E8UL},{0,0x340246BUL},
  {0,0x3401468UL},{0,0x340006DUL},{0,0x0030159UL},{0,0x8468200UL},{0,0x86000CEUL},
  {0,0x8748A00UL},{0,0x8E65540UL},{0,0x8F88000UL},{0,0xEF020A0UL},{0,0x3BF02B0UL},
  {0,0x3BEF7B0UL},{0,0x3BD4FB0UL},{0,0x3BCF060UL},{0,0x3BB0090UL},{0,0x3BA0080UL},
  {0,0x3B90080UL},{0,0x3B8F780UL},{0,0x3B722B0UL},{0,0x3B6F7B0UL},{0,0x3B54FB0UL},
  {0,0x3B4F060UL},{0,0x3B30090UL},{0,0x3B20080UL},{0,0x3B10080UL},{0,0x3B0F780UL},
  {0,0xEF000A0UL},{0,0x0010159UL},{0,0x180F407UL},{1,0x0000032UL},{1,0x0000032UL},
  {0,0x1F80003UL},{1,0x0000032UL},{1,0x0000032UL},{0,0x1E00001UL},{0,0x1F80000UL},
  {0,0x0033E60UL},
};
#define G_RF_N (sizeof(g_rf)/sizeof(g_rf[0]))

/* Milestone 2.5: RF-enable preamble + RadioA table via LSSI. COM1: 'G'/'g'. */
int rtl8188eu_config_rf(usb_dev_t *dev)
{
   USHORT i;
   ULONG  v, save;

   tcom('G');
   /* RF-enable preamble (phy_RF6052_Config_ParaFile): enable 3-wire iface. */
   save = rtw_read32(dev, 0x0870) & 0x00000010UL;      /* save RFENV (0x870 b4) */
   v = rtw_read32(dev, 0x0860); rtw_write32(dev, 0x0860, v | 0x00100000UL); /* rfintfe b20 */
   v = rtw_read32(dev, 0x0860); rtw_write32(dev, 0x0860, v | 0x00000010UL); /* rfintfo b4  */
   v = rtw_read32(dev, 0x0824); rtw_write32(dev, 0x0824, v & ~0x00000400UL); /* 3wire addr len */
   v = rtw_read32(dev, 0x0824); rtw_write32(dev, 0x0824, v & ~0x00000800UL); /* 3wire data len */

   for (i = 0; i < (USHORT)G_RF_N; i++) {
      if (g_rf[i].cmd == 0)
         rtw_write32(dev, RF_LSSI, g_rf[i].arg);
      else
         usb_mdelay((USHORT)g_rf[i].arg);
   }

   v = rtw_read32(dev, 0x0870);                        /* restore RFENV */
   rtw_write32(dev, 0x0870, (v & ~0x00000010UL) | save);
   tcom('g');
   return 0;
}
