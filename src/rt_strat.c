/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL88EU - Realtek RTL8188EU USB WiFi driver for OS/2 and ArcaOS
 * Copyright (C) 2026 the RTL88EU-OS2 project
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
 * rt_strat.c - strategy routine and initialization of the RTL88EU driver.
 *
 * Written from the IBM Physical Device Driver Reference (strategy commands, request packets, INIT, INIT COMPLETE,
 * AttachDD, Save_Message); facts used are listed in v012/PROVENANCE.md, section 5.
 *
 *   rt_strategy()  - called by rt_hdr.asm with the request packet; dispatches on the command code.
 *   rt_init()      - INIT: save the DevHelp entry, find USBD's IDC entry (AttachDD "USBD$"), register the NDIS MAC
 *                    with the Protocol Manager, report the resident end.
 *   rt_initdone()  - INIT COMPLETE: the USB stack is up now, register as a USB client.
 */
#include "rt_usb.h"
#include "rt_ndis.h"

/* ---- driver globals ---------------------------------------------------------- */
static USHORT g_rd_pending;            /* an open handle has not read yet: that read runs DHCP */

/* ---- boot messages (EDM2 "Suggestion for OS/2 and eComStation Drivers Arguments") ----
 * Default: silent, only an error is shown when the driver cannot load.
 *   /V  verbose: name, version, build date, device and NDIS status
 *   /Q  quiet:   no message at all
 *   /W  wait:    pause 30 s after all drivers are loaded so the messages can be read
 *   /AA beeps:   accepted (the driver never beeps)
 * Text goes through Save_Message (message 1178 = "%1"); the strings must be static. */
#define DRV_VERSION   "0.1.2"
#define MSG_REPLACEMENT_STRING  1178
static struct {
   USHORT id;
   USHORT count;
   PSZ    text;
} gInitMsg = { MSG_REPLACEMENT_STRING, 1, 0 };
static USHORT g_opt_v, g_opt_q, g_opt_w, g_opt_aa, g_opt_bad;

static void say(PSZ s)
{
   PSZ p;

   if (g_opt_q)
      return;
   gInitMsg.text = s;
   rt_save_message((void *)&gInitMsg);
   for (p = s; *p; p++)                          /* the same text on COM1, so it can be read from the host */
      tcom_always(*p);
   tcom_always(13); tcom_always(10);
}

/* ---- /W: timed pause at INIT COMPLETE (all drivers have printed by then) ---------------------
 * The OS/2 keyboard driver owns the keyboard hardware, so a driver cannot see a key press this early.
 * /W therefore pauses for a fixed time. */
#define WAIT_SECONDS  30
#define EVT_PAUSE     0x52544C03UL

static void wait_pause(void)
{
   USHORT it;

   for (it = 0; it < WAIT_SECONDS * 20; it++)    /* 20 x 50 ms per second */
      rt_dh_procblock(EVT_PAUSE, 50UL, WAIT_UNINTERRUPTIBLE);
}

/* Parse the CONFIG.SYS line (everything after DEVICE=): switches start with '/', any case. */
static void parse_args(PSZ p)
{
   char c, d;

   if (!p)
      return;
   while (*p) {
      if (*p == '/') {
         c = (char)(p[1] & 0xDF);                 /* to upper case */
         d = p[1] ? (char)(p[2] & 0xDF) : 0;
         if (c == 'V')                      g_opt_v = 1;
         else if (c == 'Q')                 g_opt_q = 1;
         else if (c == 'W')                 g_opt_w = 1;
         else if (c == 'A' && d == 'A')     g_opt_aa = 1;
         else                               g_opt_bad = 1;
      }
      p++;
   }
}

/* ---- INIT ---------------------------------------------------------------------------------- */
static void rt_init(RP_INIT FAR *rp)
{
   static USHORT idc[6];               /* AttachDD result: 3 reserved words, IDC offset, IDC selector, IDC DS */
   static char   usbd[] = "USBD$   ";
   union { ULONG l; USHORT w[2]; } ep;
   USHORT rc;

   rt_devhelp = rp->p1.DevHlp;         /* must precede any DevHelp call */
   parse_args(rp->Args);

   /* Keep the whole driver resident: USBD holds a pointer to our IDC entry after registration, so nothing may be
    * discarded. rt_code_end / rt_data_end are the last code / data of the last-linked module. */
   rp->Data1 = 0;
   rp->p1.End.CodeEnd = rt_code_end();
   rp->p1.End.DataEnd = rt_data_end();

   /* Attach to the USB stack to obtain its IDC entry point. */
   rc = rt_dh_attach_dd(usbd, idc);
   if (rc) {
      say("RTL88EU: could not be loaded. USB stack (USBD$) not found - load the USB drivers before it.");
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   ep.w[0] = idc[3];
   ep.w[1] = idc[4];
   gpUSBDIDC  = ep.l;
   gdsUSBDIDC = idc[5];

   /* NDIS MAC: register with the Protocol Manager (optional - only succeeds when PROTOCOL.INI has an
    * [RTL88EU_NIF] section; the GenMAC device works either way). */
   {
      USHORT nrc = ndis_init();

      ndis_find_lan();

      if (g_opt_bad)
         say("RTL88EU: warning - unknown option ignored (valid: /V /Q /W /AA).");
      if (g_opt_v) {
         say("RTL88EU: Realtek RTL8188EU (0BDA:8179) USB WLAN driver for OS/2.");
         say("RTL88EU: Version " DRV_VERSION "  Date " __DATE__ ".");
         say("RTL88EU: Supported device: Realtek 802.11n NIC 0BDA:8179, found when it is plugged in.");
         if (nrc == 0)
            say("RTL88EU: NDIS MAC registered with the Protocol Manager (adapter lan, GenMAC device WRND32$).");
         else
            say("RTL88EU: NDIS MAC not registered (no [RTL88EU_NIF] in PROTOCOL.INI?); GenMAC device WRND32$ only.");
         {
            static char lanmsg[] = "RTL88EU: TCP/IP interface: lan0.";

            lanmsg[sizeof(lanmsg) - 3] = (char)(g_lannum < 10 ? '0' + g_lannum : '?');
            say(lanmsg);
         }
         say("RTL88EU: Driver loaded without warnings.");
      }
   }
   if (g_opt_w)
      say("RTL88EU: Realtek RTL8188EU USB WLAN driver " DRV_VERSION ". The boot pauses 30 seconds after all drivers are loaded so the messages can be read.");
   dhcp_init();                         /* GDT selector for returning the DHCP lease on a device read */
   dbg_init();

   /* Registration with USBD is deferred to INIT COMPLETE: during INIT the USB stack is not yet ready to accept a
    * client registration. */
   rp->rph.Status |= STDON;
}

/* ---- INIT COMPLETE: runs after every driver's INIT; the USB stack is up now, so register here. ----- */
static void rt_initdone(RPH FAR *rp)
{
   if (g_opt_w && !g_opt_q)
      wait_pause();                    /* /W: fixed pause so the boot messages can be read */
   usb_register_client();              /* USBD calls CHKSERV for each device; ours is matched there */
   /* The chip probe does NOT run here: during boot the USB completion interrupt is not serviced yet, so a transfer
    * would just time out. The chip starts on the first GenMAC request at task time. */
   rp->Status |= STDON;
}

/* ---- the strategy routine ------------------------------------------------------------------- */
void __cdecl rt_strategy(RPH FAR *rp)
{
   rp->Status = 0;

   switch (rp->Cmd) {
   case CMD_INIT:
      rt_init((RP_INIT FAR *)rp);
      break;
   case CMD_INITCOMPLETE:
      rt_initdone(rp);
      break;

   case CMD_OPEN:
      g_rd_pending = 1;                /* the next read on this open returns the DHCP lease */
      /* (no chip init here: it starts on the first GenMAC request, so the device can be opened for the debug
       *  channel / DHCP read with the chip still in its cold state) */
      rp->Status |= STDON;
      break;

   case CMD_WRITE: {
      /* a command line for the debug channel (rtl_dbg.c) */
      RP_RW FAR *rw = (RP_RW FAR *)rp;

      rtl8188eu_dbg_write(rw->Xfer, rw->Count);
      rp->Status |= STDON;
      break;
   }

   case CMD_READ: {
      /* A read right after Open runs the driver's DHCP client and returns the lease as text (KEY=VALUE lines);
       * any further read returns 0 bytes (EOF) so COPY/TYPE finish. */
      RP_RW FAR *rw = (RP_RW FAR *)rp;
      USHORT     n = 0;

      if (g_rd_pending) {
         g_rd_pending = 0;
         n = rtl8188eu_dhcp_read(rw->Xfer, rw->Count);
      }
      rw->Count = n;
      rp->Status |= STDON;
      break;
   }

   case CMD_GENIOCTL:
      /* GenMAC wrapper interface: category 0x99, OID get/set (rtl_nic.c) */
      rtl8188eu_genmac_ioctl((RP_GENIOCTL FAR *)rp);
      rp->Status |= STDON;
      break;

   case CMD_SHUTDOWN:
      rtl8188eu_shutdown();
      rp->Status |= STDON;
      break;

   case CMD_CLOSE:
   default:
      rp->Status |= STDON;
      break;
   }
}
