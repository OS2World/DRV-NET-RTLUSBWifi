/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtw_phy.c - RTL8821AE chip layer: eFuse, MAC / baseband / AGC / RF tables, band and channel switch, transmit power,
 * LC and IQ calibration, receiver gain.
 *
 * Ported from rtl8821au/src/rtl8821_phy.c and rtl8821_efuse.c of this project, which derive from the Realtek Linux
 * driver morrownr/8821au-20210708 (hal/rtl8812a/rtl8812a_phycfg.c, rtl8812a_hal_init.c, include/hal_pg.h),
 * Copyright(c) 2007 - 2017 Realtek Corporation, GPL-2.0-only. What changed for PCIe, checked against Linux rtlwifi
 * rtl8821ae (phy.c, hw.c, reg.h):
 *   - the BB block is enabled with FEN_PCIEA (SYS_FUNC_EN bit 6) instead of FEN_USBA (rtl8821ae_phy_bb_config);
 *   - the tables are the PCIe ones (rtw_tbl.c), and their microsecond delay codes are honoured (MMIO is fast);
 *   - the MAC address is at logical eFuse offset 0xD0 (EEPROM_MAC_ADDR), not 0x107 (the USB position);
 *   - the external antenna band switch (DPDT) of the single-antenna 8811AU dongles is not driven by default:
 *     rtl8821ae_phy_switch_wirelessband does not touch it (debug "bsw 1" turns it on for a board that needs it).
 */
#include "drv.h"
#include "rtw.h"

/* ---- baseband registers (Jaguar) ------------------------------------------------------------------------- */
#define rOFDMCCKEN           0x0808
#define rTxPath              0x080C
#define rCCK_RX              0x0A04
#define rFc_area             0x0860
#define rRFMOD               0x08AC
#define rADC_Buf_Clk         0x08C4
#define rL1PeakTH            0x0848
#define rA_TxScale           0x0C1C
#define rA_RFE_Pinmux        0x0CB0
#define rA_RFE_Inv           0x0CB4
#define RF_CHNLBW            0x18

ULONG  g_chk[8];
USHORT g_channel;
UCHAR  g_band = 2;
UCHAR  g_ext_bsw;                           /* 1 = drive the external 2.4 / 5 GHz antenna switch (8811AU boards)   */

/* ---- eFuse (rtl8821_efuse.c) ------------------------------------------------------------------------------ */
#define EF_PHYS_LEN        1024         /* EFUSE_REAL_CONTENT_LEN_JAGUAR */
#define EF_MAP_LEN         512
#define EE_XTAL            0xB9         /* EEPROM_XTAL_8821AE */
#define EE_THERMAL         0xBA
#define EE_PA_TYPE         0xBC
#define EE_LNA_2G          0xBD
#define EE_LNA_5G          0xBF
#define EE_RFE             0xCA
#define EE_MAC             0xD0         /* EEPROM_MAC_ADDR (PCIe) */

UCHAR  g_ef[EF_MAP_LEN];
UCHAR  g_ef_ok;
UCHAR  g_xtal = 0x20;                   /* EEPROM_Default_CrystalCap_8812 */
UCHAR  g_lna2g, g_pa2g, g_lna5g, g_pa5g;
UCHAR  g_rfe, g_thermal = 0xFF;
USHORT g_ef_used;

static void efuse_power(int on)
{
   USHORT v;

   if (on) {
      rtw_w8(REG_EFUSE_ACCESS, 0x69);                      /* unlock (efuse_power_switch)                   */
      v = rtw_r16(REG_SYS_FUNC_EN);                        /* FEN_ELDR                                      */
      if (!(v & 0x1000)) rtw_w16(REG_SYS_FUNC_EN, (USHORT)(v | 0x1000));
      v = rtw_r16(REG_SYS_CLKR);                           /* LOADER_CLK_EN | ANA8M                         */
      if ((v & 0x0022) != 0x0022) rtw_w16(REG_SYS_CLKR, (USHORT)(v | 0x0022));
   } else
      rtw_w8(REG_EFUSE_ACCESS, 0x00);
}

/* one physical byte (efuse_OneByteRead): address into 0x31 / 0x32[1:0], start (0x33 bit 7 = 0), wait for bit 7 */
static int efuse_byte(USHORT addr, UCHAR *out)
{
   USHORT i;
   UCHAR  b;

   rtw_w8(REG_EFUSE_CTRL + 1, (UCHAR)(addr & 0xFF));
   b = rtw_r8(REG_EFUSE_CTRL + 2);
   rtw_w8(REG_EFUSE_CTRL + 2, (UCHAR)(((addr >> 8) & 0x03) | (b & 0xFC)));
   b = rtw_r8(REG_EFUSE_CTRL + 3);
   rtw_w8(REG_EFUSE_CTRL + 3, (UCHAR)(b & 0x7F));
   for (i = 0; i < 1000; i++) {
      if (rtw_r8(REG_EFUSE_CTRL + 3) & 0x80)
         break;
      dd_udelay(2);
   }
   if (i >= 1000)
      return -1;
   *out = rtw_r8(REG_EFUSE_CTRL);
   return 0;
}

/* 1024 bytes of physical records decoded into the 512-byte logical map. 0 = decoded, -1 = read error. */
int rtw_efuse_load(void)
{
   static UCHAR hdr, ext, b0, b1;                 /* static: efuse_byte() writes through a near pointer (SS != DS) */
   USHORT addr = 0, off, i, w, base;
   UCHAR  wden, any, ff;
   int    rc = -1;

   for (i = 0; i < EF_MAP_LEN; i++) g_ef[i] = 0xFF;
   g_ef_ok = 0;
   efuse_power(1);
   if (efuse_byte(addr++, &hdr) != 0)
      goto out;
   while (hdr != 0xFF && addr < EF_PHYS_LEN) {
      if ((hdr & 0x1F) == 0x0F) {                 /* extended header */
         off = (USHORT)((hdr & 0xE0) >> 5);
         if (efuse_byte(addr++, &ext) != 0) goto out;
         if ((ext & 0x0F) == 0x0F) {              /* all words disabled: next header */
            if (efuse_byte(addr++, &hdr) != 0) goto out;
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
         if (efuse_byte(addr++, &b0) != 0 || efuse_byte(addr++, &b1) != 0)
            goto out;
         base = (USHORT)(off * 8 + w * 2);
         if (base + 1 < EF_MAP_LEN) {
            g_ef[base]     = b0;
            g_ef[base + 1] = b1;
         }
      }
      if (efuse_byte(addr++, &hdr) != 0) goto out;
   }
   g_ef_used = addr;
   rc = 0;
   if (g_ef[0] == 0xFF && g_ef[1] == 0xFF)
      goto out;                                   /* nothing programmed */
   g_ef_ok = 1;
   if (g_ef[EE_XTAL] != 0xFF)
      g_xtal = g_ef[EE_XTAL];
   g_thermal = g_ef[EE_THERMAL];
   g_pa2g  = (UCHAR)((g_ef[EE_PA_TYPE] != 0xFF && (g_ef[EE_PA_TYPE] & 0x10)) ? 1 : 0);
   g_lna2g = (UCHAR)((g_ef[EE_LNA_2G] != 0xFF && (g_ef[EE_LNA_2G] & 0x08)) ? 1 : 0);
   g_pa5g  = (UCHAR)((g_ef[EE_PA_TYPE] != 0xFF && (g_ef[EE_PA_TYPE] & 0x01)) ? 1 : 0);
   g_lna5g = (UCHAR)((g_ef[EE_LNA_5G] != 0xFF && (g_ef[EE_LNA_5G] & 0x08)) ? 1 : 0);
   g_rfe   = g_ef[EE_RFE];
   any = 0;
   ff = 1;
   for (i = 0; i < 6; i++) {
      any |= g_ef[EE_MAC + i];
      if (g_ef[EE_MAC + i] != 0xFF) ff = 0;
   }
   if (any && !ff && !(g_ef[EE_MAC] & 1))         /* a unicast, programmed address */
      for (i = 0; i < 6; i++)
         g_hw_mac[i] = g_ef[EE_MAC + i];
out:
   efuse_power(0);
   return rc;
}

/* ---- tables ------------------------------------------------------------------------------------------------- */
/* The last value the RF table wrote to a register. RF read-back is not reliable (the firmware uses the same read
 * interface), so read-modify-write sequences start from this instead of from the chip. */
static ULONG rf_tbl_last(UCHAR reg)
{
   USHORT i;

   for (i = g_rf_n; i > 0; i--)
      if (g_rf_tbl[i - 1].a == reg)
         return g_rf_tbl[i - 1].v;
   return 0UL;
}

/* The last value the BB table wrote to a register. */
static ULONG bb_tbl_val(USHORT reg)
{
   USHORT i;

   for (i = g_bb_n; i > 0; i--)
      if (g_bb_tbl[i - 1].a == reg)
         return g_bb_tbl[i - 1].v;
   return 0UL;
}

static ULONG g_rf18;                         /* what RF register 0x18 (channel, bandwidth) holds */

/* delay codes of the tables: 0xFE 50 ms, 0xFD 5 ms, 0xFC 1 ms, 0xFB 50 us, 0xFA 5 us, 0xF9 1 us */
static int tbl_delay(USHORT a)
{
   if (a == 0xFE) { rtw_mdelay(50); return 1; }
   if (a == 0xFD) { rtw_mdelay(5);  return 1; }
   if (a == 0xFC) { rtw_mdelay(1);  return 1; }
   if (a == 0xFB) { dd_udelay(50);  return 1; }
   if (a == 0xFA) { dd_udelay(5);   return 1; }
   if (a == 0xF9) { dd_udelay(1);   return 1; }
   return 0;
}

void rtw_mac_tables(void)                    /* PHY_MACConfig8812 */
{
   USHORT i;

   for (i = 0; i < g_mac_n; i++)
      rtw_w8(g_mac_tbl[i].a, g_mac_tbl[i].v);
}

void rtw_bb_init(void)                       /* rtl8821ae_phy_bb_config */
{
   USHORT i;
   UCHAR  v8;
   ULONG  cap;

   v8 = rtw_r8(REG_SYS_FUNC_EN);
   v8 |= 0x40;                                                           /* FEN_PCIEA */
   rtw_w8(REG_SYS_FUNC_EN, v8);
   rtw_w8(REG_SYS_FUNC_EN, (UCHAR)(v8 | 0x02 | 0x01));                   /* FEN_BB_GLB_RSTn | FEN_BBRSTB */
   rtw_w8(REG_RF_CTRL, 0x07);                                            /* path A RF power on */
   rtw_w8(REG_OPT_CTRL + 2, 0x07);
   for (i = 0; i < g_bb_n; i++) {                                        /* PHY_REG table */
      if (tbl_delay(g_bb_tbl[i].a))
         continue;
      rtw_w32(g_bb_tbl[i].a, g_bb_tbl[i].v);
      dd_udelay(1);
   }
   for (i = 0; i < g_agc_n; i++) {                                       /* AGC table */
      if (tbl_delay(g_agc_tbl[i].a))
         continue;
      rtw_w32(g_agc_tbl[i].a, g_agc_tbl[i].v);
      dd_udelay(1);
   }
   /* crystal capacitor: 0x2C[23:18] = 0x2C[17:12] = cap */
   cap = (ULONG)(g_xtal & 0x3F);
   cap = cap | (cap << 6);
   bb_set(REG_CRYSTAL, 0x00FFF000UL, cap);
}

void rtw_rf_init(void)
{
   USHORT i;

   for (i = 0; i < g_rf_n; i++) {
      if (tbl_delay(g_rf_tbl[i].a))
         continue;
      rf_write(g_rf_tbl[i].a, g_rf_tbl[i].v);
   }
}

/* ---- band, bandwidth and channel ---------------------------------------------------------------------------- */
/* phydm_set_ext_band_switch_8821A: single-antenna boards route the antenna to the 2.4 or the 5 GHz path with an
 * external switch (DPDT) driven from the chip. Off unless g_ext_bsw (debug "bsw 1"). */
static void ext_band_switch(UCHAR five)
{
   ULONG v;

   if (!g_ext_bsw)
      return;
   v = rtw_r32(0x004C);
   v = (v & ~0x00800000UL) | 0x01000000UL;                               /* DPDT_P / DPDT_N controlled by WLAN */
   rtw_w32(0x004C, v);
   bb_set(rA_RFE_Inv, 0x0000000FUL, 7UL);
   bb_set(rA_RFE_Inv, 0x000000F0UL, 7UL);
   bb_set(rA_RFE_Inv, 0x30000000UL, five ? 2UL : 1UL);
}

/* PHY_SwitchWirelessBand8812 / rtl8821ae_phy_switch_wirelessband, 2.4 GHz */
void rtw_band_2g(void)
{
   ext_band_switch(0);
   g_band = 0;
   bb_set(rOFDMCCKEN, 0x30000000UL, 3UL);                                /* OFDM and CCK enabled */
   bb_set(rA_RFE_Pinmux, 0xF000UL, 7UL);                                 /* LNA_On, PAPE_A */
   bb_set(rA_RFE_Pinmux, 0x00F0UL, 7UL);
   if (g_lna2g) {                                                        /* phy_SetRFEReg8821: external 2.4 GHz LNA */
      bb_set(rA_RFE_Inv, 0x00100000UL, 1UL);
      bb_set(rA_RFE_Inv, 0x00400000UL, 0UL);
      bb_set(rA_RFE_Pinmux, 0x0007UL, 2UL);
      bb_set(rA_RFE_Pinmux, 0x0700UL, 2UL);
   } else {
      bb_set(rA_RFE_Inv, 0x00100000UL, 0UL);
      bb_set(rA_RFE_Inv, 0x00400000UL, 0UL);
      bb_set(rA_RFE_Pinmux, 0x0007UL, 7UL);
      bb_set(rA_RFE_Pinmux, 0x0700UL, 7UL);
   }
   bb_set(rA_TxScale, 0x0F00UL, 0UL);                                    /* AGC table select */
   bb_set(rTxPath, 0x00F0UL, 1UL);
   bb_set(rCCK_RX, 0x0F000000UL, 1UL);
   rtw_w8(REG_CCK_CHECK, (UCHAR)(rtw_r8(REG_CCK_CHECK) & ~0x80));
   bb_set(rA_TxScale, 0xFFE00000UL, 0x200UL);                            /* TX BB swing 0 dB */
}

/* the same, 5 GHz */
static void rtw_band_5g(void)
{
   USHORT n;
   ULONG  v;

   ext_band_switch(1);
   g_band = 1;
   bb_set(rA_RFE_Pinmux, 0xF000UL, 5UL);
   bb_set(rA_RFE_Pinmux, 0x00F0UL, 4UL);
   bb_set(rA_RFE_Inv, 0x00100000UL, 0UL);
   bb_set(rA_RFE_Inv, 0x00400000UL, 0UL);
   bb_set(rA_RFE_Pinmux, 0x0007UL, 7UL);
   bb_set(rA_RFE_Pinmux, 0x0700UL, 7UL);
   rtw_w8(REG_CCK_CHECK, (UCHAR)(rtw_r8(REG_CCK_CHECK) | 0x80));
   for (n = 0; n < 50; n++) {                                            /* wait for the TX packet buffers to drain */
      if ((rtw_r16(0x041A) & 0x30) == 0x30)
         break;
      dd_udelay(50);
   }
   bb_set(rOFDMCCKEN, 0x30000000UL, 3UL);
   bb_set(rA_TxScale, 0x0F00UL, 1UL);                                    /* AGC table select: 5 GHz */
   bb_set(rTxPath, 0x00F0UL, 0UL);
   bb_set(rCCK_RX, 0x0F000000UL, 0xFUL);
   v = rtw_r32(REG_RRSR);                                                /* no CCK rates in the response rate set */
   rtw_w32(REG_RRSR, (v & ~0x000FFFFFUL) | 0x000FFFF0UL);
   bb_set(rA_TxScale, 0xFFE00000UL, 0x200UL);
}

/* phy_PostSetBwMode8812 for 20 MHz, 1T1R */
void rtw_bw20(void)
{
   rtw_w16(REG_WMAC_TRXPTCL_CTL, (USHORT)(rtw_r16(REG_WMAC_TRXPTCL_CTL) & 0xFE7F));
   rtw_w8(REG_DATA_SC, 0);
   bb_set(rRFMOD, 0x003003C3UL, 0x00300200UL);
   bb_set(rADC_Buf_Clk, 0x40000000UL, 0UL);
   bb_set(rL1PeakTH, 0x03C00000UL, 8UL);
}

/* ---- transmit power ------------------------------------------------------------------------------------------
 * As rtl8821au/src/rtl8821_phy.c: 5 GHz OFDM / HT / VHT indexes per channel group from the eFuse (base + OFDM /
 * BW20 differences); 2.4 GHz keeps the BB table values. Debug "txp 0" / "txp 1". */
UCHAR g_txp5 = 1;
UCHAR g_txp_ofdm, g_txp_ht;
UCHAR g_txp_grp = 0xFF;

static UCHAR grp5(UCHAR ch)
{
   if (ch < 36)   return 0xFF;
   if (ch <= 42)  return 0;
   if (ch <= 48)  return 1;
   if (ch <= 58)  return 2;
   if (ch <= 64)  return 3;
   if (ch < 100)  return 0xFF;
   if (ch <= 106) return 4;
   if (ch <= 114) return 5;
   if (ch <= 122) return 6;
   if (ch <= 130) return 7;
   if (ch <= 138) return 8;
   if (ch <= 144) return 9;
   if (ch < 149)  return 0xFF;
   if (ch <= 155) return 10;
   if (ch <= 161) return 11;
   if (ch <= 171) return 12;
   if (ch <= 177) return 13;
   return 0xFF;
}

static int sx4(UCHAR n)                       /* signed 4-bit field in the low nibble */
{
   n &= 0x0F;
   return (n & 8) ? (int)n - 16 : (int)n;
}

static void txp_write(UCHAR ofdm, UCHAR ht)
{
   static USHORT regs[6] = { 0x0C24, 0x0C28, 0x0C2C, 0x0C30, 0x0C3C, 0x0C40 };
   U32    u;
   USHORT i, k;

   for (i = 0; i < 6; i++) {
      if (ofdm == 0)
         u.l = bb_tbl_val(regs[i]);
      else
         for (k = 0; k < 4; k++)
            u.b[k] = (UCHAR)(i < 2 ? ofdm : ht);
      rtw_w32(regs[i], u.l);
   }
   u.l = bb_tbl_val(0x0C44);                     /* VHT 1SS MCS8, MCS9 | 2SS MCS0, MCS1 */
   if (ofdm != 0) {
      u.b[0] = ht;
      u.b[1] = ht;
   }
   rtw_w32(0x0C44, u.l);
   g_txp_ofdm = ofdm;
   g_txp_ht   = ofdm ? ht : 0;
}

static void txpower(UCHAR ch)
{
   UCHAR g = 0xFF, base = 0xFF, d;
   int   o, h;

   if (ch > 14 && g_txp5 && g_ef_ok)
      g = grp5(ch);
   if (g != 0xFF)
      base = g_ef[0x22 + g];
   if (base > 0x3F) {                            /* 2.4 GHz, switched off, no eFuse, or not programmed */
      g_txp_grp = 0xFF;
      if (g_txp_ofdm != 0)
         txp_write(0, 0);
      return;
   }
   d = g_ef[0x30];
   o = base;
   h = base;
   if (d != 0xFF) {
      o += sx4(d);
      h += sx4((UCHAR)(d >> 4));
   }
   if (o < 1) o = 1;
   if (o > 0x3F) o = 0x3F;
   if (h < 1) h = 1;
   if (h > 0x3F) h = 0x3F;
   g_txp_grp = g;
   if ((UCHAR)o != g_txp_ofdm || (UCHAR)h != g_txp_ht)
      txp_write((UCHAR)o, (UCHAR)h);
}

void rtw_txpower_set(UCHAR on)
{
   g_txp5 = on;
   txpower((UCHAR)g_channel);
}

/* phy_SwChnl8812 */
void rtw_set_channel(UCHAR ch)
{
   ULONG fc, mod;

   if (ch > 14 && g_band != 1)
      rtw_band_5g();
   else if (ch <= 14 && g_band != 0)
      rtw_band_2g();
   if (36 <= ch && ch <= 48)        fc = 0x494UL;
   else if (50 <= ch && ch <= 64)   fc = 0x453UL;
   else if (100 <= ch && ch <= 116) fc = 0x452UL;
   else if (118 <= ch)              fc = 0x412UL;
   else                             fc = 0x96AUL;
   bb_set(rFc_area, 0x1FFE0000UL, fc);
   if (36 <= ch && ch <= 64)        mod = 0x00010100UL;                  /* RF_MOD_AG (bits 18:16, 9:8) = 0x101 */
   else if (100 <= ch && ch <= 140) mod = 0x00030100UL;                  /* 0x301 */
   else if (140 < ch)               mod = 0x00050100UL;                  /* 0x501 */
   else                             mod = 0UL;
   /* RF 0x18: the table value with RF_MOD_AG for the band, bandwidth bits [11:10] = 3 (20 MHz), the channel in bits 7:0 */
   g_rf18 = (rf_tbl_last(RF_CHNLBW) & ~0x00070FFFUL) | mod | 0x00000C00UL | (ULONG)ch;
   rf_write(RF_CHNLBW, g_rf18);
   g_channel = ch;
   txpower(ch);
}

/* LC calibration (phy_lc_calibrate_8812a), from the RF table values and the channel register shadow */
void rtw_lck(void)
{
   ULONG b4 = rf_tbl_last(0xB4);

   rtw_w8(REG_TXPAUSE, 0xFF);
   rf_write(0xB4, b4 | 0x4000UL);                                        /* enter LCK mode */
   rf_write(RF_CHNLBW, g_rf18 | 0x8000UL);                               /* start */
   rtw_mdelay(200);
   rf_write(RF_CHNLBW, g_rf18);
   rf_write(0xB4, b4);
   rtw_w8(REG_TXPAUSE, 0x00);
}

/* ---- IQ calibration, run by the firmware ----------------------------------------------------------------------
 * As rtl8821au/src/rtl8821_phy.c: H2C 0x45 (channel; band in bits 5:4 and 20 MHz in bits 3:0; external 5 GHz PA /
 * LNA), then up to 500 ms for the firmware's C2H 0x11 (IQK_FINISH, rtw_c2h). Linux rtlwifi calibrates on the host
 * instead; the firmware of the vendor driver (RTL8821AU.FW, version 42) does it on request. A firmware that does not
 * know the command never answers: after the first timeout the driver stops asking (g_iqk_mode = 0). */
UCHAR          g_iqk_mode = 1;
volatile UCHAR g_iqk_done;
UCHAR          g_iqk_c2h = 0xFF;
USHORT         g_iqk_sent, g_iqk_ok, g_iqk_tmo, g_iqk_ms, g_iqk_ch;

void rtw_iqk(UCHAR ch)
{
   ULONG  t0;
   USHORT n;

   if (g_iqk_mode == 0 || (g_iqk_mode == 1 && ch <= 14))
      return;
   g_iqk_done = 0;
   g_iqk_c2h = 0xFF;
   g_iqk_ch = ch;
   if (rtw_h2c(0x45, ch, (UCHAR)((ch > 14 ? 0x20 : 0x10) | 0x01), (UCHAR)(g_pa5g | (g_lna5g << 1))) != 0)
      return;
   g_iqk_sent++;
   t0 = dd_ms();
   for (n = 0; n < 100 && !g_iqk_done; n++) {
      hw_service();                              /* the report arrives as a received packet or a C2H register event */
      rtw_mdelay(5);
   }
   g_iqk_ms = (USHORT)(dd_ms() - t0);
   if (!g_iqk_done) {
      g_iqk_tmo++;
      g_iqk_mode = 0;                            /* this firmware does not calibrate on request */
      tstr("[rtw: firmware IQK did not report, off]");
      return;
   }
   g_iqk_ok++;
}

/* ---- receiver gain (IGI, BB 0xC50 bits 6:0) ------------------------------------------------------------------
 * As rtl8821au/src/rtl_rx.c (rtl8188eu_dig_tick): target = signal (dBm) + 90 between 0x20 and 0x3A, at most 4 per
 * step, every 2 s; scans use 0x20 (hear everything). */
UCHAR g_dig_auto = 1;
UCHAR g_igi = 0x20;
static ULONG g_dig_next;

void rtw_dig_set(UCHAR v)
{
   if (v == g_igi)
      return;
   rtw_w8(0x0C50, v);
   g_igi = v;
}

void rtw_dig_tick(void)
{
   ULONG now = dd_ms();
   int   t, d, r;

   if (!g_dig_auto || !g_hw_ready || (long)(now - g_dig_next) < 0L)
      return;
   g_dig_next = now + 2000UL;
   t = 0x20;
   if (g_joined) {
      r = sta_rssi();
      if (r < 0) {
         t = r + 90;
         if (t < 0x20) t = 0x20;
         if (t > 0x3A) t = 0x3A;
      }
   }
   d = t - (int)g_igi;
   if (d >= -1 && d <= 1)
      return;
   if (d > 4)  d = 4;
   if (d < -4) d = -4;
   rtw_dig_set((UCHAR)((int)g_igi + d));
}
