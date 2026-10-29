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
 * fwdl.c - RTL8188EU firmware download over the OS/2 USB register path (Phase 1
 * milestone 4). Ports hal/rtl8188e_hal_init.c's rtl8188e_FirmwareDownload +
 * _FWDownloadEnable/_PageWrite/_BlockWrite/_FWFreeToGo/_8051Reset88E, built on
 * the rtw_read/write register primitives proven by the chip-version read.
 *
 * The chip has an on-die 8051 that must be loaded with rtl8188eufw.bin (embedded
 * in fwimg.c) before it will do anything RF-related. Flow:
 *   1. if 8051 is running RAM code, quiesce it,
 *   2. enable FW download (MCUFWDL), hold 8051 in reset,
 *   3. per 4KB page: select the page, write the bytes to the 0x1000 window,
 *   4. disable download, poll the checksum-report bit,
 *   5. flag FW ready, reset the 8051, poll WINTINI_RDY = firmware booted.
 *
 * All status bits (ChkSum_rpt, MCUFWDL_RDY, WINTINI_RDY, RAM_DL_SEL) live in the
 * low byte of REG_MCUFWDL, so we use 8-bit read-modify-write throughout and never
 * touch a 32-bit runtime helper (we link /nod, so no __aNulshr et al).
 */
#include "rt_usb.h"

/* Registers (include/rtl8188e_spec.h, include/rtl8188e_hal.h). */
#define REG_SYS_FUNC_EN   0x0002
#define REG_MCUFWDL       0x0080
#define FW_START_ADDR     0x1000        /* 0x1000 window into the selected page */
#define FW_PAGE_SIZE      4096          /* MAX_PAGE_SIZE                         */
#define FW_BLOCK_SIZE     196           /* MAX_REG_BOLCK_SIZE (per control xfer) */
#define FW_HDR_SIZE       32            /* rt_firmware_hdr, skipped before code  */

/* REG_MCUFWDL bits (all in the low byte). */
#define FWDL_ChkSum_rpt   0x04          /* BIT2                                  */
#define MCUFWDL_RDY       0x02          /* BIT1                                  */
#define WINTINI_RDY       0x40          /* BIT6                                  */
#define RAM_DL_SEL        0x80          /* BIT7  1:8051 running RAM code         */

/* REG_SYS_FUNC_EN+1 bit for 8051 reset. */
#define FEN_CPUEN         0x04          /* BIT2                                  */

/* Each poll iteration is a USB round-trip (blocks/yields ~ms), so a modest count
 * spans plenty of real time for the chip to report checksum / FW-ready. */
#define FW_POLL_LIMIT     200

/* The embedded firmware (fwimg.c). */
extern unsigned char  rtl8188eufw[];
extern unsigned short rtl8188eufw_len;

/* ---- MCU firmware-download enable/disable (_FWDownloadEnable) ---------- */
static void fw_dl_enable(usb_dev_t *dev)
{
   UCHAR t;
   t = rtw_read8(dev, REG_MCUFWDL);
   rtw_write8(dev, REG_MCUFWDL, (UCHAR)(t | 0x01));       /* download enable    */
   t = rtw_read8(dev, (USHORT)(REG_MCUFWDL + 2));
   rtw_write8(dev, (USHORT)(REG_MCUFWDL + 2), (UCHAR)(t & 0xF7)); /* 8051 reset */
}

static void fw_dl_disable(usb_dev_t *dev)
{
   UCHAR t;
   t = rtw_read8(dev, REG_MCUFWDL);
   rtw_write8(dev, REG_MCUFWDL, (UCHAR)(t & 0xFE));       /* download disable   */
   rtw_write8(dev, (USHORT)(REG_MCUFWDL + 1), 0x00);      /* reserved, clear    */
}

/* ---- 8051 reset (_8051Reset88E): toggle FEN_CPUEN low then high -------- */
static void reset_8051(usb_dev_t *dev)
{
   UCHAR t;
   t = rtw_read8(dev, (USHORT)(REG_SYS_FUNC_EN + 1));
   rtw_write8(dev, (USHORT)(REG_SYS_FUNC_EN + 1), (UCHAR)(t & (UCHAR)~FEN_CPUEN));
   rtw_write8(dev, (USHORT)(REG_SYS_FUNC_EN + 1), (UCHAR)(t | FEN_CPUEN));
}

/* ---- Phase 1 milestone 4: download the firmware ----------------------- */
/*
 * Returns 0 on success (WINTINI_RDY seen = FW booted). Logs progress to COM1:
 *   D  = download starting
 *   .  = one page written
 *   C+ = checksum report OK   / C! = checksum poll failed
 *   FK = FW ready (booted)    / F! = FW-ready poll failed   / w! = a write failed
 */
int rtl8188eu_download_fw(usb_dev_t *dev)
{
   UCHAR  *fw;
   USHORT  len, off, page, pageSize, i, n;
   UCHAR   t;
   USHORT  poll;

   tcom('D');

   /* Skip the 32-byte rt_firmware_hdr (signature 0x88E1 -> header present). */
   fw  = rtl8188eufw + FW_HDR_SIZE;
   len = (USHORT)(rtl8188eufw_len - FW_HDR_SIZE);

   /* If the 8051 is already running RAM code, quiesce it first. */
   if (rtw_read8(dev, REG_MCUFWDL) & RAM_DL_SEL) {
      rtw_write8(dev, REG_MCUFWDL, 0x00);
      reset_8051(dev);
   }

   fw_dl_enable(dev);

   /* Reset the FWDL checksum-report bit before writing. */
   t = rtw_read8(dev, REG_MCUFWDL);
   rtw_write8(dev, REG_MCUFWDL, (UCHAR)(t | FWDL_ChkSum_rpt));

   /* Write the firmware one 4KB page at a time. The 0x1000 window maps to the
    * page selected by the low 3 bits of REG_MCUFWDL+2. */
   off = 0; page = 0;
   while (off < len) {
      pageSize = (USHORT)(len - off);
      if (pageSize > FW_PAGE_SIZE)
         pageSize = FW_PAGE_SIZE;

      t = rtw_read8(dev, (USHORT)(REG_MCUFWDL + 2));
      rtw_write8(dev, (USHORT)(REG_MCUFWDL + 2),
                 (UCHAR)((t & 0xF8) | (page & 0x07)));

      for (i = 0; i < pageSize; i = (USHORT)(i + FW_BLOCK_SIZE)) {
         n = (USHORT)(pageSize - i);
         if (n > FW_BLOCK_SIZE)
            n = FW_BLOCK_SIZE;
         if (rtw_writeN(dev, (USHORT)(FW_START_ADDR + i), n, fw + off + i) != 0) {
            tcom('w'); tcom('!');
            fw_dl_disable(dev);
            return -1;
         }
      }
      tcom('.');
      off  = (USHORT)(off + pageSize);
      page++;
   }

   fw_dl_disable(dev);

   /* Poll the checksum-report bit (chip verifies as it receives). */
   for (poll = 0; poll < FW_POLL_LIMIT; poll++) {
      t = rtw_read8(dev, REG_MCUFWDL);
      if (t & FWDL_ChkSum_rpt)
         break;
   }
   if (!(t & FWDL_ChkSum_rpt)) {
      tcom('C'); tcom('!');
      return -1;
   }
   tcom('C'); tcom('+');

   /* Flag MCU-FW ready, clear WINTINI_RDY (both low-byte bits -> 8-bit RMW). */
   t = rtw_read8(dev, REG_MCUFWDL);
   t = (UCHAR)((t | MCUFWDL_RDY) & (UCHAR)~WINTINI_RDY);
   rtw_write8(dev, REG_MCUFWDL, t);

   reset_8051(dev);

   /* Poll for the firmware to boot (WINTINI_RDY). */
   for (poll = 0; poll < FW_POLL_LIMIT; poll++) {
      t = rtw_read8(dev, REG_MCUFWDL);
      if (t & WINTINI_RDY)
         break;
   }
   if (!(t & WINTINI_RDY)) {
      tcom('F'); tcom('!');
      return -1;
   }
   tcom('F'); tcom('K');            /* firmware booted - Phase 1 complete */
   return 0;
}
