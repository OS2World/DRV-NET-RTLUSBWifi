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
 * rtl_init.c - RTL8188EU hardware init (Phase 2, radio bring-up).
 *
 * Milestone 2.1: the power-on sequence (ports rtl8188eu_InitPowerOn from the
 * Linux driver, hal/usb_halinit.c + hal/HalPwrSeqCmd.c + hal/Hal8188EPwrSeq.c).
 *
 * The Linux driver runs a data-driven power table with per-cut/fab/interface
 * masks via HalPwrSeqCmdParsing. We PRE-RESOLVE for our device (USB, all cuts/
 * fabs), so the table holds only the applicable steps and the interpreter drops
 * the mask matching - much smaller for 16-bit. All ops are 8-bit register
 * read/modify/write and polling on the proven rtw_read8/write8 USB HAL, so no
 * 32-bit runtime helpers (/nod safe).
 *
 * Order (per rtl8188eu_hal_init): power-on runs BEFORE firmware download. This
 * is the first step of full HW init; queue setup, BB/RF config, LLT and the RX
 * path follow in later milestones.
 */
#include "rt_usb.h"

/* power-sequence command types (HalPwrSeqCmd.h) */
#define PWR_CMD_WRITE    0x01
#define PWR_CMD_POLLING  0x02
#define PWR_CMD_DELAY    0x03
#define PWR_CMD_END      0x04

/* REG_CR (0x0100) + enable bits: HCI_TX/RXDMA | TX/RXDMA | PROTOCOL | SCHEDULE
 * | ENSEC | CALTMR = BIT 0,1,2,3,4,5,9,10 = 0x063F (rtl8188e_spec.h). */
#define REG_CR           0x0100
#define CR_ENABLE_BITS   0x063F

/* queue/page/boundary registers (milestone 2.2, rtl8188e_spec.h) */
#define REG_PBP                     0x0104
#define REG_TRXDMA_CTRL             0x010C
#define REG_TRXFF_BNDY              0x0114
#define REG_RQPN                    0x0200
#define REG_TDECTRL                 0x0208
#define REG_RQPN_NPQ                0x0214
#define REG_TXPKTBUF_BCNQ_BDNY      0x0424
#define REG_TXPKTBUF_MGQ_BDNY       0x0425
#define REG_TXPKTBUF_WMAC_LBK_BF_HD 0x045D

#define PWR_POLL_LIMIT   100         /* each poll is a USB round-trip (~ms)     */

/* one pre-resolved power-sequence step */
typedef struct {
   USHORT offset;
   UCHAR  cmd;
   UCHAR  msk;
   UCHAR  value;
} pwrcmd_t;

/* CARDEMU_TO_ACT for USB (Hal8188EPwrSeq.h; the SDIO-only 0x0074 entry omitted).
 * Static -> lives in DS (near); only read here, never handed to USBD. */
static pwrcmd_t g_pwron_flow[] = {
   {0x0006, PWR_CMD_POLLING, 0x02, 0x02},  /* wait 0x04[17]=1  power ready      */
   {0x0002, PWR_CMD_WRITE,   0x03, 0x00},  /* 0x02[1:0]=0      reset BB          */
   {0x0026, PWR_CMD_WRITE,   0x80, 0x80},  /* 0x24[23]         schmit trigger    */
   {0x0005, PWR_CMD_WRITE,   0x80, 0x00},  /* 0x04[15]=0       disable HWPDN      */
   {0x0005, PWR_CMD_WRITE,   0x18, 0x00},  /* 0x04[12:11]=0    disable WL suspend */
   {0x0005, PWR_CMD_WRITE,   0x01, 0x01},  /* 0x04[8]=1                           */
   {0x0005, PWR_CMD_POLLING, 0x01, 0x00},  /* wait 0x04[8]=0                      */
   {0x0023, PWR_CMD_WRITE,   0x10, 0x00},  /* 0x23[4]=0        LDO normal mode    */
   {0xFFFF, PWR_CMD_END,     0x00, 0x00},
};

/* Interpret a pre-resolved power sequence (WRITE = RMW, POLLING = wait match). */
static int pwr_seq_run(usb_dev_t *dev, pwrcmd_t *seq)
{
   USHORT i;
   USHORT poll;
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
         if (poll >= PWR_POLL_LIMIT)
            return -1;                /* polling timed out */
         break;
      default:
         break;                       /* no DELAY step in the power-on flow */
      }
   }
   return 0;
}

/*
 * Milestone 2.1: power the MAC on, then enable the DMA/WMAC/SCHEDULE/SEC blocks.
 * COM1 trace: 'W' start, 'P' ok, '!' power-seq polling timeout.
 */
int rtl8188eu_init_power_on(usb_dev_t *dev)
{
   USHORT cr;

   tcom('W');
   if (pwr_seq_run(dev, g_pwron_flow) != 0) {
      tcom('!');
      return -1;
   }

   /* Enable MAC DMA/WMAC/SCHEDULE/SEC block (REG_CR), matching InitPowerOn. */
   rtw_write16(dev, REG_CR, 0x0000);
   cr = rtw_read16(dev, REG_CR);
   cr = (USHORT)(cr | CR_ENABLE_BITS);
   rtw_write16(dev, REG_CR, cr);

   tcom('P');
   return 0;
}

/*
 * Milestone 2.2: TX-queue reserved pages + priority + page/FIFO boundaries.
 * Ports _InitQueueReservedPage / _InitQueuePriority / _InitPageBoundary /
 * _InitTransferPageSize / _InitTxBufferBoundary(0) - the steps between power-on
 * and firmware in rtl8188eu_hal_init. Values PRE-RESOLVED for the typical case
 * (wifi_spec off) and the RTL8188EU's 3 bulk-OUT endpoints, so this is just
 * fixed register writes on the proven rtw_* HAL. COM1: 'Q' start, 'q' done.
 *
 * NOTE: assumes OutEpNumber = 3 (common RTL8188EU layout); the queue-priority
 * mapping is TX-side, so it does not affect the RX scan even if the endpoint
 * count differs.
 */
int rtl8188eu_init_queue(usb_dev_t *dev)
{
   USHORT v16;

   tcom('Q');

   /* reserved page (non-wifi_spec): NPQ then RQPN (HPQ|LPQ|PUBQ|LD_RQPN) */
   rtw_write16(dev, REG_RQPN_NPQ, 0x0000);
   rtw_write16(dev, REG_RQPN_NPQ, 0x000D);
   rtw_write32(dev, REG_RQPN, 0x808E000DUL);

   /* queue priority (3 out-EPs, non-wifi_spec): BE/BK=LOW VI=NORMAL VO/MGT/HI=HIGH
    * -> _TXDMA_*_MAP bits = 0xF5B0; preserve low 3 bits of TRXDMA_CTRL. */
   v16 = (USHORT)((rtw_read16(dev, REG_TRXDMA_CTRL) & 0x0007) | 0xF5B0);
   rtw_write16(dev, REG_TRXDMA_CTRL, v16);

   /* RX page boundary = MAX_RX_DMA_BUFFER_SIZE_88E(0x2400) - 1 */
   rtw_write16(dev, (USHORT)(REG_TRXFF_BNDY + 2), 0x23FF);

   /* transfer page size RX/TX = 128 (_PSRX(1)|_PSTX(1) = 0x11) */
   rtw_write8(dev, REG_PBP, 0x11);

   /* TX buffer boundary = 0 (first call; re-set to txpktbuf_bndy before LLT) */
   rtw_write8(dev, REG_TXPKTBUF_BCNQ_BDNY, 0x00);
   rtw_write8(dev, REG_TXPKTBUF_MGQ_BDNY, 0x00);
   rtw_write8(dev, REG_TXPKTBUF_WMAC_LBK_BF_HD, 0x00);
   rtw_write8(dev, REG_TRXFF_BNDY, 0x00);
   rtw_write8(dev, (USHORT)(REG_TDECTRL + 1), 0x00);

   tcom('q');
   return 0;
}

/* ===== Card disable (chip back to a cold state) ===============================
 * Port of the Linux CardDisableRTL8188EU flow (stop RX, MCU off, RF off, LDO sleep, MAC power-down
 * request, HWPDN), reduced to what a USB dongle needs. Used before a retry when the chip was found
 * "warm" (already running from a previous session) and the normal power-on/firmware sequence failed. */
static void poll_clear(usb_dev_t *dev, USHORT reg, UCHAR bit)
{
   USHORT t;

   for (t = 0; t < 100; t++) {                  /* up to ~100 ms */
      if (!(rtw_read8(dev, reg) & bit))
         return;
      usb_mdelay(1);
   }
}

int rtl8188eu_card_disable(usb_dev_t *dev)
{
   UCHAR v;

   v = rtw_read8(dev, 0x04EC);                  /* stop the TX report timer */
   rtw_write8(dev, 0x04EC, (UCHAR)(v & ~0x02));
   rtw_write8(dev, 0x0100, 0x00);               /* REG_CR = 0: MAC TX/RX off */
   rtw_write8(dev, 0x0522, 0xFF);               /* pause all TX queues */
   v = rtw_read8(dev, 0x0003);                  /* SYS_FUNC_EN+1: FEN_CPUEN off = stop the 8051 */
   rtw_write8(dev, 0x0003, (UCHAR)(v & ~0x04));
   rtw_write8(dev, 0x0080, 0x00);               /* REG_MCUFWDL: firmware-ready state cleared */
   v = rtw_read8(dev, 0x0194);                  /* REG_32K_CTRL */
   rtw_write8(dev, 0x0194, (UCHAR)(v & ~0x01));
   /* ACT -> CARDEMU */
   rtw_write8(dev, 0x001F, 0x00);               /* RF off */
   v = rtw_read8(dev, 0x0023);
   rtw_write8(dev, 0x0023, (UCHAR)(v | 0x10));  /* LDO sleep mode */
   v = rtw_read8(dev, 0x0005);
   rtw_write8(dev, 0x0005, (UCHAR)(v | 0x02));  /* MAC off by HW state machine */
   poll_clear(dev, 0x0005, 0x02);
   /* CARDEMU -> PDN */
   v = rtw_read8(dev, 0x0006);
   rtw_write8(dev, 0x0006, (UCHAR)(v & ~0x01));
   v = rtw_read8(dev, 0x0005);
   rtw_write8(dev, 0x0005, (UCHAR)(v | 0x80));  /* HWPDN */
   /* reset the MCU IO wrapper */
   v = rtw_read8(dev, 0x001D);
   rtw_write8(dev, 0x001D, (UCHAR)(v & ~0x08));
   v = rtw_read8(dev, 0x001D);
   rtw_write8(dev, 0x001D, (UCHAR)(v | 0x08));
   usb_mdelay(50);
   tcom('Y');                                   /* trace: card disabled */
   return 0;
}
