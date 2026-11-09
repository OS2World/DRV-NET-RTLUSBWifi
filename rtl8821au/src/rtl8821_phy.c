/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8821AU driver for OS/2 and ArcaOS.
 *
 * Derived from the Realtek Linux driver (morrownr/8821au-20210708: hal/rtl8812a/usb/usb_halinit.c,
 * hal/rtl8812a/rtl8812a_phycfg.c, hal/rtl8812a/rtl8812a_hal_init.c), Copyright(c) 2007 - 2017 Realtek Corporation,
 * GPL-2.0-only.
 *
 * rtl8821_phy.c - RTL8821A bring-up, phase 3: MAC / DMA setup, baseband + AGC + RF-A tables, band and channel switch.
 *
 * Order follows rtl8812au_hal_init() after the firmware download:
 *   MAC table -> queue pages / boundaries / priorities -> WMAC / EDCA / beacon / USB settings -> MAC TX+RX enable
 *   -> BB (function enable, PHY_REG table, AGC table, crystal cap) -> RF A table -> 2.4 GHz band + channel + 20 MHz.
 * Only 32-bit operations the 16-bit compiler inlines are used (no variable shifts, no multiply / divide).
 */
#include "rt_usb.h"
#include "rtl8821_tbl.h"

/* ---- registers --------------------------------------------------------------------------------------- */
#define REG_SYS_FUNC_EN      0x0002
#define REG_RF_CTRL          0x001F
#define REG_CR               0x0100
#define REG_PBP              0x0104
#define REG_TRXDMA_CTRL      0x010C
#define REG_TRXFF_BNDY       0x0114
#define REG_RQPN             0x0200
#define REG_TDECTRL          0x0208
#define REG_RQPN_NPQ         0x0214
#define REG_OPT_CTRL         0x0074
#define REG_CRYSTAL          0x002C
#define REG_BCNQ_BDNY        0x0424
#define REG_MGQ_BDNY         0x0425
#define REG_WMAC_LBK_BF_HD   0x045D
#define REG_HIQ_NO_LMT_EN    0x05A7
#define REG_RX_DRVINFO_SZ    0x060F
#define REG_RCR              0x0608
#define REG_MAR              0x0620
#define REG_RXFLTMAP1        0x06A2
#define REG_RRSR             0x0440
#define REG_SPEC_SIFS        0x0428
#define REG_RETRY_LIMIT      0x042A
#define REG_MAC_SPEC_SIFS    0x063A
#define REG_SIFS_CTX         0x0514
#define REG_SIFS_TRX         0x0516
#define REG_EDCA_BE_PARAM    0x0508
#define REG_EDCA_BK_PARAM    0x050C
#define REG_EDCA_VI_PARAM    0x0504
#define REG_EDCA_VO_PARAM    0x0500
#define REG_USTIME_TSF       0x055C
#define REG_USTIME_EDCA      0x0638
#define REG_FWHW_TXQ_CTRL    0x0420
#define REG_ACKTO            0x0640
#define REG_RXDMA_STATUS     0x0288
#define REG_AMPDU_MAX_TIME   0x0456
#define REG_AMPDU_MAX_LENGTH 0x0458
#define REG_RXDMA_PRO        0x0290
#define REG_HT_SINGLE_AMPDU  0x04C7
#define REG_RX_PKT_LIMIT     0x060C
#define REG_PIFS             0x0512
#define REG_MAX_AGGR_NUM     0x04CA
#define REG_FAST_EDCA_CTRL   0x0460
#define REG_ARFR0            0x0444
#define REG_ARFR1            0x044C
#define REG_ARFR2            0x048C
#define REG_ARFR3            0x0494
#define REG_HIMR0            0x00B0
#define REG_HIMR1            0x00B8
#define REG_USB_SPECIAL_OPT  0xFE55
#define REG_HWSEQ_CTRL       0x0423
#define REG_BAR_MODE_CTRL    0x04CC
#define REG_QUEUE_CTRL       0x04C6
#define REG_EARLY_MODE_CTRL  0x02BC
#define REG_TX_RPT_TIME      0x04F0
#define REG_SDIO_CTRL        0x0070
#define REG_ACLK_MON         0x003E
#define REG_USB_HRPWM        0xFE58
#define REG_CCK_CHECK        0x0454
#define REG_DATA_SC          0x0483
#define REG_WMAC_TRXPTCL_CTL 0x0668
#define REG_BCN_CTRL         0x0550
#define REG_TBTT_PROHIBIT    0x0540
#define REG_DRVERLYINT       0x0558
#define REG_BCNDMATIM        0x0559
#define REG_BCNTCFG          0x0510
#define REG_CAMCMD           0x0670

/* ---- baseband / RF registers (Jaguar) ------------------------------------------------------------------ */
#define rOFDMCCKEN           0x0808
#define rTxPath              0x080C
#define rCCK_RX              0x0A04
#define rFc_area             0x0860
#define rRFMOD               0x08AC
#define rADC_Buf_Clk         0x08C4
#define rL1PeakTH            0x0848
#define rHSSIRead            0x08B0
#define rA_TxScale           0x0C1C
#define rA_LSSIWrite         0x0C90
#define rA_RFE_Pinmux        0x0CB0
#define rA_RFE_Inv           0x0CB4
#define rA_SIRead            0x0D08
#define rA_PIRead            0x0D04
#define RF_CHNLBW            0x18

#define TX_PAGE_BOUNDARY     248            /* TX_PAGE_BOUNDARY_8821: (0xFF - 8 beacon pages) + 1 */
#define RX_DMA_BOUNDARY      0x3E7F         /* MAX_RX_DMA_BUFFER_SIZE_8821 (0x3E80) - 1 */
#define TX_TOTAL_PAGES       247            /* TX_TOTAL_PAGE_NUMBER_8821 */
#define PG_HPQ               8              /* NORMAL_PAGE_NUM_HPQ_8821 */
#define PG_LPQ               8
#define PG_NPQ               0
#define PG_EPQ               4


/* ---- results for the diagnostics (rtl_dhcp.c) -------------------------------------------------------- */
USHORT g_r21_phy_stage;                     /* 1 mac tbl, 2 mac init, 3 bb, 4 agc, 5 rf, 6 band, 7 channel, 8 done */
ULONG  g_r21_chk[8];                        /* register read-backs after the init (see r21_phy_init) */
USHORT g_r21_channel;                       /* channel currently set (0 = none) */

/* ---- 32-bit helpers built from 16-bit operations ------------------------------------------------------- */
typedef union { ULONG l; USHORT w[2]; UCHAR b[4]; } r21_u32;

static ULONG shl1(ULONG v)
{
   static r21_u32 x;

   x.l = v;
   x.w[1] = (USHORT)((x.w[1] << 1) | (x.w[0] >> 15));
   x.w[0] = (USHORT)(x.w[0] << 1);
   return x.l;
}

static ULONG shr1(ULONG v)
{
   static r21_u32 x;

   x.l = v;
   x.w[0] = (USHORT)((x.w[0] >> 1) | (x.w[1] << 15));
   x.w[1] = (USHORT)(x.w[1] >> 1);
   return x.l;
}

/* BB register: value in the bits selected by mask (rmw), like PHY_SetBBReg8812 */
static void bb_set(usb_dev_t *dev, USHORT reg, ULONG mask, ULONG data)
{
   ULONG v, m;

   if (mask != 0xFFFFFFFFUL) {
      v = rtw_read32(dev, reg);
      m = mask;
      while (!(m & 1UL)) {                 /* move data to the lowest set bit of the mask */
         m = shr1(m);
         data = shl1(data);
      }
      v = (v & ~mask) | (data & mask);
      rtw_write32(dev, reg, v);
   } else
      rtw_write32(dev, reg, data);
}

static ULONG bb_get(usb_dev_t *dev, USHORT reg, ULONG mask)
{
   ULONG v = rtw_read32(dev, reg) & mask;

   while (!(mask & 1UL)) {
      mask = shr1(mask);
      v = shr1(v);
   }
   return v;
}

/* RF path A register write: (offset << 20 | data[19:0]) into the 3-wire register of the BB */
static void rf_write(usb_dev_t *dev, UCHAR reg, ULONG data)
{
   static r21_u32 x;

   x.l = data;
   x.w[1] = (USHORT)((x.w[1] & 0x000F) | ((USHORT)reg << 4));
   rtw_write32(dev, rA_LSSIWrite, x.l);
}

/* RF path A register read (phy_RFSerialRead for the 8821: no CCA toggling, 20 us settle) */
static ULONG rf_read(usb_dev_t *dev, UCHAR reg)
{
   ULONG v;
   USHORT pi;

   pi = (USHORT)bb_get(dev, 0x0C00, 0x4UL);
   bb_set(dev, rHSSIRead, 0xFFUL, (ULONG)reg);
   usb_mdelay(1);
   v = bb_get(dev, pi ? rA_PIRead : rA_SIRead, 0xFFFFFUL);
   return v;
}

/* The last value the RF table wrote to a register. RF read-back is not reliable (the firmware uses the same read interface),
 * so read-modify-write sequences start from this instead of from the chip. */
static ULONG rf_tbl_last(UCHAR reg)
{
   USHORT i;

   for (i = g_r21_rf_n; i > 0; i--)
      if (g_r21_rf_tbl[i - 1].a == reg)
         return g_r21_rf_tbl[i - 1].v;
   return 0UL;
}

static ULONG g_rf18;                         /* what RF register 0x18 (channel, bandwidth) holds */

/* delay codes of the tables: 0xFE 50 ms, 0xFD 5 ms, 0xFC 1 ms; the microsecond codes are shorter than a USB transfer */
static int tbl_delay(USHORT a)
{
   if (a == 0xFE) { usb_mdelay(50); return 1; }
   if (a == 0xFD) { usb_mdelay(5);  return 1; }
   if (a == 0xFC) { usb_mdelay(1);  return 1; }
   if (a == 0xFB || a == 0xFA || a == 0xF9) return 1;
   return 0;
}

/* ---- MAC / DMA / WMAC / USB setup (the _Init*_8812A functions) --------------------------------------------- */
static void r21_mac_init(usb_dev_t *dev)
{
   USHORT v16;
   UCHAR  v8, pro;
   ULONG  v32;

   /* _InitQueueReservedPage_8821AUsb (no WiFi-spec config, all four queues in use) */
   rtw_write32(dev, REG_RQPN_NPQ, (ULONG)PG_NPQ | ((ULONG)PG_EPQ << 16));
   rtw_write32(dev, REG_RQPN, (ULONG)PG_HPQ | ((ULONG)PG_LPQ << 8) |
               ((ULONG)(TX_TOTAL_PAGES - PG_HPQ - PG_LPQ - PG_NPQ - PG_EPQ) << 16) | 0x80000000UL);
   /* _InitTxBufferBoundary_8821AUsb */
   rtw_write8(dev, REG_BCNQ_BDNY, TX_PAGE_BOUNDARY);
   rtw_write8(dev, REG_MGQ_BDNY, TX_PAGE_BOUNDARY);
   rtw_write8(dev, REG_WMAC_LBK_BF_HD, TX_PAGE_BOUNDARY);
   rtw_write8(dev, REG_TRXFF_BNDY, TX_PAGE_BOUNDARY);
   rtw_write8(dev, REG_TDECTRL + 1, TX_PAGE_BOUNDARY);
   /* _InitQueuePriority (four OUT endpoints): BE,BK -> low (1); VI,VO -> normal (2); MGT -> extra (0); HI -> high (3) */
   v16 = (USHORT)(rtw_read16(dev, REG_TRXDMA_CTRL) & 0x0007);
   v16 |= (USHORT)((3 << 14) | (0 << 12) | (1 << 10) | (1 << 8) | (2 << 6) | (2 << 4));
   rtw_write16(dev, REG_TRXDMA_CTRL, v16);
   rtw_write8(dev, REG_HIQ_NO_LMT_EN, 0xFF);
   /* _InitPageBoundary_8812AUsb */
   rtw_write16(dev, REG_TRXFF_BNDY + 2, RX_DMA_BOUNDARY);
   g_r21_phy_stage = 2;

   /* _InitDriverInfoSize_8812A: PHY status of 4 * 8 bytes with every received frame */
   rtw_write8(dev, REG_RX_DRVINFO_SZ, 4);
   /* _InitInterrupt_8812AU: no interrupt sources enabled, interrupt-IN endpoint selected for USB 2 */
   rtw_write32(dev, REG_HIMR0, 0UL);
   rtw_write32(dev, REG_HIMR1, 0UL);
   rtw_write8(dev, REG_USB_SPECIAL_OPT, (UCHAR)(rtw_read8(dev, REG_USB_SPECIAL_OPT) & ~0x10));   /* INT_BULK_SEL = 0: events on the interrupt endpoint, RX data on bulk-IN (this dongle has the interrupt-IN endpoint) */
   /* _InitNetworkType_8812A: link type AP (as the vendor driver does at this point) */
   v32 = rtw_read32(dev, REG_CR);
   v32 = (v32 & ~0x00030000UL) | 0x00020000UL;
   rtw_write32(dev, REG_CR, v32);
   /* _InitWMACSetting_8812A: RCR = APM|AM|AB|CBSSID_DATA|CBSSID_BCN|APP_ICV|AMF|HTC_LOC_CTRL|APP_MIC|APP_PHYST_RXFF|FORCEACK */
   rtw_write32(dev, REG_RCR, 0x740060CEUL);
   rtw_write32(dev, REG_MAR, 0xFFFFFFFFUL);
   rtw_write32(dev, REG_MAR + 4, 0xFFFFFFFFUL);
   rtw_write16(dev, REG_RXFLTMAP1, 0x0400);
   /* _InitAdaptiveCtrl_8812AUsb: response rate set 1M + all OFDM/HT/VHT basic rates, retry limits 0x30 */
   v32 = rtw_read32(dev, REG_RRSR);
   v32 = (v32 & ~0x000FFFFFUL) | 0x000FFFF1UL;
   rtw_write32(dev, REG_RRSR, v32);
   rtw_write16(dev, REG_SPEC_SIFS, 0x1010);
   rtw_write16(dev, REG_RETRY_LIMIT, 0x3030);
   /* _InitEDCA_8812AUsb */
   rtw_write16(dev, REG_SPEC_SIFS, 0x100A);
   rtw_write16(dev, REG_MAC_SPEC_SIFS, 0x100A);
   rtw_write16(dev, REG_SIFS_CTX, 0x100A);
   rtw_write16(dev, REG_SIFS_TRX, 0x100A);
   rtw_write32(dev, REG_EDCA_BE_PARAM, 0x005EA42BUL);
   rtw_write32(dev, REG_EDCA_BK_PARAM, 0x0000A44FUL);
   rtw_write32(dev, REG_EDCA_VI_PARAM, 0x005EA324UL);
   rtw_write32(dev, REG_EDCA_VO_PARAM, 0x002FA226UL);
   rtw_write8(dev, REG_USTIME_TSF, 0x50);
   rtw_write8(dev, REG_USTIME_EDCA, 0x50);
   /* _InitRetryFunction_8812A */
   v8 = rtw_read8(dev, REG_FWHW_TXQ_CTRL);
   rtw_write8(dev, REG_FWHW_TXQ_CTRL, (UCHAR)(v8 | 0x80));
   rtw_write8(dev, REG_ACKTO, 0x80);
   /* init_UsbAggregationSetting_8812A: no TX / RX aggregation. TDECTRL is written by _InitBurstPktLen below. */
   /* _InitBeaconParameters_8812A */
   rtw_write16(dev, REG_BCN_CTRL, 0x1010);
   rtw_write8(dev, REG_TBTT_PROHIBIT, 0x04);
   rtw_write8(dev, REG_TBTT_PROHIBIT + 1, 0x64);
   rtw_write8(dev, REG_TBTT_PROHIBIT + 2, (UCHAR)(rtw_read8(dev, REG_TBTT_PROHIBIT + 2) & 0xF0));
   rtw_write8(dev, REG_DRVERLYINT, 0x05);
   rtw_write8(dev, REG_BCNDMATIM, 0x02);
   rtw_write16(dev, REG_BCNTCFG, 0x4413);
   /* _InitBurstPktLen (8821U: USB 2 branch) */
   rtw_write8(dev, 0xF050, 0x01);
   rtw_write16(dev, REG_RXDMA_STATUS, 0x7400);
   rtw_write8(dev, 0x0289, 0xF5);
   rtw_write8(dev, REG_AMPDU_MAX_TIME, 0x5E);
   rtw_write32(dev, REG_AMPDU_MAX_LENGTH, 0xFFFFFFFFUL);
   rtw_write8(dev, REG_USTIME_TSF, 0x50);
   rtw_write8(dev, REG_USTIME_EDCA, 0x50);
   v8 = rtw_read8(dev, 0xFE17);
   pro = rtw_read8(dev, REG_RXDMA_PRO);
   if (((v8 >> 4) & 0x03) == 0)
      rtw_write8(dev, REG_RXDMA_PRO, (UCHAR)((pro | 0x1E) & ~0x20));     /* burst packet length 512 bytes */
   else
      rtw_write8(dev, REG_RXDMA_PRO, (UCHAR)((pro | 0x2E) & ~0x10));     /* 64 bytes */
   rtw_write8(dev, REG_TDECTRL, 0x10);
   rtw_write8(dev, REG_HT_SINGLE_AMPDU, (UCHAR)(rtw_read8(dev, REG_HT_SINGLE_AMPDU) | 0x80));
   rtw_write8(dev, REG_RX_PKT_LIMIT, 0x18);
   rtw_write8(dev, REG_PIFS, 0x00);
   rtw_write16(dev, REG_MAX_AGGR_NUM, 0x1F1F);
   rtw_write8(dev, REG_FWHW_TXQ_CTRL, 0x80);
   rtw_write32(dev, REG_FAST_EDCA_CTRL, 0x03087777UL);
   rtw_write8(dev, 0x001C, (UCHAR)(rtw_read8(dev, 0x001C) | 0x60));
   rtw_write32(dev, REG_ARFR0, 0x00000010UL);
   rtw_write32(dev, REG_ARFR0 + 4, 0xFFFFF000UL);
   rtw_write32(dev, REG_ARFR1, 0x00000010UL);
   rtw_write32(dev, REG_ARFR1 + 4, 0x003FF000UL);
   rtw_write32(dev, REG_ARFR2, 0x00000015UL);
   rtw_write32(dev, REG_ARFR2 + 4, 0x003FF000UL);
   rtw_write32(dev, REG_ARFR3, 0x00000015UL);
   rtw_write32(dev, REG_ARFR3 + 4, 0xFFCFF000UL);
   /* MAC TX / RX enable (after the RX FIFO boundary is set) */
   rtw_write8(dev, REG_CR, (UCHAR)(rtw_read8(dev, REG_CR) | 0xC0));
}

/* ---- baseband / RF tables -------------------------------------------------------------------------------- */
static void r21_bb_init(usb_dev_t *dev)
{
   USHORT i;
   UCHAR  v8;
   ULONG  cap;

   v8 = rtw_read8(dev, REG_SYS_FUNC_EN);
   v8 |= 0x04;                                                           /* FEN_USBA */
   rtw_write8(dev, REG_SYS_FUNC_EN, v8);
   rtw_write8(dev, REG_SYS_FUNC_EN, (UCHAR)(v8 | 0x02 | 0x01));          /* FEN_BB_GLB_RSTn | FEN_BBRSTB */
   rtw_write8(dev, REG_RF_CTRL, 0x07);                                   /* path A RF power on */
   rtw_write8(dev, REG_OPT_CTRL + 2, 0x07);                              /* path B RF power on (as the vendor driver) */
   g_r21_phy_stage = 3;

   for (i = 0; i < g_r21_bb_n; i++) {                                    /* PHY_REG table */
      if (tbl_delay(g_r21_bb_tbl[i].a))
         continue;
      rtw_write32(dev, g_r21_bb_tbl[i].a, g_r21_bb_tbl[i].v);
   }
   g_r21_phy_stage = 4;
   for (i = 0; i < g_r21_agc_n; i++) {                                   /* AGC table */
      if (tbl_delay(g_r21_agc_tbl[i].a))
         continue;
      rtw_write32(dev, g_r21_agc_tbl[i].a, g_r21_agc_tbl[i].v);
   }
   /* crystal capacitor: 0x2C[23:18] = 0x2C[17:12] = cap */
   cap = (ULONG)(g_r21_xtal & 0x3F);
   cap = cap | (cap << 6);
   bb_set(dev, REG_CRYSTAL, 0x00FFF000UL, cap);
}

static void r21_rf_init(usb_dev_t *dev)
{
   USHORT i;

   for (i = 0; i < g_r21_rf_n; i++) {
      if (tbl_delay(g_r21_rf_tbl[i].a))
         continue;
      rf_write(dev, g_r21_rf_tbl[i].a, g_r21_rf_tbl[i].v);
   }
}

/* ---- band, bandwidth and channel ---------------------------------------------------------------------------- */
/* PHY_SwitchWirelessBand8812, 2.4 GHz branch for an 8821 with an MP chip and no external LNA */
static UCHAR g_band = 2;                    /* 0 = 2.4 GHz, 1 = 5 GHz, 2 = not set yet */

/* phydm_set_ext_band_switch_8821A: the single-antenna 8811AU boards route the antenna to the 2.4 or the 5 GHz path with an
 * external switch (DPDT) driven from the chip; the vendor driver switches it with the band (drv_ant_band_switch = 1). */
static void r21_ext_band_switch(usb_dev_t *dev, UCHAR five)
{
   ULONG v = rtw_read32(dev, 0x004C);

   v = (v & ~0x00800000UL) | 0x01000000UL;                               /* DPDT_P / DPDT_N are outputs controlled by WLAN */
   rtw_write32(dev, 0x004C, v);
   bb_set(dev, rA_RFE_Inv, 0x0000000FUL, 7UL);
   bb_set(dev, rA_RFE_Inv, 0x000000F0UL, 7UL);
   bb_set(dev, rA_RFE_Inv, 0x30000000UL, five ? 2UL : 1UL);
}

static void r21_band_2g(usb_dev_t *dev)
{
   r21_ext_band_switch(dev, 0);
   g_band = 0;
   bb_set(dev, rOFDMCCKEN, 0x30000000UL, 3UL);                           /* OFDM and CCK enabled */
   bb_set(dev, rA_RFE_Pinmux, 0xF000UL, 7UL);                            /* phy_SetRFEReg8821: PA and LNA off ... */
   bb_set(dev, rA_RFE_Pinmux, 0x00F0UL, 7UL);
   if (g_r21_lna2g) {                                                    /* ... external 2.4 GHz LNA in use */
      bb_set(dev, rA_RFE_Inv, 0x00100000UL, 1UL);
      bb_set(dev, rA_RFE_Inv, 0x00400000UL, 0UL);
      bb_set(dev, rA_RFE_Pinmux, 0x0007UL, 2UL);
      bb_set(dev, rA_RFE_Pinmux, 0x0700UL, 2UL);
   } else {                                                              /* ... external LNA bypassed */
      bb_set(dev, rA_RFE_Inv, 0x00100000UL, 0UL);
      bb_set(dev, rA_RFE_Inv, 0x00400000UL, 0UL);
      bb_set(dev, rA_RFE_Pinmux, 0x0007UL, 7UL);
      bb_set(dev, rA_RFE_Pinmux, 0x0700UL, 7UL);
   }
   bb_set(dev, rA_TxScale, 0x0F00UL, 0UL);                               /* AGC table select */
   bb_set(dev, rTxPath, 0x00F0UL, 1UL);                                  /* CCK false-alarm workaround */
   bb_set(dev, rCCK_RX, 0x0F000000UL, 1UL);
   rtw_write8(dev, REG_CCK_CHECK, (UCHAR)(rtw_read8(dev, REG_CCK_CHECK) & ~0x80));
   bb_set(dev, rA_TxScale, 0xFFE00000UL, 0x200UL);                       /* TX BB swing 0 dB */
}

/* PHY_SwitchWirelessBand8812, 5 GHz branch for an 8821 with an MP chip */
static void r21_band_5g(usb_dev_t *dev)
{
   USHORT n;
   ULONG  v;

   r21_ext_band_switch(dev, 1);
   g_band = 1;
   bb_set(dev, rA_RFE_Pinmux, 0xF000UL, 5UL);                            /* phy_SetRFEReg8821: PA and LNA on */
   bb_set(dev, rA_RFE_Pinmux, 0x00F0UL, 4UL);
   bb_set(dev, rA_RFE_Inv, 0x00100000UL, 0UL);
   bb_set(dev, rA_RFE_Inv, 0x00400000UL, 0UL);
   bb_set(dev, rA_RFE_Pinmux, 0x0007UL, 7UL);
   bb_set(dev, rA_RFE_Pinmux, 0x0700UL, 7UL);
   rtw_write8(dev, REG_CCK_CHECK, (UCHAR)(rtw_read8(dev, REG_CCK_CHECK) | 0x80));
   for (n = 0; n < 50; n++) {                                            /* wait for the TX packet buffers to drain */
      if ((rtw_read16(dev, 0x041A) & 0x30) == 0x30)
         break;
      usb_mdelay(1);
   }
   bb_set(dev, rOFDMCCKEN, 0x30000000UL, 3UL);
   bb_set(dev, rA_TxScale, 0x0F00UL, 1UL);                               /* AGC table select: 5 GHz */
   bb_set(dev, rTxPath, 0x00F0UL, 0UL);
   bb_set(dev, rCCK_RX, 0x0F000000UL, 0xFUL);
   v = rtw_read32(dev, REG_RRSR);                                        /* no CCK rates in the response rate set */
   rtw_write32(dev, REG_RRSR, (v & ~0x000FFFFFUL) | 0x000FFFF0UL);
   bb_set(dev, rA_TxScale, 0xFFE00000UL, 0x200UL);                       /* TX BB swing 0 dB (eFuse / by-rate values later) */
}

/* phy_PostSetBwMode8812 for 20 MHz, 1T1R */
static void r21_bw20(usb_dev_t *dev)
{
   rtw_write16(dev, REG_WMAC_TRXPTCL_CTL, (USHORT)(rtw_read16(dev, REG_WMAC_TRXPTCL_CTL) & 0xFE7F));
   rtw_write8(dev, REG_DATA_SC, 0);
   bb_set(dev, rRFMOD, 0x003003C3UL, 0x00300200UL >> 0);
   bb_set(dev, rADC_Buf_Clk, 0x40000000UL, 0UL);
   bb_set(dev, rL1PeakTH, 0x03C00000UL, 8UL);
   /* the RF bandwidth bits [11:10] = 3 are part of the RF 0x18 value written by rtl8821_set_channel */
}

/* ---- transmit power ----------------------------------------------------------------------------------------
 * The BB table leaves one power index per rate group on both bands (0xC20 CCK 0x2C, 0xC24 / 0xC28 OFDM 0x30, 0xC2C-0xC38
 * HT 0x2C, 0xC3C-0xC4C VHT 0x2A; one byte per rate, 0.5 dB steps). Realtek's drivers set it per channel from the eFuse
 * (hal_load_pg_txpwr_info_path_5g and phy_get_pg_txpwr_idx in hal_com_phycfg.c; the Linux build has the by-rate table
 * off, CONFIG_TXPWR_BY_RATE_EN = n, so every rate of a section gets the same index). 5 GHz, path A, 1T:
 *   logical 0x22-0x2F  BW40 base index of the 14 channel groups (rtw_get_ch_group)
 *   logical 0x30       BW20 difference (bits 7:4) and OFDM difference (bits 3:0), signed 4 bit
 *   OFDM index = base + OFDM difference; HT / VHT 20 MHz index = base + BW20 difference; at most 0x3F.
 * Only 5 GHz is set from the eFuse (R21_TXP5 in rt_usb.h; debug "txp 0" / "txp 1"): 2.4 GHz keeps the table values,
 * which work, and leaving 5 GHz switches back to them. A base above 0x3F (an unprogrammed eFuse) keeps the table values
 * too. Linux reads an unprogrammed difference byte (0xFF) as -1 / -1; here it counts as 0. */
UCHAR g_r21_txp5 = R21_TXP5;            /* 1 = 5 GHz power from the eFuse */
UCHAR g_r21_txp_ofdm, g_r21_txp_ht;     /* indexes the registers hold now; 0 = the BB table values */
UCHAR g_r21_txp_grp = 0xFF;             /* eFuse channel group in use (0xFF = none) */

static UCHAR r21_grp5(UCHAR ch)
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

static int r21_sx4(UCHAR n)              /* signed 4-bit field in the low nibble */
{
   n &= 0x0F;
   return (n & 8) ? (int)n - 16 : (int)n;
}

/* The last value the BB table wrote to a register. */
static ULONG bb_tbl_val(USHORT reg)
{
   USHORT i;

   for (i = g_r21_bb_n; i > 0; i--)
      if (g_r21_bb_tbl[i - 1].a == reg)
         return g_r21_bb_tbl[i - 1].v;
   return 0UL;
}

/* Write the OFDM index (0xC24 / 0xC28) and the 1SS HT / VHT index (0xC2C, 0xC30, 0xC3C, 0xC40, 0xC44 bytes 0-1);
 * ofdm = 0 puts the BB table values back. The 2SS bytes and CCK (0xC20) are left as the table has them. */
static void r21_txp_write(usb_dev_t *dev, UCHAR ofdm, UCHAR ht)
{
   static USHORT regs[6] = { 0x0C24, 0x0C28, 0x0C2C, 0x0C30, 0x0C3C, 0x0C40 };
   static union { ULONG l; UCHAR b[4]; } u;
   USHORT i, k;

   for (i = 0; i < 6; i++) {
      if (ofdm == 0)
         u.l = bb_tbl_val(regs[i]);
      else
         for (k = 0; k < 4; k++)
            u.b[k] = (UCHAR)(i < 2 ? ofdm : ht);
      rtw_write32(dev, regs[i], u.l);
   }
   u.l = bb_tbl_val(0x0C44);                     /* VHT 1SS MCS8, MCS9 | 2SS MCS0, MCS1 */
   if (ofdm != 0) {
      u.b[0] = ht;
      u.b[1] = ht;
   }
   rtw_write32(dev, 0x0C44, u.l);
   g_r21_txp_ofdm = ofdm;
   g_r21_txp_ht   = ofdm ? ht : 0;
}

static void r21_txpower(usb_dev_t *dev, UCHAR ch)
{
   UCHAR g = 0xFF, base = 0xFF, d;
   int   o, h;

   if (ch > 14 && g_r21_txp5 && g_r21_ef_ok)
      g = r21_grp5(ch);
   if (g != 0xFF)
      base = g_r21_ef[0x22 + g];
   if (base > 0x3F) {                            /* 2.4 GHz, switched off, no eFuse, or not programmed */
      g_r21_txp_grp = 0xFF;
      if (g_r21_txp_ofdm != 0)
         r21_txp_write(dev, 0, 0);
      return;
   }
   d = g_r21_ef[0x30];
   o = base;
   h = base;
   if (d != 0xFF) {
      o += r21_sx4(d);
      h += r21_sx4((UCHAR)(d >> 4));
   }
   if (o < 1) o = 1;                             /* 0 would read as "table values" here; index 1 is -31 dB anyway */
   if (o > 0x3F) o = 0x3F;
   if (h < 1) h = 1;
   if (h > 0x3F) h = 0x3F;
   g_r21_txp_grp = g;
   if ((UCHAR)o != g_r21_txp_ofdm || (UCHAR)h != g_r21_txp_ht)
      r21_txp_write(dev, (UCHAR)o, (UCHAR)h);
}

/* Debug "txp N": switch the 5 GHz eFuse power on (1) or off (0) and apply it to the current channel. */
void rtl8821_txpower_set(usb_dev_t *dev, UCHAR on)
{
   g_r21_txp5 = on;
   r21_txpower(dev, (UCHAR)g_r21_channel);
}

/* phy_SwChnl8812 for a 2.4 GHz channel */
void rtl8821_set_channel(usb_dev_t *dev, UCHAR ch)
{
   ULONG fc, mod;

   if (ch > 14 && g_band != 1)
      r21_band_5g(dev);
   else if (ch <= 14 && g_band != 0)
      r21_band_2g(dev);
   if (36 <= ch && ch <= 48)        fc = 0x494UL;
   else if (50 <= ch && ch <= 64)   fc = 0x453UL;
   else if (100 <= ch && ch <= 116) fc = 0x452UL;
   else if (118 <= ch)              fc = 0x412UL;
   else                             fc = 0x96AUL;
   bb_set(dev, rFc_area, 0x1FFE0000UL, fc);
   if (36 <= ch && ch <= 64)        mod = 0x00010100UL;                  /* RF_MOD_AG (bits 18:16, 9:8) = 0x101 */
   else if (100 <= ch && ch <= 140) mod = 0x00030100UL;                  /* 0x301 */
   else if (140 < ch)               mod = 0x00050100UL;                  /* 0x501 */
   else                             mod = 0UL;
   /* RF 0x18: the table value with RF_MOD_AG for the band, bandwidth bits [11:10] = 3 (20 MHz) and the channel in the low byte. */
   g_rf18 = (rf_tbl_last(RF_CHNLBW) & ~0x00070FFFUL) | mod | 0x00000C00UL | (ULONG)ch;
   rf_write(dev, RF_CHNLBW, g_rf18);
   g_r21_channel = ch;
   r21_txpower(dev, ch);
}

/* debug-channel access to the RF path A registers */
ULONG rtl8821_rf_read(usb_dev_t *dev, UCHAR reg)
{
   return rf_read(dev, reg);
}

void rtl8821_rf_write(usb_dev_t *dev, UCHAR reg, ULONG data)
{
   rf_write(dev, reg, data);
}

/* ---- phase 4: receive path --------------------------------------------------------------------------------- */
/* Accept management frames (beacons, probe responses) from every BSSID, plus data / control for later phases.
 * RCR: AAP|APM|AM|AB (0x0F) + ADF (bit 11) + AMF (bit 13) + APP_PHYST_RXFF (bit 28): the value that worked on the 8188EU. */
#define R21_RX_RCR   0x1000280FUL

void rtl8821_rx_enable(usb_dev_t *dev)
{
   USHORT i;
   UCHAR  v8;

   v8 = rtw_read8(dev, REG_TRXDMA_CTRL);
   rtw_write8(dev, REG_TRXDMA_CTRL, (UCHAR)(v8 & ~0x04));               /* RXDMA_AGG_EN off: one frame stream, no USB RX aggregation */
   for (i = 0; i < 6; i++)
      rtw_write8(dev, (USHORT)(0x0610 + i), g_mymac[i]);                /* MACID */
   rtw_write16(dev, 0x06A0, 0xFFFF);                                    /* RX filter maps: management, control, data */
   rtw_write16(dev, 0x06A2, 0xFFFF);
   rtw_write16(dev, 0x06A4, 0xFFFF);
   rtw_write32(dev, REG_RCR, R21_RX_RCR);
   g_r21_chk[7] = rtw_read32(dev, REG_RCR);
}

/* ---- the whole phase-3 init, called after the firmware is running ------------------------------------------- */
/* LC calibration (phy_lc_calibrate_8812a), written from the RF table values and the channel register shadow */
void rtl8821_lck(usb_dev_t *dev)
{
   ULONG b4 = rf_tbl_last(0xB4);

   rtw_write8(dev, 0x0522, 0xFF);                                        /* pause TX */
   rf_write(dev, 0xB4, b4 | 0x4000UL);                                   /* enter LCK mode */
   rf_write(dev, RF_CHNLBW, g_rf18 | 0x8000UL);                          /* start */
   usb_mdelay(150);
   usb_mdelay(50);
   rf_write(dev, RF_CHNLBW, g_rf18);
   rf_write(dev, 0xB4, b4);                                              /* leave LCK mode */
   rtw_write8(dev, 0x0522, 0x00);
}

USHORT g_r21_hold;                                                       /* 1 = the background scan does nothing (experiments) */
USHORT g_r21_norx = 0;                                                   /* 1 = do not start the async receive engine at init (debug) */

static UCHAR g_h2c_box;                  /* next firmware command box (r21_h2c below); 0 after a firmware load */

int rtl8821_phy_init(usb_dev_t *dev)
{
   USHORT i;

   g_r21_phy_stage = 0;
   g_r21_txp_ofdm = 0;                                                   /* the BB table below sets the power registers again */
   g_r21_txp_ht = 0;
   g_h2c_box = 0;                                                        /* the firmware was just loaded: it starts at box 0 */
   rtl8821_efuse_load(dev);                                              /* crystal trim, PA / LNA flags, MAC address */
   for (i = 0; i < g_r21_mac_n; i++)                                     /* PHY_MACConfig8812 */
      rtw_write8(dev, g_r21_mac_tbl[i].a, g_r21_mac_tbl[i].v);
   g_r21_phy_stage = 1;
   r21_mac_init(dev);
   r21_bb_init(dev);
   g_r21_phy_stage = 5;
   r21_rf_init(dev);
   g_r21_phy_stage = 6;
   r21_band_2g(dev);
   rtl8821_set_channel(dev, 1);
   r21_bw20(dev);
   g_r21_phy_stage = 7;

   /* remaining writes of rtl8812au_hal_init */
   rtw_write32(dev, REG_CAMCMD, 0xC0000000UL);                           /* invalidate all security CAM entries */
   rtw_write8(dev, REG_HWSEQ_CTRL, 0xFF);
   rtw_write32(dev, REG_BAR_MODE_CTRL, 0x0201FFFFUL);
   rtw_write8(dev, 0x0652, 0x00);
   rtw_write8(dev, REG_QUEUE_CTRL, (UCHAR)(rtw_read8(dev, REG_QUEUE_CTRL) & 0xF7));
   rtw_write8(dev, REG_FWHW_TXQ_CTRL + 1, 0x0F);
   rtw_write8(dev, REG_EARLY_MODE_CTRL + 3, 0x01);
   rtw_write16(dev, REG_TX_RPT_TIME, 0x3DF0);
   rtw_write8(dev, REG_SDIO_CTRL, 0x00);
   rtw_write8(dev, REG_ACLK_MON, 0x00);
   rtw_write8(dev, REG_USB_HRPWM, 0x00);

   /* read-backs: BB 0x800, 0x808, 0xC1C, 0xC50, RF 0x00, RF 0x18, MAC CR, RCR */
   g_r21_chk[0] = rtw_read32(dev, 0x0800);
   g_r21_chk[1] = rtw_read32(dev, 0x0808);
   g_r21_chk[2] = rtw_read32(dev, 0x0C1C);
   g_r21_chk[3] = rtw_read32(dev, 0x0C50);
   g_r21_chk[4] = rf_read(dev, 0x00);
   g_r21_chk[5] = rf_read(dev, RF_CHNLBW);
   g_r21_chk[6] = rtw_read32(dev, REG_CR);
   g_r21_chk[7] = rtw_read32(dev, REG_RCR);
   rtl8821_rx_enable(dev);
   g_r21_phy_stage = 8;
   return 0;
}

/* ---- stand-ins for the 8188EU chip layer that the shared code still calls --------------------------------- */
int rtl8188eu_card_disable(usb_dev_t *dev)
{
   (void)dev;                                    /* the 8821 power-off flow is not ported yet */
   return 0;
}

/* ---- firmware commands (H2C) ------------------------------------------------------------------------------
 * fill_h2c_cmd_8812 (rtl8812a_cmd.c, CONFIG_H2C_EF): four mailboxes, used in turn. A box is free when its bit in
 * REG_HMETFR is clear (the firmware clears it when it has read the command). The extension box (0x1F0 + 4 * n, bytes 4-7
 * of a longer command) is written first, then the box itself (0x1D0 + 4 * n): byte 0 = command id, bytes 1-3 = data.
 * Byte writes, as the Linux driver does them. Task time only. */
#define REG_HMETFR       0x01CC
#define REG_HMEBOX_0     0x01D0
#define REG_HMEBOX_EXT0  0x01F0

USHORT        g_r21_h2c_busy;                    /* commands dropped because the box stayed busy */

static int r21_h2c(usb_dev_t *dev, UCHAR id, UCHAR b1, UCHAR b2, UCHAR b3)
{
   UCHAR  box = g_h2c_box, bit = (UCHAR)(1 << box);
   USHORT n, mb = (USHORT)(REG_HMEBOX_0 + (box << 2)), xb = (USHORT)(REG_HMEBOX_EXT0 + (box << 2));

   for (n = 0; n < 100 && (rtw_read8(dev, REG_HMETFR) & bit); n++)
      usb_mdelay(1);
   if (n >= 100) {
      g_r21_h2c_busy++;
      return -1;
   }
   for (n = 0; n < 4; n++)
      rtw_write8(dev, (USHORT)(xb + n), 0);
   rtw_write8(dev, mb, id);
   rtw_write8(dev, (USHORT)(mb + 1), b1);
   rtw_write8(dev, (USHORT)(mb + 2), b2);
   rtw_write8(dev, (USHORT)(mb + 3), b3);
   g_h2c_box = (UCHAR)((box + 1) & 3);
   return 0;
}

/* ---- IQ calibration, run by the firmware ----------------------------------------------------------------------
 * For an 8821A without Bluetooth coexistence the Linux driver does not calibrate on the host: rtl8812_init_dm_priv sets
 * RegIQKFWOffload, and phy_iq_calibrate_8821a then sends H2C 0x45 (_phy_iq_calibrate_by_fw_8821a) and waits up to 500 ms
 * for the firmware's C2H 0x11 (IQK_FINISH, seen by rx_c2h in rtl_rx.c). The firmware writes the TX / RX IQ correction
 * itself. Command data: channel; band in bits 5:4 (1 = 2.4 GHz, 2 = 5 GHz) and bandwidth in bits 3:0 (1 = 20 MHz);
 * external 5 GHz PA (bit 0) and LNA (bit 1). RTL8821AU.FW is the non-Bluetooth image (30880 bytes, Realtek's
 * array_mp_8821a_fw_nic), the one Linux uses this way.
 * Run after the join has tuned to the access point's channel (Linux sets bNeedIQK on a join, rtw_mlme_ext.c).
 * R21_IQK in rt_usb.h, debug "iqk N": 0 = off, 1 = 5 GHz joins only (2.4 GHz works without it), 2 = every join.
 * The read-out shows the correction registers before and after: 0xC10 (RX), and 0xCCC / 0xCD4 on BB page C1 (TX);
 * phy_reset_iqk_result_8821a gives their "no correction" values (0xC10[9:0] 0x100, 0xCCC[10:0] 0, 0xCD4[10:0] 0x200). */
UCHAR          g_r21_iqk = R21_IQK;
volatile UCHAR g_r21_iqk_done;                   /* set by the C2H IQK_FINISH report */
UCHAR          g_r21_iqk_c2h = 0xFF;             /* its first data byte */
USHORT         g_r21_iqk_sent, g_r21_iqk_ok, g_r21_iqk_tmo, g_r21_iqk_ms, g_r21_iqk_ch;
ULONG          g_r21_iqc[6];                     /* before: 0xC10, 0xCCC, 0xCD4; after: the same (0xFFFFFFFF = not read) */

static void r21_iqc_read(usb_dev_t *dev, ULONG *r)
{
   r[0] = rtw_read32(dev, 0x0C10);
   bb_set(dev, 0x082C, 0x80000000UL, 1UL);       /* BB page C1 */
   r[1] = rtw_read32(dev, 0x0CCC);
   r[2] = rtw_read32(dev, 0x0CD4);
   bb_set(dev, 0x082C, 0x80000000UL, 0UL);       /* back to page C */
}

void rtl8821_iqk(usb_dev_t *dev, UCHAR ch)
{
   ULONG  t0;
   USHORT n;

   if (g_r21_iqk == 0 || (g_r21_iqk == 1 && ch <= 14))
      return;
   r21_iqc_read(dev, &g_r21_iqc[0]);
   g_r21_iqc[3] = g_r21_iqc[4] = g_r21_iqc[5] = 0xFFFFFFFFUL;
   g_r21_iqk_done = 0;
   g_r21_iqk_c2h = 0xFF;
   g_r21_iqk_ch = ch;
   if (r21_h2c(dev, 0x45, ch, (UCHAR)((ch > 14 ? 0x20 : 0x10) | 0x01), (UCHAR)(g_r21_pa5g | (g_r21_lna5g << 1))) != 0)
      return;
   g_r21_iqk_sent++;
   t0 = rtl8188eu_ms();
   for (n = 0; n < 100 && !g_r21_iqk_done; n++)  /* up to 500 ms, as phydm_iqk_wait */
      usb_mdelay(5);
   g_r21_iqk_ms = (USHORT)(rtl8188eu_ms() - t0);
   if (!g_r21_iqk_done) {
      g_r21_iqk_tmo++;                           /* the firmware may still be calibrating: leave BB page select alone */
      return;
   }
   g_r21_iqk_ok++;
   r21_iqc_read(dev, &g_r21_iqc[3]);
}

void rtl8821_iqk_set(usb_dev_t *dev, UCHAR mode)  /* debug "iqk N": set the mode and run it on the current channel */
{
   g_r21_iqk = mode;
   rtl8821_iqk(dev, (UCHAR)g_r21_channel);
}

/* Read-out lines IQOK / IQTX / IQTY / IQRX / IQRY (rtl_dhcp.c). */
USHORT rtl8188eu_iqk_info(USHORT i)
{
   if (i == 0) return g_r21_iqk_ok;              /* IQOK: calibrations the firmware reported finished */
   if (i == 1) return g_r21_iqk_sent;            /* IQTX: commands sent */
   if (i == 2) return g_r21_iqk_tmo;             /* IQTY: no finish report within 500 ms */
   if (i == 3) return g_r21_iqk_ms;              /* IQRX: duration of the last one, ms */
   if (i == 4) return g_r21_iqk_ch;              /* IQRY: its channel */
   return 0;
}
