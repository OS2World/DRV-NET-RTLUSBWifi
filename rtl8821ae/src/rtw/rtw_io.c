/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtw_io.c - register access of the RTL8821AE back end: baseband and RF registers, firmware commands.
 *
 * Ported from rtl8821au/src/rtl8821_phy.c (bb_set, bb_get, rf_write, rf_read, r21_h2c), which derives from the Realtek
 * Linux driver morrownr/8821au-20210708 (hal/rtl8812a/rtl8812a_phycfg.c, rtl8812a_cmd.c), Copyright(c) 2007 - 2017
 * Realtek Corporation, GPL-2.0-only. Over PCIe every register is a plain MMIO access in BAR2 (rtw_r8 ... rtw_w32 in
 * rtw.h); the USB vendor requests of the dongle driver are gone.
 * Only 32-bit operations the 16-bit compiler inlines are used (no variable shifts, no multiply / divide).
 */
#include "drv.h"
#include "rtw.h"

void rtw_mdelay(USHORT ms)
{
   dd_msleep(ms ? ms : 1);
}

ULONG shl1(ULONG v)
{
   U32 x;

   x.l = v;
   x.w[1] = (USHORT)((x.w[1] << 1) | (x.w[0] >> 15));
   x.w[0] = (USHORT)(x.w[0] << 1);
   return x.l;
}

ULONG shr1(ULONG v)
{
   U32 x;

   x.l = v;
   x.w[0] = (USHORT)((x.w[0] >> 1) | (x.w[1] << 15));
   x.w[1] = (USHORT)(x.w[1] >> 1);
   return x.l;
}

/* BB register: data into the bits selected by mask (read-modify-write), like PHY_SetBBReg8812 */
void bb_set(USHORT reg, ULONG mask, ULONG data)
{
   ULONG v, m;

   if (mask != 0xFFFFFFFFUL) {
      v = rtw_r32(reg);
      m = mask;
      while (!(m & 1UL)) {                 /* move data to the lowest set bit of the mask */
         m = shr1(m);
         data = shl1(data);
      }
      v = (v & ~mask) | (data & mask);
      rtw_w32(reg, v);
   } else
      rtw_w32(reg, data);
}

ULONG bb_get(USHORT reg, ULONG mask)
{
   ULONG v = rtw_r32(reg) & mask;

   while (mask && !(mask & 1UL)) {
      mask = shr1(mask);
      v = shr1(v);
   }
   return v;
}

/* RF path A register write: (offset << 20 | data[19:0]) into the 3-wire register of the BB (0xC90) */
void rf_write(UCHAR reg, ULONG data)
{
   U32 x;

   x.l = data;
   x.w[1] = (USHORT)((x.w[1] & 0x000F) | ((USHORT)reg << 4));
   rtw_w32(0x0C90, x.l);
   dd_udelay(1);
}

/* RF path A register read (phy_RFSerialRead for the 8821: no CCA toggling, a short settle) */
ULONG rf_read(UCHAR reg)
{
   USHORT pi;

   pi = (USHORT)bb_get(0x0C00, 0x4UL);
   bb_set(0x08B0, 0xFFUL, (ULONG)reg);
   dd_udelay(20);
   return bb_get(pi ? 0x0D04 : 0x0D08, 0xFFFFFUL);
}

/* ---- firmware commands (H2C) ------------------------------------------------------------------------------
 * fill_h2c_cmd_8812 / rtl8821ae_fill_h2c_cmd: four mailboxes, used in turn. A box is free when its bit in REG_HMETFR
 * is clear (the firmware clears it when it has read the command). The extension box (0x1F0 + 4 * n) is written first,
 * then the box itself (0x1D0 + 4 * n): byte 0 = command id, bytes 1-3 = data. Task time only. */
UCHAR  g_h2c_box;
USHORT g_h2c_busy;

int rtw_h2c(UCHAR id, UCHAR b1, UCHAR b2, UCHAR b3)
{
   UCHAR  box = g_h2c_box, bit = (UCHAR)(1 << box);
   USHORT n, mb = (USHORT)(REG_HMEBOX_0 + (box << 2)), xb = (USHORT)(REG_HMEBOX_EXT0 + (box << 2));

   for (n = 0; n < 100 && (rtw_r8(REG_HMETFR) & bit); n++)
      rtw_mdelay(1);
   if (n >= 100) {
      g_h2c_busy++;
      return -1;
   }
   rtw_w32(xb, 0UL);
   rtw_w8(mb, id);
   rtw_w8(mb + 1, b1);
   rtw_w8(mb + 2, b2);
   rtw_w8(mb + 3, b3);
   g_h2c_box = (UCHAR)((box + 1) & 3);
   return 0;
}
