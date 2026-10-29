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
 * rtl_iqk.c - RTL8188EU RX/TX IQ calibration (Phase 2 milestone 2.7).
 *
 * Port of PHY_IQCalibrate_8188E (hal/HalPhyRf_8188e.c), SIMPLIFIED to a single
 * shot for 1T1R (path A only): the stock driver runs 3 passes and averages via
 * phy_SimularityCompare; we do ONE pass and apply it directly (final_candidate
 * = 0). This is the missing mandatory init step - the radio hears energy on the
 * AP's channel but decodes ZERO frames (Fast_Fsync=0) without IQ correction.
 *
 * Runs at task time (CMDOpen), after BB+RF config and rx_enable. Calibrates at
 * channel 6 (the confirmed AP channel).
 *
 * MS C 6.0 / 16-bit / /nod constraints handled:
 *  - NO 32-bit runtime helpers: every 32-bit shift is done by byte/word picking
 *    through a union; the one true 32-bit signed multiply in the result-apply
 *    (X*Oldval) is an inline `imul` (16x16->DX:AX) into a DS scratch long.
 *  - all register temporaries are locals of ULONG/union; no stack pointers to
 *    USBD (rtw_read/write32 already bounce through DS-safe statics).
 */
#include "rt_usb.h"

/* ---- register addresses (rtl8188e phy reg defs) ----------------------- */
/* ADDA regs saved/forced during IQK (IQK_ADDA_REG_NUM = 16). */
static USHORT g_adda[16] = {
   0x85c, 0xe6c, 0xe70, 0xe74, 0xe78, 0xe7c, 0xe80, 0xe84,
   0xe88, 0xe8c, 0xed0, 0xed4, 0xed8, 0xedc, 0xee0, 0xeec
};
/* MAC regs saved/forced (IQK_MAC_REG_NUM = 4): [0..2] byte, [3] dword. */
static USHORT g_macr[4] = { 0x522, 0x550, 0x551, 0x040 };
/* BB regs saved/restored (IQK_BB_REG_NUM = 9). */
static USHORT g_bbr[9] = {
   0xc04, 0xc08, 0x874, 0xb68, 0xb6c, 0x870, 0x860, 0x864, 0x800
};

static ULONG g_addabk[16];
static USHORT g_iqk_reg[5] = { 0x0C14, 0x0C4C, 0x0C80, 0x0C94, 0x0CA0 };
static ULONG  g_iqk_pre[5], g_iqk_post[5];
static ULONG g_bbbk[9];
static ULONG g_macbk[4];        /* [0..2] byte value, [3] dword */
static int   g_res[4];          /* result[0..3]: TXpre TXpost RXpre RXpost */

/* DS scratch for the inline signed multiply (avoids the long-mul helper). */
static long g_prod;

/* Output a USHORT as 4 hex digits (16-bit constant shifts only). */
static void ih16(USHORT v)
{
   static char hx[] = "0123456789ABCDEF";
   tcom(hx[(v >> 12) & 0x0F]);
   tcom(hx[(v >> 8)  & 0x0F]);
   tcom(hx[(v >> 4)  & 0x0F]);
   tcom(hx[ v        & 0x0F]);
}

/* g_prod = (signed)a * (signed)b, via one imul (DX:AX = AX * r/m16, signed).
 * a in [-512,511], b in [0,1023] -> product fits in 21 bits, well within 32. */
static void imul_s(int a, int b)
{
   g_prod = rt_imul16(a, b);
}

/* read-modify-write a BB dword: (old & ~clr) | orv. clr is a compile-time
 * constant (constant 32-bit NOT is inline-safe under /nod). */
static void bb_rmw(usb_dev_t *dev, USHORT addr, ULONG clr, ULONG orv)
{
   ULONG v = rtw_read32(dev, addr);
   v &= ~clr;
   v |= orv;
   rtw_write32(dev, addr, v);
}

/* set (on!=0) or clear a single BB bit. */
static void bb_bit(usb_dev_t *dev, USHORT addr, ULONG bit, int on)
{
   ULONG v = rtw_read32(dev, addr);
   if (on)
      v |= bit;
   else
      v &= ~bit;
   rtw_write32(dev, addr, v);
}

/* ---- TX path-A IQK (phy_PathA_IQK_8188E) ------------------------------ */
/* returns 0x01 if TX IQK OK. */
static UCHAR iqk_txa(usb_dev_t *dev)
{
   union { ULONG l; USHORT w[2]; } u;
   ULONG eac;
   USHORT e94, e9c;

   rtw_write32(dev, 0xe30, 0x10008c1cUL);
   rtw_write32(dev, 0xe34, 0x30008c1cUL);
   rtw_write32(dev, 0xe38, 0x8214032aUL);
   rtw_write32(dev, 0xe3c, 0x28160000UL);
   rtw_write32(dev, 0xe4c, 0x00462911UL);        /* LO cal setting          */
   rtw_write32(dev, 0xe48, 0xf9000000UL);        /* one shot LOK & IQK      */
   rtw_write32(dev, 0xe48, 0xf8000000UL);
   usb_mdelay(10);

   eac = rtw_read32(dev, 0xeac);
   u.l = rtw_read32(dev, 0xe94); e94 = (USHORT)(u.w[1] & 0x03FF);
   u.l = rtw_read32(dev, 0xe9c); e9c = (USHORT)(u.w[1] & 0x03FF);
   u.l = eac;
   /* !(eac & BIT28) && (e94 != 0x142) && (e9c != 0x42); BIT28 = hi-word 0x1000 */
   if (!(u.w[1] & 0x1000) && e94 != 0x142 && e9c != 0x42)
      return 0x01;
   return 0x00;
}

/* ---- RX path-A IQK (phy_PathA_RxIQK) ---------------------------------- */
/* returns 0x01 (Tx ok) or 0x03 (Tx+Rx ok). RF writes go over path-A LSSI
 * (0x840) as (addr<<20)|data - values precomputed. */
static UCHAR iqk_rxa(usb_dev_t *dev)
{
   union { ULONG l; USHORT w[2]; } u, t;
   ULONG eac;
   USHORT e94, e9c, ea4;
   UCHAR result = 0;

   /* modify RXIQK mode table (RF: WE_LUT/RCK_OS/TXPA_G1/G2, PA+PAD off) */
   rtw_write32(dev, 0xe28, 0x00000000UL);
   rtw_write32(dev, 0x840, 0x0EF800A0UL);        /* RF 0xEF = 0x800a0       */
   rtw_write32(dev, 0x840, 0x03030000UL);        /* RF 0x30 = 0x30000       */
   rtw_write32(dev, 0x840, 0x0310000FUL);        /* RF 0x31 = 0x0000f       */
   rtw_write32(dev, 0x840, 0x032F117BUL);        /* RF 0x32 = 0xf117b       */
   rtw_write32(dev, 0x840, 0x0DF00980UL);        /* RF 0xdf = 0x980         */
   rtw_write32(dev, 0x840, 0x05651000UL);        /* RF 0x56 = 0x51000       */
   rtw_write32(dev, 0xe28, 0x80800000UL);

   rtw_write32(dev, 0xe40, 0x01007c00UL);        /* IQK setting             */
   rtw_write32(dev, 0xe44, 0x81004800UL);
   rtw_write32(dev, 0xe30, 0x10008c1cUL);        /* path-A IQK setting      */
   rtw_write32(dev, 0xe34, 0x30008c1cUL);
   rtw_write32(dev, 0xe38, 0x82160c1fUL);
   rtw_write32(dev, 0xe3c, 0x28160000UL);
   rtw_write32(dev, 0xe4c, 0x0046a911UL);        /* LO cal setting          */
   rtw_write32(dev, 0xe48, 0xf9000000UL);        /* one shot                */
   rtw_write32(dev, 0xe48, 0xf8000000UL);
   usb_mdelay(10);

   eac = rtw_read32(dev, 0xeac);
   u.l = rtw_read32(dev, 0xe94); e94 = (USHORT)(u.w[1] & 0x03FF);
   u.l = rtw_read32(dev, 0xe9c); e9c = (USHORT)(u.w[1] & 0x03FF);
   u.l = eac;
   if (!(u.w[1] & 0x1000) && e94 != 0x142 && e9c != 0x42)   /* BIT28 */
      result |= 0x01;
   else
      return result;                             /* Tx not OK -> ignore Rx  */

   /* u4tmp = 0x80007C00 | (e94<<16) | e9c */
   t.w[1] = (USHORT)(0x8000 | e94);
   t.w[0] = (USHORT)(0x7C00 | e9c);
   rtw_write32(dev, 0xe40, t.l);

   /* 1 RX IQK - modify RXIQK mode table 2 */
   rtw_write32(dev, 0xe28, 0x00000000UL);
   rtw_write32(dev, 0x840, 0x0EF800A0UL);
   rtw_write32(dev, 0x840, 0x03030000UL);
   rtw_write32(dev, 0x840, 0x0310000FUL);
   rtw_write32(dev, 0x840, 0x032F7FFAUL);        /* RF 0x32 = 0xf7ffa       */
   rtw_write32(dev, 0xe28, 0x80800000UL);
   rtw_write32(dev, 0xe44, 0x01004800UL);
   rtw_write32(dev, 0xe30, 0x38008c1cUL);
   rtw_write32(dev, 0xe34, 0x18008c1cUL);
   rtw_write32(dev, 0xe38, 0x82160c05UL);
   rtw_write32(dev, 0xe3c, 0x28160c1fUL);
   rtw_write32(dev, 0xe4c, 0x0046a911UL);
   rtw_write32(dev, 0xe48, 0xf9000000UL);
   rtw_write32(dev, 0xe48, 0xf8000000UL);
   usb_mdelay(10);

   eac = rtw_read32(dev, 0xeac);
   u.l = rtw_read32(dev, 0xea4); ea4 = (USHORT)(u.w[1] & 0x03FF);

   rtw_write32(dev, 0xe28, 0x00000000UL);        /* reload RF 0xdf          */
   rtw_write32(dev, 0x840, 0x0DF00180UL);        /* RF 0xdf = 0x180         */

   u.l = eac;
   /* !(eac & BIT27) && (ea4 != 0x132) && ((eac>>16 & 0x3FF) != 0x36) */
   if (!(u.w[1] & 0x0800) && ea4 != 0x132 && (USHORT)(u.w[1] & 0x03FF) != 0x36)
      result |= 0x02;
   return result;
}

/* ---- apply the path-A result (patha_fill_iqk) ------------------------- */
static void iqk_fill_a(usb_dev_t *dev, int txonly)
{
   union { ULONG l; USHORT w[2]; UCHAR b[4]; } u, o;
   USHORT oldval, tx0a, tx0c;
   int    X, Y;
   UCHAR  bE;

   /* Oldval_0 = (read(0xc80) >> 22) & 0x3FF = (hi-word >> 6) & 0x3FF */
   u.l = rtw_read32(dev, 0xc80);
   oldval = (USHORT)((u.w[1] >> 6) & 0x03FF);

   /* X = result[0] sign-extended to 16-bit int; TX0_A = (X*oldval) >> 8 */
   X = g_res[0];
   if (X & 0x0200) X |= 0xFC00;
   imul_s(X, oldval);
   u.l = g_prod;
   tx0a = (USHORT)(u.b[1] | (u.b[2] << 8));       /* (X*oldval) >> 8         */
   bE   = (UCHAR)((u.b[0] >> 7) & 1);             /* (X*oldval >> 7) & 1     */
   bb_rmw(dev, 0xc80, 0x000003FFUL, (ULONG)(tx0a & 0x03FF));
   bb_bit(dev, 0xc4c, 0x80000000UL, bE);          /* rOFDM0_ECCAThreshold B31*/

   /* Y = result[1]; TX0_C = (Y*oldval) >> 8 */
   Y = g_res[1];
   if (Y & 0x0200) Y |= 0xFC00;
   imul_s(Y, oldval);
   u.l = g_prod;
   tx0c = (USHORT)(u.b[1] | (u.b[2] << 8));
   bE   = (UCHAR)((u.b[0] >> 7) & 1);
   /* 0xc94 bits31:28 = (TX0_C & 0x3C0) >> 6 */
   o.w[0] = 0; o.w[1] = (USHORT)(((tx0c & 0x03C0) >> 6) << 12);
   bb_rmw(dev, 0xc94, 0xF0000000UL, o.l);
   /* 0xc80 bits21:16 = TX0_C & 0x3F */
   o.w[0] = 0; o.w[1] = (USHORT)(tx0c & 0x003F);
   bb_rmw(dev, 0xc80, 0x003F0000UL, o.l);
   bb_bit(dev, 0xc4c, 0x20000000UL, bE);          /* ECCAThreshold BIT29     */

   if (txonly)
      return;

   /* RX: 0xc14 bits9:0 = result[2]; bits15:10 = result[3]&0x3F; 0xca0
    * bits31:28 = (result[3]>>6)&0xF */
   bb_rmw(dev, 0xc14, 0x000003FFUL, (ULONG)(g_res[2] & 0x03FF));
   o.w[1] = 0; o.w[0] = (USHORT)((g_res[3] & 0x3F) << 10);
   bb_rmw(dev, 0xc14, 0x0000FC00UL, o.l);
   o.w[0] = 0; o.w[1] = (USHORT)(((g_res[3] >> 6) & 0x0F) << 12);
   bb_rmw(dev, 0xca0, 0xF0000000UL, o.l);
}

/* ---- top-level single-shot IQK --------------------------------------- */
/* COM1: 'I' start, then Tx result 'T<e94><e9c>', Rx result 'R<ea4><eac>',
 * ok-bits 'o<xx>', 'i' done. */
static USHORT g_iqk_ok = 0xFFFF;

int rtl8188eu_iqk(usb_dev_t *dev)
{
   union { ULONG l; USHORT w[2]; } u;
   USHORT i;
   UCHAR  txok, rxok, pi;

   tcom('I');

   /* tune channel 6 first (calibrate on the AP channel) */
   rtw_write32(dev, 0x840, 0x0180F406UL);

   for (i = 0; i < 4; i++) g_res[i] = 0;

   /* 1. save ADDA / MAC / BB */
   for (i = 0; i < 16; i++) g_addabk[i] = rtw_read32(dev, g_adda[i]);
   g_macbk[0] = rtw_read8(dev, g_macr[0]);
   g_macbk[1] = rtw_read8(dev, g_macr[1]);
   g_macbk[2] = rtw_read8(dev, g_macr[2]);
   g_macbk[3] = rtw_read32(dev, g_macr[3]);
   for (i = 0; i < 9; i++) g_bbbk[i] = rtw_read32(dev, g_bbr[i]);

   /* 2. path-A ADDA on (1T1R fixed values) */
   rtw_write32(dev, g_adda[0], 0x0b1b25a0UL);
   for (i = 1; i < 16; i++) rtw_write32(dev, g_adda[i], 0x0bdb25a0UL);

   /* 3. PI-mode: read RfPiEnable (0x820 BIT8); if off, switch BB to PI mode */
   u.l = rtw_read32(dev, 0x820);
   pi  = (UCHAR)((u.w[1] & 0x0001) ? 1 : 0);      /* BIT8 = hi-word bit0     */
   if (!pi) {
      rtw_write32(dev, 0x820, 0x01000100UL);
      rtw_write32(dev, 0x828, 0x01000100UL);
   }

   /* 4. BB setting for IQK */
   bb_bit(dev, 0x800, 0x01000000UL, 0);           /* rFPGA0_RFMOD BIT24 = 0  */
   rtw_write32(dev, 0xc04, 0x03a05600UL);
   rtw_write32(dev, 0xc08, 0x000800e4UL);
   rtw_write32(dev, 0x874, 0x22204000UL);
   bb_bit(dev, 0x870, 0x00000400UL, 1);           /* BIT10 */
   bb_bit(dev, 0x870, 0x04000000UL, 1);           /* BIT26 */
   bb_bit(dev, 0x860, 0x00000400UL, 0);           /* BIT10 */
   bb_bit(dev, 0x864, 0x00000400UL, 0);           /* BIT10 */

   /* 5. MAC setting for calibration */
   rtw_write8(dev, g_macr[0], 0x3F);
   rtw_write8(dev, g_macr[1], (UCHAR)(g_macbk[1] & ~0x08));   /* ~BIT3 */
   rtw_write8(dev, g_macr[2], (UCHAR)(g_macbk[2] & ~0x08));
   rtw_write8(dev, g_macr[3], (UCHAR)(g_macbk[3] & ~0x20));   /* ~BIT5 */

   /* 6. page-B init + IQK setting */
   rtw_write32(dev, 0xb68, 0x0f600000UL);         /* rConfig_AntA            */
   rtw_write32(dev, 0xe28, 0x80800000UL);         /* rFPGA0_IQK              */
   rtw_write32(dev, 0xe40, 0x01007c00UL);
   rtw_write32(dev, 0xe44, 0x81004800UL);

   /* 7. TX IQK (a couple of tries) */
   txok = 0;
   for (i = 0; i < 3; i++) {
      txok = iqk_txa(dev);
      if (txok == 0x01) {
         u.l = rtw_read32(dev, 0xe94); g_res[0] = (int)(u.w[1] & 0x03FF);
         u.l = rtw_read32(dev, 0xe9c); g_res[1] = (int)(u.w[1] & 0x03FF);
         break;
      }
   }
   tcom('T'); ih16((USHORT)g_res[0]); ih16((USHORT)g_res[1]);

   /* 8. RX IQK */
   rxok = 0;
   for (i = 0; i < 3; i++) {
      rxok = iqk_rxa(dev);
      if (rxok == 0x03) {
         u.l = rtw_read32(dev, 0xea4); g_res[2] = (int)(u.w[1] & 0x03FF);
         u.l = rtw_read32(dev, 0xeac); g_res[3] = (int)(u.w[1] & 0x03FF);
         break;
      }
   }
   tcom('R'); ih16((USHORT)g_res[2]); ih16((USHORT)g_res[3]);
   tcom('o'); ih16((USHORT)((txok << 4) | rxok));
   g_iqk_ok = (USHORT)((txok << 4) | rxok);

   /* 9. back to BB mode */
   rtw_write32(dev, 0xe28, 0x00000000UL);

   /* 10. restore (single-shot: do the t!=0 reload path unconditionally) */
   if (!pi) {
      rtw_write32(dev, 0x820, 0x01000000UL);
      rtw_write32(dev, 0x828, 0x01000000UL);
   }
   for (i = 0; i < 16; i++) rtw_write32(dev, g_adda[i], g_addabk[i]);
   rtw_write8(dev, g_macr[0], (UCHAR)g_macbk[0]);
   rtw_write8(dev, g_macr[1], (UCHAR)g_macbk[1]);
   rtw_write8(dev, g_macr[2], (UCHAR)g_macbk[2]);
   rtw_write32(dev, g_macr[3], g_macbk[3]);
   for (i = 0; i < 9; i++) rtw_write32(dev, g_bbr[i], g_bbbk[i]);
   rtw_write32(dev, 0x840, 0x00032ed3UL);         /* restore RX initial gain */
   rtw_write32(dev, 0xe30, 0x01008c00UL);         /* 0xe30 IQC default       */
   rtw_write32(dev, 0xe50, 0x01008c00UL);

   /* 11. apply the correction (only if TX IQK produced a value). The registers
    * the fill touches are saved before and after so the correction can be
    * switched on/off at runtime (rtl8188eu_iqk_set) for A/B tests. */
   for (i = 0; i < 5; i++) g_iqk_pre[i] = rtw_read32(dev, g_iqk_reg[i]);
   if (g_res[0] != 0)
      iqk_fill_a(dev, (g_res[2] == 0));            /* txonly if RX IQK failed */
   for (i = 0; i < 5; i++) g_iqk_post[i] = rtw_read32(dev, g_iqk_reg[i]);

   tcom('i');
   return 0;
}

/* IQ-calibration outcome for the diagnostics: 0 = ok bits (tx << 4 | rx), 1..4 = the four result values. */
USHORT rtl8188eu_iqk_info(USHORT i)
{
   if (i == 0) return g_iqk_ok;
   if (i <= 4) return (USHORT)g_res[i - 1];
   return 0xFFFF;
}

/* Switch the IQ-calibration correction on (post values) or off (pre values). */
void rtl8188eu_iqk_set(usb_dev_t *dev, int on)
{
   USHORT i;

   for (i = 0; i < 5; i++)
      rtw_write32(dev, g_iqk_reg[i], on ? g_iqk_post[i] : g_iqk_pre[i]);
}
