/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtw_hal.c - RTL8821AE bring-up: device table, PCIe power-on flow, LLT, MAC / DMA setup, firmware download, hw_up /
 * hw_down, the status text and the debug commands of the back end.
 *
 * Order of rtl8821ae_hw_init (Linux rtlwifi rtl8821ae/hw.c; Copyright(c) 2009-2010 Realtek Corporation, GPL-2.0):
 *   power off a chip that is still on -> _rtl8821ae_init_mac (power-on flow for PCIe, LLT, ring addresses)
 *   -> firmware download -> MAC table -> BB + AGC tables -> RF table -> _rtl8821ae_hw_configure -> 2.4 GHz band
 *   -> CAM reset, security, MAC address -> release the RX / PCIe DMA.
 * The power sequences are those of rtl8821ae/pwrseq.h resolved for the PCIe interface and an MP chip (cut A), in the
 * table form of rtl8821au/src/rtl8821_hal.c; the LLT, firmware download and 8051 reset follow the same file (ported
 * from the Realtek vendor driver morrownr/8821au-20210708, GPL-2.0-only). The firmware image is the file written to
 * \DEV\WRND32$ at boot (os2/dbg.c keeps it in g_fwblob).
 */
#include "drv.h"
#include "rtw.h"

USHORT g_hw_ready, g_hw_failed, g_hw_state;
UCHAR  g_hw_mac[6];
USHORT g_rtw_step;
USHORT g_pwr_fail, g_llt_fail, g_fw_tries, g_fw_chksum, g_fw_ready, g_fw_ver, g_fw_sub;
ULONG  g_mcufwdl;
UCHAR  g_cut;
static ULONG  g_syscfg;
static USHORT g_was_on;

/* ---- device table ------------------------------------------------------------------------------------------ */
typedef struct {
   USHORT dev;
   UCHAR  ok;
   char  *name;
} rtw_dev_t;

static const rtw_dev_t g_devs[] = {
   { 0x8821, 1, "Realtek RTL8821AE 802.11ac PCIe (1x1)" },
   { 0x8812, 0, "Realtek RTL8812AE (2x2): recognised, not supported by this driver" },
   { 0xC821, 0, "Realtek RTL8821CE: a different chip (rtw88 family), not supported by this driver" },
   { 0xB821, 0, "Realtek RTL8821CE: a different chip (rtw88 family), not supported by this driver" },
};
#define NDEVS (sizeof(g_devs) / sizeof(g_devs[0]))

static char  *g_devname = "no Realtek RTL8821AE adapter";
static USHORT g_matched;

int hw_pci_match(USHORT ven, USHORT dev, USHORT subven, USHORT subdev)
{
   USHORT i;

   (void)subven; (void)subdev;
   if (ven != 0x10EC)
      return 0;
   for (i = 0; i < NDEVS; i++) {
      if (g_devs[i].dev != dev)
         continue;
      if (!g_matched)
         g_devname = g_devs[i].name;
      if (!g_devs[i].ok)
         return 0;
      g_matched = 1;
      g_devname = g_devs[i].name;
      g_bar_cfg = 0x18;                       /* BAR2: memory registers (rtlwifi bar_id 2); BAR0 is I/O space */
      g_bar_len = 0x00010000UL;               /* 16 KB of registers; one 64 KB window maps them              */
      return 1;
   }
   return 0;
}

char *hw_name(void)    { return g_devname; }
char *hw_fw_name(void) { return "RTL8821AE.FW (Linux rtlwifi/rtl8821aefw_29.bin) or RTL8821AU.FW"; }

/* A Realtek 8821A firmware image: header signature 0x2101 (IS_FW_HEADER_EXIST_8821). Debug commands are text. */
int hw_fw_is_mine(UCHAR FAR *h, USHORT n)
{
   if (n < 32)
      return 0;
   return h[0] == 0x01 && h[1] == 0x21;
}

UCHAR hw_vendor_oui(USHORT i)
{
   static const UCHAR rtk[3] = { 0x00, 0xE0, 0x4C };   /* Realtek's OUI; the adapter's own when known       */

   if (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2])
      return g_hw_mac[i < 3 ? i : 0];
   return rtk[i < 3 ? i : 0];
}

/* ---- power sequences ------------------------------------------------------------------------------------------- */
#define PWR_WRITE    1
#define PWR_POLL     2
#define PWR_DELAY    3                        /* microseconds in msk                                          */
#define PWR_END      4

typedef struct {
   USHORT offset;
   UCHAR  cmd;
   UCHAR  msk;
   UCHAR  value;
} pwr_t;

/* RTL8821A_NIC_ENABLE_FLOW = CARDDIS_TO_CARDEMU + CARDEMU_TO_ACT, PCIe entries, cut A */
static const pwr_t g_pwr_on[] = {
   {0x0005, PWR_WRITE, 0x88, 0x00},           /* clear suspend enable and power down enable                  */
   {0x0005, PWR_WRITE, 0x18, 0x00},           /* 0x04[12:11] = 0: disable WL suspend                         */
   {0x0301, PWR_WRITE, 0xFF, 0x00},           /* PCIe DMA start                                              */
   {0x0005, PWR_WRITE, 0x1C, 0x00},           /* disable SW LPS 0x04[10] and WLSUS_EN 0x04[12:11]            */
   {0x0075, PWR_WRITE, 0x01, 0x01},           /* (PCIe) 0x75[0] = 1                                          */
   {0x0006, PWR_POLL,  0x02, 0x02},           /* wait until 0x04[17] = 1: power ready                        */
   {0x0075, PWR_WRITE, 0x01, 0x00},
   {0x0006, PWR_WRITE, 0x01, 0x01},           /* release WLON reset 0x04[16] = 1                             */
   {0x0005, PWR_WRITE, 0x80, 0x00},           /* disable HWPDN 0x04[15] = 0                                  */
   {0x0005, PWR_WRITE, 0x18, 0x00},           /* disable WL suspend                                          */
   {0x0005, PWR_WRITE, 0x01, 0x01},           /* power-on state machine                                      */
   {0x0005, PWR_POLL,  0x01, 0x00},           /* until it clears                                             */
   {0x004F, PWR_WRITE, 0x01, 0x01},           /* DPDT_SEL_P output from the WL baseband                      */
   {0x0067, PWR_WRITE, 0x30, 0x30},           /* PAPE_G / PAPE_A and LNAON from the WL baseband              */
   {0x0025, PWR_WRITE, 0x40, 0x00},           /* anapar_mac<118>: WLAN single function                       */
   {0x0049, PWR_WRITE, 0x02, 0x02},           /* falling-edge interrupt                                      */
   {0x0063, PWR_WRITE, 0x02, 0x02},           /* GPIO9 interrupt mode                                        */
   {0x0062, PWR_WRITE, 0x02, 0x00},           /* GPIO9 input mode                                            */
   {0x0058, PWR_WRITE, 0x01, 0x01},           /* HSISR GPIO[C:0] interrupt                                   */
   {0x005A, PWR_WRITE, 0x02, 0x02},           /* HSISR GPIO9 interrupt                                       */
   {0x002E, PWR_WRITE, 0xFF, 0x82},           /* 0x2C[23:12] = 0x820: XTAL trim                              */
   {0x0010, PWR_WRITE, 0x40, 0x40},           /* 0x10[6] = 1 (cut A)                                         */
   {0xFFFF, PWR_END,   0x00, 0x00},
};

/* RTL8821A_NIC_LPS_ENTER_FLOW (ACT_TO_LPS), PCIe */
static const pwr_t g_pwr_lps[] = {
   {0x0301, PWR_WRITE, 0xFF, 0xFF},           /* PCIe DMA stop                                               */
   {0x0522, PWR_WRITE, 0xFF, 0xFF},           /* TX pause                                                    */
   {0x05F8, PWR_POLL,  0xFF, 0x00},           /* nothing being transmitted                                   */
   {0x05F9, PWR_POLL,  0xFF, 0x00},
   {0x05FA, PWR_POLL,  0xFF, 0x00},
   {0x05FB, PWR_POLL,  0xFF, 0x00},
   {0x0002, PWR_WRITE, 0x01, 0x00},           /* CCK and OFDM off, clocks gated                              */
   {0x0002, PWR_DELAY, 0x01, 0x00},
   {0x0002, PWR_WRITE, 0x02, 0x00},           /* whole BB reset                                              */
   {0x0100, PWR_WRITE, 0xFF, 0x03},           /* reset MAC TRX                                               */
   {0x0101, PWR_WRITE, 0x02, 0x00},
   {0x0553, PWR_WRITE, 0x20, 0x20},           /* respond TxOK to the scheduler                               */
   {0xFFFF, PWR_END,   0x00, 0x00},
};

/* RTL8821A_NIC_DISABLE_FLOW = ACT_TO_CARDEMU + CARDEMU_TO_CARDDIS, PCIe */
static const pwr_t g_pwr_off[] = {
   {0x001F, PWR_WRITE, 0xFF, 0x00},           /* RF off                                                      */
   {0x004F, PWR_WRITE, 0x01, 0x00},           /* DPDT_SEL_P from register 0x65[2]                            */
   {0x0049, PWR_WRITE, 0x02, 0x00},           /* rising-edge interrupt                                       */
   {0x0005, PWR_WRITE, 0x02, 0x02},           /* 0x04[9] = 1: MAC off by the hardware state machine          */
   {0x0005, PWR_POLL,  0x02, 0x00},
   {0x0005, PWR_WRITE, 0x04, 0x04},           /* 0x04[10] = 1: SW LPS                                        */
   {0xFFFF, PWR_END,   0x00, 0x00},
};

static int pwr_run(const pwr_t *seq)
{
   USHORT i, poll;
   UCHAR  v;

   for (i = 0; seq[i].cmd != PWR_END; i++) {
      switch (seq[i].cmd) {
      case PWR_WRITE:
         v = rtw_r8(seq[i].offset);
         v = (UCHAR)((v & (UCHAR)~seq[i].msk) | (seq[i].value & seq[i].msk));
         rtw_w8(seq[i].offset, v);
         break;
      case PWR_POLL:
         for (poll = 0; poll < 5000; poll++) {          /* 50 ms, as rtl_hal_pwrseqcmdparsing                */
            v = (UCHAR)(rtw_r8(seq[i].offset) & seq[i].msk);
            if (v == (UCHAR)(seq[i].value & seq[i].msk))
               break;
            dd_udelay(10);
         }
         if (poll >= 5000) {
            g_pwr_fail = (USHORT)(i + 1);
            return -1;
         }
         break;
      case PWR_DELAY:
         dd_udelay(seq[i].msk);
         break;
      }
   }
   return 0;
}

/* ---- 8051 reset (rtl8821ae_firmware_selfreset / _8051Reset8812, 8821 branch) ------------------------------------ */
static void fw_selfreset(void)
{
   UCHAR v;

   v = rtw_r8(REG_RSV_CTRL + 1);
   rtw_w8(REG_RSV_CTRL + 1, (UCHAR)(v & ~0x01));
   v = rtw_r8(REG_SYS_FUNC_EN + 1);
   rtw_w8(REG_SYS_FUNC_EN + 1, (UCHAR)(v & ~0x04));
   dd_udelay(50);
   v = rtw_r8(REG_RSV_CTRL + 1);
   rtw_w8(REG_RSV_CTRL + 1, (UCHAR)(v | 0x01));
   v = rtw_r8(REG_SYS_FUNC_EN + 1);
   rtw_w8(REG_SYS_FUNC_EN + 1, (UCHAR)(v | 0x04));
}

/* _rtl8821ae_poweroff_adapter */
static void power_off(void)
{
   UCHAR v;

   pwr_run(g_pwr_lps);
   if (rtw_r8(REG_MCUFWDL) & 0x80)
      fw_selfreset();
   v = rtw_r8(REG_SYS_FUNC_EN + 1);
   rtw_w8(REG_SYS_FUNC_EN + 1, (UCHAR)(v & ~0x04));   /* reset the MCU                                      */
   rtw_w8(REG_MCUFWDL, 0x00);
   pwr_run(g_pwr_off);
   v = rtw_r8(REG_RSV_CTRL + 1);                      /* reset the MCU IO wrapper                           */
   rtw_w8(REG_RSV_CTRL + 1, (UCHAR)(v & ~0x01));
   v = rtw_r8(REG_RSV_CTRL + 1);
   rtw_w8(REG_RSV_CTRL + 1, (UCHAR)(v | 0x01));
   rtw_w8(REG_RSV_CTRL, 0x0E);                        /* lock the ISO / CLK / power control registers       */
}

/* ---- LLT (_rtl8821ae_llt_table_init) ------------------------------------------------------------------------------ */
#define TX_PAGE_BNDY   0xF7
#define LAST_PAGE      255

static int llt_write(USHORT addr, USHORT data)
{
   U32    v;
   USHORT n;

   v.b[0] = (UCHAR)data;                     /* data 7..0, address 15..8, operation (1 = write) 31..30           */
   v.b[1] = (UCHAR)addr;
   v.b[2] = 0;
   v.b[3] = 0x40;
   rtw_w32(REG_LLT_INIT, v.l);
   for (n = 0; n <= 100; n++) {
      v.l = rtw_r32(REG_LLT_INIT);
      if ((v.b[3] & 0xC0) == 0)
         return 0;
      dd_udelay(2);
   }
   g_llt_fail = addr;
   return -1;
}

static int llt_init(void)
{
   USHORT i;

   g_llt_fail = 0xFFFF;
   rtw_w8(REG_TRXFF_BNDY, TX_PAGE_BNDY);
   rtw_w16(REG_TRXFF_BNDY + 2, 0x3E80 - 1);              /* RX DMA boundary: MAX_RX_DMA_BUFFER_SIZE - 1     */
   rtw_w8(REG_TDECTRL + 1, TX_PAGE_BNDY);
   rtw_w8(REG_BCNQ_BDNY, TX_PAGE_BNDY);
   rtw_w8(REG_MGQ_BDNY, TX_PAGE_BNDY);
   rtw_w8(REG_PBP, 0x31);
   rtw_w8(REG_RX_DRVINFO_SZ, 0x04);
   for (i = 0; i < TX_PAGE_BNDY - 1; i++)
      if (llt_write(i, (USHORT)(i + 1)))
         return -1;
   if (llt_write(TX_PAGE_BNDY - 1, 0xFF))                /* end of the TX list                              */
      return -1;
   for (i = TX_PAGE_BNDY; i < LAST_PAGE; i++)            /* the rest as a ring                               */
      if (llt_write(i, (USHORT)(i + 1)))
         return -1;
   if (llt_write(LAST_PAGE, TX_PAGE_BNDY))
      return -1;
   rtw_w32(REG_RQPN, 0x80E60808UL);                      /* pages: high 8, low 8, public 0xE6, load          */
   rtw_w8(REG_RQPN_NPQ, 0x00);
   return 0;
}

/* ---- _rtl8821ae_init_mac -------------------------------------------------------------------------------------------- */
static int init_mac(void)
{
   UCHAR  v;
   USHORT w;

   rtw_w8(REG_RSV_CTRL, 0x00);
   v = rtw_r8(REG_APS_FSMCO + 1);
   rtw_w8(REG_APS_FSMCO + 1, (UCHAR)(v & ~0x80));        /* no auto power down to chip-off                   */
   if (pwr_run(g_pwr_on))
      return -1;
   v = rtw_r8(REG_APS_FSMCO);
   rtw_w8(REG_APS_FSMCO, (UCHAR)(v | 0x10));
   rtw_w8(REG_CR, 0xFF);
   rtw_mdelay(2);
   rtw_w8(REG_HWSEQ_CTRL, 0xFF);
   rtw_mdelay(2);
   v = rtw_r8(REG_SYS_CFG + 3);
   if (v & 0x01)                                         /* LDO mode: 0x7C[6]                                */
      rtw_w8(0x007C, (UCHAR)(rtw_r8(0x007C) | 0x40));
   v = rtw_r8(REG_GPIO_MUXCFG + 1);
   rtw_w8(REG_GPIO_MUXCFG + 1, (UCHAR)(v & ~0x10));
   rtw_w16(REG_CR, 0x02FF);                              /* MAC blocks on, DMA on (MAC TX / RX later)        */
   if (llt_init())
      return -2;
   rtw_w32(REG_HISR, 0xFFFFFFFFUL);
   rtw_w32(REG_HISRE, 0xFFFFFFFFUL);
   w = rtw_r16(REG_TRXDMA_CTRL);
   w = (USHORT)((w & 0x000B) | 0xF5B1);                  /* queue priorities; RXDMA_AGG_EN (bit 2) off       */
   rtw_w16(REG_TRXDMA_CTRL, w);
   rtw_w8(REG_FWHW_TXQ_CTRL + 1, 0x1F);
   rtw_w32(REG_RCR, RCR_SCAN);
   rtw_w16(REG_RXFLTMAP2, 0xFFFF);
   rtw_trx_desa();
   rtw_w8(REG_PCIE_CTRL_REG + 3, 0x77);
   rtw_w32(REG_INT_MIG, 0UL);                            /* no interrupt migration (coalescing)              */
   rtw_w32(REG_MCUTST_1, 0UL);
   rtw_w8(REG_SECONDARY_CCA, 0x03);
   return 0;
}

/* ---- _rtl8821ae_hw_configure, with the station settings of the RTL8821AU driver -------------------------------------- */
static void hw_configure(void)
{
   rtw_w32(REG_RRSR, 0x00000FFFUL);                      /* RATE_ALL_CCK | RATE_ALL_OFDM_AG                  */
   rtw_w32(REG_ARFR0, 0x00000010UL);
   rtw_w32(REG_ARFR0 + 4, 0xFFFFF000UL);
   rtw_w32(REG_ARFR1, 0x00000010UL);
   rtw_w32(REG_ARFR1 + 4, 0x003FF000UL);
   rtw_w32(REG_ARFR2, 0x00000015UL);
   rtw_w32(REG_ARFR2 + 4, 0x003FF000UL);
   rtw_w32(REG_ARFR3, 0x00000015UL);
   rtw_w32(REG_ARFR3 + 4, 0xFFCFF000UL);
   rtw_w16(REG_FWHW_TXQ_CTRL, 0x1F00);
   rtw_w8(REG_AMPDU_MAX_TIME, 0x70);
   rtw_w16(REG_RL, 0x0707);                              /* retry limits (the descriptor sets its own)       */
   rtw_w32(REG_DARFRC, 0x01000000UL);
   rtw_w32(REG_DARFRC + 4, 0x07060504UL);
   rtw_w32(REG_RARFRC, 0x01000000UL);
   rtw_w32(REG_RARFRC + 4, 0x07060504UL);
   rtw_w16(REG_BCN_CTRL, 0x1010);                        /* station: TSF update from beacons, no beaconing   */
   rtw_w8(REG_TBTT_PROHIBIT, 0x04);
   rtw_w8(REG_TBTT_PROHIBIT + 1, 0x64);
   rtw_w16(REG_NAV_PROT_LEN, 0x0040);
   rtw_w32(REG_FAST_EDCA_CTRL, 0x03086666UL);
   rtw_w8(REG_HT_SINGLE_AMPDU, 0x80);
   rtw_w8(REG_RX_PKT_LIMIT, 0x20);
   rtw_w16(REG_MAX_AGGR_NUM, 0x1F1F);
   rtw_w32(REG_MAR, 0xFFFFFFFFUL);                       /* every multicast group                           */
   rtw_w32(REG_MAR + 4, 0xFFFFFFFFUL);
   rtw_w16(REG_RXFLTMAP0, 0xFFFF);                       /* management, control, data                       */
   rtw_w16(REG_RXFLTMAP1, 0x0400);                       /* control: PS-Poll only (_InitWMACSetting_8812A)  */
   rtw_w16(REG_RXFLTMAP2, 0xFFFF);
   rtw_w32(REG_BAR_MODE_CTRL, 0x0201FFFFUL);
   rtw_w8(REG_NAV_UPPER, 0x00);
}

/* ---- firmware download (rtl8821ae_download_fw, _rtl8821ae_write_fw) ------------------------------------------------ */
#define FW_START     0x1000U
#define FW_PAGE      4096U
#define FW_HDR       32U
#define FW_MAX       32768UL

static UCHAR g_fwchunk[256];

static int fw_download(void)
{
   ULONG  off, left;
   USHORT page, n, k, i, tries, poll;
   ULONG  v32;
   UCHAR  v;
   static UCHAR hdr[8];

   if (g_fwblob.len < 64UL || g_fwblob.len > FW_MAX + FW_HDR)
      return -2;
   if (blob_read(&g_fwblob, 0UL, (UCHAR FAR *)hdr, 8))
      return -2;
   g_fw_ver = (USHORT)(hdr[4] | (hdr[5] << 8));
   g_fw_sub = hdr[6];
   off = 0;
   if ((hdr[0] & 0xF0) == 0x00 && hdr[1] == 0x21)        /* header present: skip its 32 bytes                */
      off = FW_HDR;
   if (rtw_r8(REG_MCUFWDL) & 0x80) {                     /* 8051 running RAM code: reset it first           */
      rtw_w8(REG_MCUFWDL, 0x00);
      fw_selfreset();
   }
   v = rtw_r8(REG_MCUFWDL);                              /* _rtl8821ae_enable_fw_download(true)             */
   rtw_w8(REG_MCUFWDL, (UCHAR)(v | 0x01));
   v = rtw_r8(REG_MCUFWDL + 2);
   rtw_w8(REG_MCUFWDL + 2, (UCHAR)(v & 0xF7));
   g_fw_chksum = 0;
   for (tries = 0; tries < 3 && !g_fw_chksum; tries++) {
      ULONG pos = off;

      g_fw_tries = (USHORT)(tries + 1);
      v = rtw_r8(REG_MCUFWDL);
      rtw_w8(REG_MCUFWDL, (UCHAR)(v | 0x04));            /* reset the checksum report                       */
      left = g_fwblob.len - off;
      page = 0;
      while (left) {
         USHORT psz = (USHORT)(left > FW_PAGE ? FW_PAGE : left);

         v = (UCHAR)((rtw_r8(REG_MCUFWDL + 2) & 0xF8) | (page & 0x07));
         rtw_w8(REG_MCUFWDL + 2, v);
         for (k = 0; k < psz; k = (USHORT)(k + n)) {
            n = (USHORT)(psz - k);
            if (n > sizeof(g_fwchunk))
               n = sizeof(g_fwchunk);
            if (blob_read(&g_fwblob, pos + k, (UCHAR FAR *)g_fwchunk, n))
               return -2;
            for (i = 0; i + 4 <= n; i += 4)              /* dword writes, then the odd bytes (block write)  */
               rtw_w32(FW_START + k + i, get32((UCHAR FAR *)g_fwchunk + i));
            for (; i < n; i++)
               rtw_w8(FW_START + k + i, g_fwchunk[i]);
         }
         pos += psz;
         left -= psz;
         page++;
      }
      for (poll = 0; poll < 100; poll++) {
         if (rtw_r32(REG_MCUFWDL) & 0x00000004UL) {      /* FWDL_CHKSUM_RPT                                 */
            g_fw_chksum = 1;
            break;
         }
         rtw_mdelay(1);
      }
   }
   v = rtw_r8(REG_MCUFWDL);
   rtw_w8(REG_MCUFWDL, (UCHAR)(v & 0xFE));               /* download off                                    */
   if (!g_fw_chksum)
      return -3;
   v32 = rtw_r32(REG_MCUFWDL);                           /* _rtl8821ae_fw_free_to_go                        */
   v32 |= 0x00000002UL;                                  /* MCUFWDL_RDY                                     */
   v32 &= ~0x00000040UL;                                 /* WINTINI_RDY                                     */
   rtw_w32(REG_MCUFWDL, v32);
   fw_selfreset();
   g_fw_ready = 0;
   for (poll = 0; poll < 200; poll++) {
      v32 = rtw_r32(REG_MCUFWDL);
      if (v32 & 0x00000040UL) {
         g_fw_ready = 1;
         break;
      }
      rtw_mdelay(1);
   }
   g_mcufwdl = v32;
   return g_fw_ready ? 0 : -4;
}

/* ---- bring-up ---------------------------------------------------------------------------------------------------- */
static void step(USHORT s)
{
   g_rtw_step = s;
   g_hw_state = s;
   tcom('{'); th8((UCHAR)s); tcom('}');
}

static int fail(const char *what, ULONG v)
{
   dd_err(what, v);
   g_hw_failed = 1;
   g_trx_on = 0;
   return -1;
}

int hw_up(void)
{
   USHORT i;
   int    rc;

   g_hw_failed = 0;
   g_hw_ready = 0;
   g_trx_on = 0;
   step(1);
   if (!g_fwblob.done)
      return fail("the firmware file has not been written to the device", 0UL);
   step(2);
   pci_enable();
   step(3);
   if (bar_map())
      return fail("the register BAR could not be mapped", g_bar0);
   step(4);
   if (rtw_trx_alloc())
      return fail("not enough DMA memory for the rings", (ULONG)dma_free_bytes());
   if (hook_init() || irq_hook())
      tstr("[rtw: no interrupt: polled]");
   step(5);
   rtw_irq_disable();
   g_syscfg = rtw_r32(REG_SYS_CFG);
   if (g_syscfg == 0UL || g_syscfg == 0xFFFFFFFFUL)
      return fail("the adapter does not answer (REG_SYS_CFG)", g_syscfg);
   g_cut = (UCHAR)((LO16(g_syscfg) >> 12) & 0x0F);       /* CHIP_VER_RTL_MASK, bits 15:12                    */
   {
      UCHAR cr = rtw_r8(REG_CR);

      g_was_on = (USHORT)(cr != 0 && cr != 0xEA);       /* the MAC is still on (warm boot): power it off    */
      if (g_was_on)
         power_off();
   }
   step(6);
   rtw_trx_init();
   if ((rc = init_mac()) != 0)
      return fail("MAC power-on / LLT failed (step, LLT entry)", ((ULONG)g_pwr_fail << 16) | g_llt_fail);
   step(7);
   rtw_efuse_load();
   if (!g_cfgmac && (g_hw_mac[0] | g_hw_mac[1] | g_hw_mac[2] | g_hw_mac[3] | g_hw_mac[4] | g_hw_mac[5])) {
      fmemcpy((UCHAR FAR *)g_mymac, (UCHAR FAR *)g_hw_mac, 6);
      ndis_set_mac(g_mymac);
   }
   step(8);
   rtw_w8(REG_SYS_CFG, (UCHAR)(rtw_r8(REG_SYS_CFG) & 0x7F));
   g_h2c_box = 0;
   if ((rc = fw_download()) != 0)
      return fail("firmware download failed (2 file, 3 checksum, 4 not started)", (ULONG)(USHORT)(-rc));
   step(9);
   rtw_mac_tables();
   rtw_bb_init();
   step(10);
   rtw_rf_init();
   hw_configure();
   step(11);
   rtw_band_2g();
   rtw_set_channel(1);
   g_curch = 1;
   rtw_bw20();
   rtw_lck();
   step(12);
   rtw_keys_reset(0);
   for (i = 0; i < 6; i++)
      rtw_w8(REG_MACID + i, g_mymac[i]);
   rtw_w8(REG_MSR, (UCHAR)((rtw_r8(REG_MSR) & 0xFC) | 0x02));   /* link type: station (infrastructure)    */
   rtw_w8(REG_BCNTCFG + 1, 0x66);                        /* station (_rtl8821ae_set_media_status)           */
   rtw_w8(REG_TXPAUSE, 0x00);
   {
      UCHAR v = rtw_r8(REG_RXDMA_CONTROL);

      if (v & 0x04)
         rtw_w8(REG_RXDMA_CONTROL, (UCHAR)(v & ~0x04));  /* release RX DMA                                  */
   }
   rtw_w8(REG_PCIE_CTRL_REG + 1, 0x00);                  /* release TX / RX PCIe DMA                        */
   rtw_w8(REG_CR, (UCHAR)(rtw_r8(REG_CR) | 0xC0));       /* MAC TX / RX on                                  */
   rtw_w32(REG_RCR, RCR_SCAN);
   rtw_dig_set(0x20);
   g_chk[0] = rtw_r32(0x0800);
   g_chk[1] = rtw_r32(0x0808);
   g_chk[2] = rtw_r32(0x0C1C);
   g_chk[3] = rtw_r32(0x0C50);
   g_chk[4] = rf_read(0x00);
   g_chk[5] = rf_read(0x18);
   g_chk[6] = rtw_r32(REG_CR);
   g_chk[7] = rtw_r32(REG_RCR);
   rtw_sta_reset();
   g_trx_on = 1;
   rtw_irq_enable();
   step(0x64);
   g_hw_ready = 1;
   tstr("[rtw: ready, firmware "); th16(g_fw_ver); tstr("]"); tnl();
   return 0;
}

void hw_down(void)
{
   if (!g_hw_ready && !g_hw_state)
      return;
   if (g_joined)
      sta_leave();
   g_hw_ready = 0;
   rtw_irq_disable();
   g_trx_on = 0;
   if (g_nbarsel)
      power_off();
   g_hw_state = 0;
}

/* ---- status lines ------------------------------------------------------------------------------------------------- */
void hw_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void))
{
   USHORT i;

   ps("RTW step="); ph(g_rtw_step, 2); ps(" warm="); ph(g_was_on, 1); ps(" SYSCFG="); ph(g_syscfg, 8);
   ps(" cut="); ph(g_cut, 1); ps(" pwrfail="); ph(g_pwr_fail, 2); ps(" lltfail="); ph(g_llt_fail, 4); nl();
   ps("FW ver="); ph(g_fw_ver, 4); ps(" sub="); ph(g_fw_sub, 2); ps(" tries="); ph(g_fw_tries, 1);
   ps(" chksum="); ph(g_fw_chksum, 1); ps(" ready="); ph(g_fw_ready, 1); ps(" MCUFWDL="); ph(g_mcufwdl, 8);
   ps(" H2CBUSY="); ph(g_h2c_busy, 4); nl();
   ps("EFUSE ok="); ph(g_ef_ok, 1); ps(" used="); ph(g_ef_used, 4); ps(" xtal="); ph(g_xtal, 2);
   ps(" rfe="); ph(g_rfe, 2); ps(" pa2g/lna2g/pa5g/lna5g="); ph(g_pa2g, 1); ph(g_lna2g, 1); ph(g_pa5g, 1); ph(g_lna5g, 1);
   ps(" thermal="); ph(g_thermal, 2); nl();
   ps("PHY ch="); ph(g_channel, 2); ps(" band="); ph(g_band, 1); ps(" igi="); ph(g_igi, 2);
   ps(" txp5="); ph(g_txp5, 1); ps(" ofdm="); ph(g_txp_ofdm, 2); ps(" ht="); ph(g_txp_ht, 2); ps(" grp="); ph(g_txp_grp, 2);
   ps(" IQK mode="); ph(g_iqk_mode, 1); ps(" sent="); ph(g_iqk_sent, 4); ps(" ok="); ph(g_iqk_ok, 4);
   ps(" tmo="); ph(g_iqk_tmo, 4); ps(" ms="); ph(g_iqk_ms, 4); nl();
   ps("CHK"); for (i = 0; i < 8; i++) { ps(" "); ph(g_chk[i], 8); } nl();
   ps("REGS CR="); ph(g_nbarsel ? rtw_r32(REG_CR) : 0UL, 8); ps(" RCR="); ph(g_nbarsel ? rtw_r32(REG_RCR) : 0UL, 8);
   ps(" HIMR="); ph(g_nbarsel ? rtw_r32(REG_HIMR) : 0UL, 8); ps(" HISR="); ph(g_nbarsel ? rtw_r32(REG_HISR) : 0UL, 8);
   ps(" PCIE="); ph(g_nbarsel ? rtw_r32(REG_PCIE_CTRL_REG) : 0UL, 8); ps(" TXPAUSE="); ph(g_nbarsel ? rtw_r8(REG_TXPAUSE) : 0, 2); nl();
   ps("TRX on="); ph(g_trx_on, 1); ps(" rxi="); ph(rtw_trx_state(0), 2); ps(" behead="); ph(rtw_trx_state(1), 2);
   ps(" mghead="); ph(rtw_trx_state(2), 2); ps(" masked="); ph(rtw_trx_state(3), 1); ps(" svc="); ph(g_svc_runs, 4);
   ps(" lastisr="); ph(g_hisr_last, 8); nl();
   ps("RX frames="); ph(g_rx_frames, 4); ps(" crc="); ph(g_rx_crc, 4); ps(" c2h="); ph(g_rx_c2h, 4); ps("/"); ph(g_c2h_reg, 4);
   ps(" rdu="); ph(g_rx_rdu, 4); ps(" big="); ph(g_rx_big, 4); ps(" fovw="); ph(g_rx_fovw, 4); ps(" rate="); ph(g_rx_rate, 2); nl();
   ps("TX be="); ph(g_tx_ok[0], 4); ps(" mgmt="); ph(g_tx_ok[1], 4); ps(" full be/mgmt="); ph(g_tx_full[0], 4); ps("/");
   ph(g_tx_full[1], 4); ps(" fovw="); ph(g_tx_fovw, 4); ps(" rate="); ph(g_txrate_auto ? 0xFF : g_txrate, 2);
   ps(" ctl="); ph(g_ctlrate, 2); ps(" keys p/g/dup="); ph(g_keys_p, 2); ph(g_keys_g, 2); ph(g_keys_dup, 2);
   ps(" scans="); ph(g_scan_n, 4); nl();
}

/* ---- debug commands (written to \DEV\WRND32$; results on COM1) ---------------------------------------------------- */
static char *skip(char *p)
{
   while (*p == ' ') p++;
   return p;
}

static ULONG hexv(char **pp)
{
   char  *p = skip(*pp);
   ULONG  v = 0;

   for (;; p++) {
      char   c = *p;
      USHORT d;

      if (c >= '0' && c <= '9') d = (USHORT)(c - '0');
      else if (c >= 'a' && c <= 'f') d = (USHORT)(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') d = (USHORT)(c - 'A' + 10);
      else break;
      v = (v << 4) | d;
   }
   *pp = p;
   return v;
}

static int is(char *c, const char *w)
{
   while (*w)
      if (*c++ != *w++)
         return 0;
   return *c == ' ' || *c == 0;
}

/* 1 = handled. Values are hexadecimal. */
int hw_cmd(char *c)
{
   static char *p;                                            /* its address is passed on: static (SS != DS)  */
   ULONG  a, v;

   if (!g_nbarsel)
      return 0;
   p = c;
   while (*p && *p != ' ') p++;
   if (is(c, "w1") || is(c, "w2") || is(c, "w4")) {          /* w1|w2|w4 <reg> <value>                    */
      a = hexv(&p); v = hexv(&p);
      if (c[1] == '1') rtw_w8((USHORT)a, (UCHAR)v);
      else if (c[1] == '2') rtw_w16((USHORT)a, (USHORT)v);
      else rtw_w32((USHORT)a, v);
   } else if (is(c, "r1")) {                                  /* r1 <reg>: one byte                         */
      a = hexv(&p);
      tstr("[r1 "); th16((USHORT)a); tcom('='); th8(rtw_r8((USHORT)a)); tcom(']'); tnl();
   } else if (is(c, "rf")) {                                  /* rf <reg> [value]                           */
      a = hexv(&p);
      if (*skip(p)) { v = hexv(&p); rf_write((UCHAR)a, v); }
      tstr("[rf "); th8((UCHAR)a); tcom('='); th32(rf_read((UCHAR)a)); tcom(']'); tnl();
   } else if (is(c, "ch")) {                                  /* ch <channel, hex>                          */
      if (g_hw_ready && !g_joined) { v = hexv(&p); rtw_set_channel((UCHAR)v); g_curch = (UCHAR)v; }
   } else if (is(c, "txp")) {
      rtw_txpower_set((UCHAR)hexv(&p));
   } else if (is(c, "iqk")) {                                 /* iqk <mode>: 0 off, 1 5 GHz, 2 always; runs */
      g_iqk_mode = (UCHAR)hexv(&p);
      rtw_iqk((UCHAR)g_channel);
   } else if (is(c, "dig")) {                                 /* dig <igi>: fixed gain; dig 0 = automatic   */
      v = hexv(&p);
      g_dig_auto = (UCHAR)(v == 0);
      if (v) rtw_dig_set((UCHAR)v);
   } else if (is(c, "rate")) {                                /* rate <code>|ff: fixed data rate / automatic */
      v = hexv(&p);
      g_txrate_auto = (UCHAR)(v == 0xFF);
      if (v != 0xFF) g_txrate = (UCHAR)v;
   } else if (is(c, "ctl")) {                                 /* ctl <code>: handshake / ARP / DHCP rate     */
      g_ctlrate = (UCHAR)hexv(&p);
   } else if (is(c, "bsw")) {                                 /* bsw 0|1: external 2.4 / 5 GHz antenna switch */
      g_ext_bsw = (UCHAR)hexv(&p);
      if (g_hw_ready && g_channel) { g_band = 2; rtw_set_channel((UCHAR)g_channel); }
   } else if (is(c, "lck")) {
      if (g_hw_ready) rtw_lck();
   } else if (is(c, "down")) {
      hw_down();
   } else
      return 0;
   return 1;
}
