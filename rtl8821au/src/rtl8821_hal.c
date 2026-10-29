/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL8821AU - Realtek RTL8811AU/RTL8821AU USB WiFi driver for OS/2 and ArcaOS
 * Copyright (C) 2026 the RTL88EU-OS2 project
 *
 * Portions of this file are derived from the Linux rtl8821au driver (morrownr/8821au-20210708),
 * Copyright(c) 2007 - 2017 Realtek Corporation. That code is licensed under version 2 of the
 * GNU General Public License only, so this file is GPL-2.0-only as well.
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
 * rtl8821_hal.c - RTL8821A chip bring-up, phase 2: reset of a running chip, MAC power-on, the TX packet buffer
 * page table (LLT) and the firmware download.
 *
 * Order follows rtl8812au_hal_init() of the Linux driver (hal/rtl8812a/usb/usb_halinit.c):
 *   hw_reset (only if the 8051 is already running) -> power on -> LLT -> drop-incorrect-bulk-out -> firmware.
 * The 30 KB firmware does not fit the driver's 64 KB data segment, so it is read from a file at INIT (ring 3,
 * DosOpen is available there) into memory from AllocPhys and copied to the chip in chunks at task time.
 */
#include "rt_usb.h"

/* registers (hal_com_reg.h of the Linux driver) */
#define REG_SYS_FUNC_EN      0x0002
#define REG_APS_FSMCO        0x0004
#define REG_RSV_CTRL         0x001C
#define REG_RF_CTRL          0x001F
#define REG_MCUFWDL          0x0080
#define REG_CR               0x0100
#define REG_LLT_INIT         0x01E0
#define REG_TXDMA_OFFSET_CHK 0x020C
#define REG_SYS_CFG          0x00F0
#define rFPGA0_XCD_RFPara    0x08B4

#define FW_START_ADDRESS     0x1000U
#define FW_PAGE_SIZE         4096U         /* MAX_DLFW_PAGE_SIZE */
#define FW_HDR_SIZE          32U
#define FW_MAXLEN            32768UL       /* FW_SIZE_8812 */
#define FWDL_CHKSUM_RPT      0x00000004UL  /* MCUFWDL bit 2 */
#define MCUFWDL_RDY          0x00000002UL
#define WINTINI_RDY          0x00000040UL
#define RAM_DL_SEL           0x80

#define TX_PAGE_BOUNDARY_8821   248        /* (0xFF - BCNQ_PAGE_NUM_8821 (8)) + 1 */
#define LAST_ENTRY_OF_TX_PKT_BUFFER 255
#define POLLING_LLT_THRESHOLD   20

/* ---- results, shown by the diagnostics (rtl_dhcp.c) ---------------------------------------------------- */
USHORT g_r21_stage;                        /* last step reached: 1 reset 2 power-on 3 LLT 4 bulk-out 5 firmware 6 done */
USHORT g_r21_pwr_fail;                     /* index+1 of the power-sequence step that failed (0 = none) */
USHORT g_r21_llt_fail;                     /* LLT entry that failed (0xFFFF = none) */
USHORT g_r21_fwload;                       /* INIT-time file load: 0 ok, 1 open failed, 2 AllocPhys, 3 GDT, 4 too big/short */
USHORT g_r21_fwlen;                        /* bytes of firmware image in memory */
USHORT g_r21_fwsig;                        /* signature word of the image header (0x2101 for the 8821A) */
USHORT g_r21_fwtries;                      /* download attempts */
USHORT g_r21_chksum, g_r21_ready;          /* checksum report seen / WINTINI_RDY seen */
ULONG  g_r21_mcufwdl;                      /* REG_MCUFWDL at the end */
ULONG  g_r21_cfg;                          /* REG_SYS_CFG as read */
USHORT g_r21_tried;                         /* the bring-up was started by the first diagnostics read */

static ULONG  g_fw_phys;                   /* AllocPhys block holding the image */
static USHORT g_fw_sel;                    /* GDT selector used to read it */
static USHORT g_fw_sels[2];                /* [0] image buffer, [1] the caller's buffer of a write */

/* ---- firmware image ------------------------------------------------------------------------------------
 * INIT runs at ring 3, where a GDT selector from PhysToGDTSelector cannot be used, so INIT only allocates the buffer
 * and the selectors. The image arrives at task time through the device's write entry (ring 0):
 *    copy /b \IBMCOM\MACS\RTL8821AU.FW \DEV\WRND32$
 * A write is taken as the image when it starts with the header signature bytes 01 21 and is at least 64 bytes long
 * (the debug channel's commands are plain text and never start with 01). */
int rtl8821_fw_load(void)
{
   g_r21_fwlen = 0;
   tcom_always(13); tcom_always(10); tcom_always('F'); tcom_always('2');
   if (rt_dh_alloc_phys(FW_MAXLEN, 0, &g_fw_phys) != 0) {
      g_r21_fwload = 2;
      return -1;
   }
   tcom_always('F'); tcom_always('3');
   if (rt_dh_alloc_gdt(g_fw_sels, 2) != 0) {
      g_r21_fwload = 3;
      return -1;
   }
   tcom_always('F'); tcom_always('4');
   g_fw_sel = g_fw_sels[0];
   return 0;
}

/* task time (ring 0): a write to the device; returns 1 when it was the firmware image */
int rtl8821_fw_write(ULONG phys, USHORT n)
{
   static UCHAR hdr[2];

   if (n < 64U || n > (USHORT)FW_MAXLEN || g_fw_sel == 0)
      return 0;
   rt_dh_phys_to_gdt(phys, n, g_fw_sels[1]);
   rt_movmem((PVOID)hdr, MAKEP(g_fw_sels[1], 0), 2);
   if (hdr[0] != 0x01 || hdr[1] != 0x21)
      return 0;
   rt_dh_phys_to_gdt(g_fw_phys, n, g_fw_sel);
   rt_movmem(MAKEP(g_fw_sel, 0), MAKEP(g_fw_sels[1], 0), n);
   g_r21_fwlen = n;
   g_r21_fwload = 0;
   g_hw_failed = 0;                                  /* the chip may now be started (again) */
   g_r21_tried = 0;
   g_r21_phy_stage = 0;
   tcom_always('F'); tcom_always('W');
   return 1;
}

/* copy n bytes of the image from offset off into dst (task time) */
static void fw_get(ULONG off, UCHAR *dst, USHORT n)
{
   rt_dh_phys_to_gdt(g_fw_phys + off, n, g_fw_sel);
   rt_movmem((PVOID)dst, MAKEP(g_fw_sel, 0), n);
}

/* ---- power-on flow --------------------------------------------------------------------------------------
 * Rtl8821A CARDEMU_TO_ACT of Hal8821APwrSeq.h, pre-resolved for USB and an MP chip (cut mask A): the PCI-only and
 * test-chip-only entries are left out. WRITE = read-modify-write of the masked bits, POLLING = wait for the masked
 * bits, DELAY = milliseconds in msk. */
#define PWR_CMD_WRITE    0x01
#define PWR_CMD_POLLING  0x02
#define PWR_CMD_DELAY    0x03
#define PWR_CMD_END      0x04
#define PWR_POLL_LIMIT   100

typedef struct {
   USHORT offset;
   UCHAR  cmd;
   UCHAR  msk;
   UCHAR  value;
} r21pwr_t;

static r21pwr_t g_pwron[] = {
   {0x0020, PWR_CMD_WRITE,   0x01, 0x01},  /* 0x20[0]=1   enable LDOA12 macro block                       */
   {0x0067, PWR_CMD_WRITE,   0x10, 0x00},  /* 0x67[4]=0   disable BT_GPS_SEL pins                         */
   {0x0001, PWR_CMD_DELAY,   0x01, 0x00},  /* delay 1 ms                                                  */
   {0x0000, PWR_CMD_WRITE,   0x20, 0x00},  /* 0x00[5]=0   release analog IPS to digital                   */
   {0x0005, PWR_CMD_WRITE,   0x1C, 0x00},  /* disable SW LPS and WLSUS_EN                                 */
   {0x0006, PWR_CMD_POLLING, 0x02, 0x02},  /* wait until power ready (0x04[17]=1)                         */
   {0x0006, PWR_CMD_WRITE,   0x01, 0x01},  /* release WLON reset (0x04[16]=1)                             */
   {0x0005, PWR_CMD_WRITE,   0x80, 0x00},  /* disable HWPDN                                               */
   {0x0005, PWR_CMD_WRITE,   0x18, 0x00},  /* disable WL suspend                                          */
   {0x0005, PWR_CMD_WRITE,   0x01, 0x01},  /* trigger the power-on state machine                          */
   {0x0005, PWR_CMD_POLLING, 0x01, 0x00},  /* wait until it clears                                        */
   {0x004F, PWR_CMD_WRITE,   0x01, 0x01},  /* DPDT_SEL_P output from the WL baseband                      */
   {0x0067, PWR_CMD_WRITE,   0x30, 0x30},  /* PAPE / LNAON from the WL baseband                           */
   {0x0025, PWR_CMD_WRITE,   0x40, 0x00},  /* anapar_mac<118>: WLAN single function                       */
   {0x0049, PWR_CMD_WRITE,   0x02, 0x02},  /* falling-edge interrupt                                      */
   {0x0063, PWR_CMD_WRITE,   0x02, 0x02},  /* GPIO9 interrupt mode                                        */
   {0x0062, PWR_CMD_WRITE,   0x02, 0x00},  /* GPIO9 input mode                                            */
   {0x0058, PWR_CMD_WRITE,   0x01, 0x01},  /* HSISR GPIO[C:0] interrupt                                   */
   {0x005A, PWR_CMD_WRITE,   0x02, 0x02},  /* HSISR GPIO9 interrupt                                       */
   {0x002E, PWR_CMD_WRITE,   0xFF, 0x82},  /* XTAL trim                                                   */
   {0x0010, PWR_CMD_WRITE,   0x40, 0x40},  /* 0x10[6]=1 (cut A entry; applies to the MP chip flow)        */
   {0xFFFF, PWR_CMD_END,     0x00, 0x00},
};

static int pwr_seq_run(usb_dev_t *dev, r21pwr_t *seq)
{
   USHORT i, poll;
   UCHAR  v;

   for (i = 0; seq[i].cmd != PWR_CMD_END; i++) {
      switch (seq[i].cmd) {
      case PWR_CMD_WRITE:
         v = rtw_read8(dev, seq[i].offset);
         v = (UCHAR)((v & (UCHAR)~seq[i].msk) | (seq[i].value & seq[i].msk));
         rtw_write8(dev, seq[i].offset, v);
         break;
      case PWR_CMD_POLLING:
         for (poll = 0; poll < PWR_POLL_LIMIT; poll++) {
            v = (UCHAR)(rtw_read8(dev, seq[i].offset) & seq[i].msk);
            if (v == (UCHAR)(seq[i].value & seq[i].msk))
               break;
         }
         if (poll >= PWR_POLL_LIMIT) {
            g_r21_pwr_fail = (USHORT)(i + 1);
            return -1;
         }
         break;
      case PWR_CMD_DELAY:
         usb_mdelay(seq[i].msk);
         break;
      default:
         break;
      }
   }
   return 0;
}

static int r21_power_on(usb_dev_t *dev)
{
   USHORT v16;
   UCHAR  v;

   if (pwr_seq_run(dev, g_pwron) != 0)
      return -1;
   /* enable the MAC DMA / WMAC / scheduler / security blocks (_InitPowerOn_8812AU) */
   rtw_write16(dev, REG_CR, 0x0000);
   v16 = rtw_read16(dev, REG_CR);
   v16 |= 0x063F;                          /* HCI_TX/RXDMA, TX/RXDMA, PROTOCOL, SCHEDULE, ENSEC, CALTMR */
   rtw_write16(dev, REG_CR, v16);
   v = rtw_read8(dev, REG_SYS_CFG + 3);    /* 8821U: LDO mode needs 0x7C[6] */
   if (v & 0x01) {
      v = rtw_read8(dev, 0x007C);
      rtw_write8(dev, 0x007C, (UCHAR)(v | 0x40));
   }
   return 0;
}

/* ---- 8051 reset (_8051Reset8812, 8821 branch) ------------------------------------------------------------ */
static void r21_8051_reset(usb_dev_t *dev)
{
   UCHAR a, b;

   a = rtw_read8(dev, REG_RSV_CTRL);
   rtw_write8(dev, REG_RSV_CTRL, (UCHAR)(a & ~0x02));
   a = rtw_read8(dev, REG_RSV_CTRL + 1);
   rtw_write8(dev, REG_RSV_CTRL + 1, (UCHAR)(a & ~0x01));
   b = rtw_read8(dev, REG_SYS_FUNC_EN + 1);
   rtw_write8(dev, REG_SYS_FUNC_EN + 1, (UCHAR)(b & ~0x04));
   a = rtw_read8(dev, REG_RSV_CTRL);
   rtw_write8(dev, REG_RSV_CTRL, (UCHAR)(a & ~0x02));
   a = rtw_read8(dev, REG_RSV_CTRL + 1);
   rtw_write8(dev, REG_RSV_CTRL + 1, (UCHAR)(a | 0x01));
   rtw_write8(dev, REG_SYS_FUNC_EN + 1, (UCHAR)(b | 0x04));
}

/* A chip whose firmware is still running (warm start) is reset first (rtl8812au_hw_reset). */
static void r21_hw_reset(usb_dev_t *dev)
{
   UCHAR  v;
   USHORT n;

   r21_8051_reset(dev);
   rtw_write8(dev, REG_MCUFWDL, 0x00);
   rtw_write32(dev, rFPGA0_XCD_RFPara, rtw_read32(dev, rFPGA0_XCD_RFPara) | 0x00000040UL);   /* clock gate before BB reset */
   v = rtw_read8(dev, REG_SYS_FUNC_EN);
   rtw_write8(dev, REG_SYS_FUNC_EN, (UCHAR)(v & ~0x03));                                        /* reset BB */
   rtw_write8(dev, REG_RF_CTRL, 0);                                                              /* reset RF */
   rtw_write16(dev, REG_CR, 0);                                                                  /* reset TRX path */
   v = rtw_read8(dev, REG_APS_FSMCO + 1);
   rtw_write8(dev, REG_APS_FSMCO + 1, (UCHAR)(v | 0x02));                                        /* auto FSM off */
   for (n = 0; n < 200; n++) {
      v = rtw_read8(dev, REG_APS_FSMCO + 1);
      if (!(v & 0x02))
         break;
   }
   rtw_write8(dev, REG_APS_FSMCO + 1, (UCHAR)(v | 0x01));                                        /* auto FSM on */
   v = rtw_read8(dev, REG_SYS_FUNC_EN + 1);
   rtw_write8(dev, REG_SYS_FUNC_EN + 1, (UCHAR)(v & ~0x90));
   v = rtw_read8(dev, REG_SYS_FUNC_EN + 1);
   rtw_write8(dev, REG_SYS_FUNC_EN + 1, (UCHAR)(v | 0x90));
}

/* ---- LLT (InitLLTTable8812A) ------------------------------------------------------------------------------ */
static int llt_write(usb_dev_t *dev, USHORT addr, USHORT data)
{
   static ULONG v;
   UCHAR  *p = (UCHAR *)&v;
   USHORT n;

   p[0] = (UCHAR)data;                                             /* data in bits 7..0, address in 15..8, */
   p[1] = (UCHAR)addr;                                             /* operation (1 = write access) in 31..30 */
   p[2] = 0;
   p[3] = 0x40;
   rtw_write32(dev, REG_LLT_INIT, v);
   for (n = 0; n <= POLLING_LLT_THRESHOLD; n++) {
      v = rtw_read32(dev, REG_LLT_INIT);
      if ((p[3] & 0xC0) == 0)                                      /* operation field back to "no active" */
         return 0;
   }
   g_r21_llt_fail = addr;
   return -1;
}

static int r21_init_llt(usb_dev_t *dev)
{
   USHORT i;

   g_r21_llt_fail = 0xFFFF;
   for (i = 0; i < TX_PAGE_BOUNDARY_8821 - 1; i++)
      if (llt_write(dev, i, (USHORT)(i + 1)) != 0)
         return -1;
   if (llt_write(dev, TX_PAGE_BOUNDARY_8821 - 1, 0xFF) != 0)         /* end of list */
      return -1;
   for (i = TX_PAGE_BOUNDARY_8821; i < LAST_ENTRY_OF_TX_PKT_BUFFER; i++)   /* the rest as a ring buffer */
      if (llt_write(dev, i, (USHORT)(i + 1)) != 0)
         return -1;
   return llt_write(dev, LAST_ENTRY_OF_TX_PKT_BUFFER, TX_PAGE_BOUNDARY_8821);
}

/* ---- firmware download (FirmwareDownload8812) ------------------------------------------------------------- */
static UCHAR g_chunk[196];

static int fw_write_block(usb_dev_t *dev, ULONG imgoff, USHORT addr, USHORT n)
{
   fw_get(imgoff, g_chunk, n);
   return rtw_writeN(dev, addr, n, g_chunk) == 0 ? 0 : -1;
}

/* one 4 KB (or shorter) page: select it, then 196-byte blocks, 8-byte blocks, single bytes (_BlockWrite_8812) */
static int fw_write_page(usb_dev_t *dev, USHORT page, ULONG imgoff, USHORT size)
{
   USHORT done = 0, a;
   UCHAR  v;

   v = (UCHAR)((rtw_read8(dev, REG_MCUFWDL + 2) & 0xF8) | (page & 0x07));
   rtw_write8(dev, REG_MCUFWDL + 2, v);
   a = FW_START_ADDRESS;
   while ((USHORT)(size - done) >= 196) {
      if (fw_write_block(dev, imgoff + done, (USHORT)(a + done), 196) != 0)
         return -1;
      done = (USHORT)(done + 196);
   }
   while ((USHORT)(size - done) >= 8) {
      if (fw_write_block(dev, imgoff + done, (USHORT)(a + done), 8) != 0)
         return -1;
      done = (USHORT)(done + 8);
   }
   while (done < size) {
      fw_get(imgoff + done, g_chunk, 1);
      rtw_write8(dev, (USHORT)(a + done), g_chunk[0]);
      done++;
   }
   return 0;
}

static int r21_fw_download(usb_dev_t *dev)
{
   ULONG  imgoff, remain;
   USHORT page, n, tries, poll;
   UCHAR  v;
   ULONG  v32;
   static UCHAR hdr[4];                              /* static: passed as a far pointer (SS != DS) */

   if (g_r21_fwlen < 64U)
      return -2;                                    /* no image in memory */
   fw_get(0UL, hdr, 4);
   g_r21_fwsig = (USHORT)(hdr[0] | ((USHORT)hdr[1] << 8));
   imgoff = 0;
   remain = g_r21_fwlen;
   if ((g_r21_fwsig & 0xFFF0) == 0x2100) {           /* header present: skip its 32 bytes */
      imgoff = FW_HDR_SIZE;
      remain -= FW_HDR_SIZE;
   }
   if (rtw_read8(dev, REG_MCUFWDL) & RAM_DL_SEL) {   /* 8051 running RAM code: reset it first */
      rtw_write8(dev, REG_MCUFWDL, 0x00);
      r21_8051_reset(dev);
   }
   /* enable download: MCUFWDL |= 1, 8051 reset bit 0x82[3] = 0 */
   v = rtw_read8(dev, REG_MCUFWDL);
   rtw_write8(dev, REG_MCUFWDL, (UCHAR)(v | 0x01));
   v = rtw_read8(dev, REG_MCUFWDL + 2);
   rtw_write8(dev, REG_MCUFWDL + 2, (UCHAR)(v & 0xF7));

   g_r21_chksum = 0;
   for (tries = 0; tries < 3 && !g_r21_chksum; tries++) {
      g_r21_fwtries = (USHORT)(tries + 1);
      v = rtw_read8(dev, REG_MCUFWDL);
      rtw_write8(dev, REG_MCUFWDL, (UCHAR)(v | 0x04));    /* reset the checksum report */
      page = 0;
      n = 0;
      {
         ULONG left = remain, off = imgoff;
         while (left) {
            n = (USHORT)(left > FW_PAGE_SIZE ? FW_PAGE_SIZE : left);
            if (fw_write_page(dev, page, off, n) != 0)
               break;
            off += n;
            left -= n;
            page++;
         }
         if (left)
            continue;                                      /* a write failed: try again */
      }
      for (poll = 0; poll < 50; poll++) {
         v32 = rtw_read32(dev, REG_MCUFWDL);
         if (v32 & FWDL_CHKSUM_RPT) {
            g_r21_chksum = 1;
            break;
         }
         usb_mdelay(1);
      }
   }
   v = rtw_read8(dev, REG_MCUFWDL);
   rtw_write8(dev, REG_MCUFWDL, (UCHAR)(v & 0xFE));        /* disable download */
   if (!g_r21_chksum)
      return -3;

   /* _FWFreeToGo8812: mark the image ready, reset the 8051, wait for WINTINI_RDY */
   v32 = rtw_read32(dev, REG_MCUFWDL);
   v32 |= MCUFWDL_RDY;
   v32 &= ~WINTINI_RDY;
   rtw_write32(dev, REG_MCUFWDL, v32);
   r21_8051_reset(dev);
   g_r21_ready = 0;
   for (poll = 0; poll < 100; poll++) {
      v32 = rtw_read32(dev, REG_MCUFWDL);
      if (v32 & WINTINI_RDY) {
         g_r21_ready = 1;
         break;
      }
      usb_mdelay(2);
   }
   g_r21_mcufwdl = v32;
   return g_r21_ready ? 0 : -4;
}

static int rtl8821_hw_init_run(usb_dev_t *dev);

static UCHAR  g_early_busy;
static int    g_early_rc;

/* Bind time: power the MAC so the eFuse can be read (the firmware is not needed for that). 0 = ok. */
int rtl8821_early_power(usb_dev_t *dev)
{
   if (rt_tas(&g_early_busy))
      return -1;
   g_early_rc = r21_power_on(dev);
   g_early_busy = 0;
   return g_early_rc;
}

/* ---- the phase-2 bring-up -------------------------------------------------------------------------------- */
int rtl8821_hw_init(usb_dev_t *dev)
{
   int rc;
   static UCHAR busy;

   if (g_r21_phy_stage == 8)
      return 0;                                    /* already up (until a new firmware image is written) */

   if (rt_tas(&busy))
      return 0;                                    /* another caller is already bringing the chip up */
   rc = rtl8821_hw_init_run(dev);
   busy = 0;
   return rc;
}

static int rtl8821_hw_init_run(usb_dev_t *dev)
{
   int rc;

   g_r21_stage = 0;
   if (g_r21_fwlen < 64U)
      return -2;                                   /* no firmware image yet: do not touch the chip */
   g_r21_cfg = rtw_read32(dev, REG_SYS_CFG);
   if (g_r21_cfg == 0UL || g_r21_cfg == 0xFFFFFFFFUL)
      return -1;                                   /* USB control path not working */
   g_r21_stage = 1; tcom_always('H'); tcom_always('1');
   if (rtw_read8(dev, REG_MCUFWDL) & RAM_DL_SEL)
      r21_hw_reset(dev);
   g_r21_stage = 2; tcom_always('H'); tcom_always('2');
   if (r21_power_on(dev) != 0)
      return -1;
   g_r21_stage = 3; tcom_always('H'); tcom_always('3');
   if (r21_init_llt(dev) != 0)
      return -1;
   g_r21_stage = 4; tcom_always('H'); tcom_always('4');
   rtw_write32(dev, REG_TXDMA_OFFSET_CHK, rtw_read32(dev, REG_TXDMA_OFFSET_CHK) | 0x00000200UL);   /* drop incorrect bulk out */
   g_r21_stage = 5; tcom_always('H'); tcom_always('5');
   rc = r21_fw_download(dev);
   g_r21_mcufwdl = rtw_read32(dev, REG_MCUFWDL);
   if (rc != 0)
      return rc;
   g_r21_stage = 6; tcom_always('H'); tcom_always('6');
   rtl8821_phy_init(dev);                           /* MAC / BB / RF tables, band, channel, receive filter */
   tcom_always('H'); tcom_always('7');
   dev->attached = 1;
   g_hw_ready = 1;
   if (!g_r21_norx)
      rtl8188eu_rx_engine_start(dev);               /* always-on asynchronous bulk-IN receive */
   tcom_always('H'); tcom_always('8');
   return 0;
}
