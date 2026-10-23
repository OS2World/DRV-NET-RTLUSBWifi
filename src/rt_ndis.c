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
 * rt_ndis.c - NDIS 2.0.1 MAC front-end of the RTL8188EU driver.
 *
 * Written from the NDIS 2.0.1 specification. At INIT the driver registers its common characteristics table with
 * the Protocol Manager (device PROTMAN$); the stack (TCP/IP via MPTS/NETBIND) then calls the system request
 * (Bind) and uses the upper dispatch table (Request, TransmitChain, ...).
 *
 * The protocol calls every entry point with the MAC's DS as the last parameter (the entry points cannot assume
 * DS); the small assembler stubs in rt_ndisx.asm load it and call the ndis_c_* functions below.
 */
#include "rt_usb.h"
#include "rt_ndis.h"

/* 16-bit DOS API (DOSCALLS ordinals 70, 59, 137, 53; imported at link time). Pascal convention. */
extern USHORT __far __pascal DosOpen(PSZ name, PUSHORT phf, PUSHORT pAction, ULONG size, USHORT attr,
                                     USHORT openFlag, USHORT openMode, ULONG reserved);
extern USHORT __far __pascal DosClose(USHORT hf);
extern USHORT __far __pascal DosRead(USHORT hf, PVOID buf, USHORT cb, PUSHORT pActual);
extern USHORT __far __pascal DosDevIOCtl(PVOID data, PVOID parm, USHORT func, USHORT cat, USHORT hf);

#define OPEN_ACTION_FAIL_IF_NEW      0x0000
#define OPEN_ACTION_OPEN_IF_EXISTS   0x0001
#define FILE_EXISTED                 0x0001
#define OPEN_ACCESS_READONLY         0x0000
#define OPEN_SHARE_DENYREADWRITE     0x0010
#define OPEN_SHARE_DENYNONE          0x0040
#define OPEN_FLAGS_NOINHERIT         0x0080

/* ---- tables ----------------------------------------------------------------- */
static char g_adapter_descr[] = "Realtek RTL8188EU USB WLAN (OS/2 native)";

extern struct CommonChars   gCCT;
extern struct MACSpecChars  gSCT;
extern struct MACSpecStatus gSST;
extern struct MACUpperDisp  gUDT;
extern struct MCAddrList    gMCAL;

struct CommonChars gCCT = {
   sizeof(struct CommonChars),
   MAJOR_NDIS_VERSION, MINOR_NDIS_VERSION,
   0,
   MAJOR_DRV_VERSION, MINOR_DRV_VERSION,
   CCTFLAG_BINDING_UPPER,
   NDIS_MODULE_NAME,
   UPPER_PROTOCOL_MAC, UPPER_INTERFACE_MAC,
   LOWER_PROTOCOL_USB, LOWER_INTERFACE_USB,
   0,                      /* module ID  (assigned by the Protocol Manager)  */
   0,                      /* module DS  (set in ndis_init)                  */
   ndis_sysreq_entry,
   &gSCT, &gSST, &gUDT,
   0,                      /* no lower dispatch table                         */
   0, 0
};

struct MACSpecChars gSCT = {
   sizeof(struct MACSpecChars),
   MAC_TYPE_NAME,
   STATION_ADDR_LEN,
   { 0, 0, 0, 0, 0, 0 },                   /* permanent station address (filled in ndis_init) */
   { 0, 0, 0, 0, 0, 0 },                   /* current                                        */
   0UL,                                    /* functional address: none                       */
   &gMCAL,
   MAX_LINK_SPEED,
   SCTFLAG_BROADCAST | SCTFLAG_MULTICAST | SCTFLAG_SET_STN_ADDR |
   SCTFLAG_STAT_ALWAYS_CURR | SCTFLAG_RECEIVE_CHAIN,
   MAX_FRAME_SIZE,
   8UL * 1600UL, 1600,                     /* transmit buffer capacity / block size          */
   8UL * 1600UL, 1600,                     /* receive buffer capacity / block size           */
   { 0xFF, 0xFF, 0xFF },                   /* no IEEE vendor code                            */
   0,
   g_adapter_descr,
   0,                                      /* IRQ (USB: none)                                */
   4,                                      /* transmit queue depth                           */
   MAX_DATA_BLOCK_COUNT
};

struct MCAddrList gMCAL = { MAX_MC_ADDRS, 0 };

struct MACSpecStatus gSST = {
   sizeof(struct MACSpecStatus) - sizeof(WORD) - sizeof(BYTE),
   0xFFFFFFFFUL,           /* diagnostics not run */
   0UL,                    /* status              */
   0,                      /* packet filter       */
   0UL,
   0xFFFFFFFFUL,           /* ClearStatistics time not kept */
   0UL, 0UL, 0xFFFFFFFFUL, 0UL, 0UL, 0UL,
   0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL, 0xFFFFFFFFUL,
   0UL,
   0UL, 0xFFFFFFFFUL, 0UL, 0UL,
   0xFFFFFFFFUL, 0xFFFFFFFFUL,
   0UL, 0UL,
   0, 0
};

struct MACUpperDisp gUDT = {
   &gCCT,
   ndis_genreq_entry,
   ndis_transmit_entry,
   ndis_transfer_entry,
   ndis_release_entry,
   ndis_indon_entry,
   ndis_indoff_entry
};

/* the bound protocol (copied at Bind) */
static struct CommonChars   gProtCCT;
static struct ProtLowerDisp gProtLDT;
static USHORT               g_ndis_reg;          /* registered with the Protocol Manager */
static USHORT               g_txsel;             /* GDT selector for physical TransmitChain blocks */

/* ---- registration with the Protocol Manager (init time) ---------------------- */
USHORT ndis_init(void)
{
   static char     pmname[] = "\\DEV\\PROTMAN$";
   static struct PMReqBlock rb;
   static USHORT   hPM, action;                 /* passed to DosOpen as far pointers: statics, not stack */
   USHORT          rc, i;

   gCCT.moduleDS = rt_getds();
   rt_dh_alloc_gdt(&g_txsel, 1);                   /* used to read physical TransmitChain data blocks */
   for (i = 0; i < 6; i++)
      gSCT.permStnAddr[i] = gSCT.currStnAddr[i] = g_mymac[i];

   rc = DosOpen((PSZ)pmname, &hPM, &action, 0L, 0,
                OPEN_ACTION_OPEN_IF_EXISTS,
                OPEN_FLAGS_NOINHERIT | OPEN_SHARE_DENYREADWRITE | OPEN_ACCESS_READONLY, 0L);
   if (rc != 0 || action != FILE_EXISTED)
      return 1;                                 /* no Protocol Manager */
   rb.opcode   = PM_OP_REGISTER_MODULE;
   rb.pointer1 = (PVOID)&gCCT;
   rb.pointer2 = 0;                             /* binding list: must be NULL for a MAC */
   rc = DosDevIOCtl(0, (PVOID)&rb, IOCTL_FUN_PM, IOCTL_CAT_LAN_MANAGER, hPM);
   DosClose(hPM);
   if (rc != 0 || rb.status != 0)
      return 2;                                 /* registration failed (no [RTL88EU_NIF] in PROTOCOL.INI?) */
   g_ndis_reg = 1;
   return 0;
}

WORD ndis_is_open(void)
{
   return (gSST.status & SSTATUS_OPEN) ? 1 : 0;
}

/* ---- receive: buffer pool + ReceiveChain indication ------------------------------
 * Called from the USB completion context (task time, may not block). A buffer stays busy until ReceiveRelease
 * when the protocol returns WAIT_FOR_RELEASE. */
#define RXP_N     6
#define RXP_SIZE  1520
static UCHAR            g_rxp[RXP_N][RXP_SIZE];
static UCHAR            g_rxp_busy[RXP_N];
static struct RCBufferD g_rcb;
static BYTE             g_indicate;
static UCHAR            g_txg[RXP_SIZE];            /* gather buffer for one outgoing frame */

/* Indication exclusion (NDIS 2.0.1, IndicationOff/IndicationOn). The protocol brackets its own critical sections
 * with IndicationOff ... IndicationOn and expects no indication to be running, or to start, in between ("similar in
 * concept to disabling interrupts"). Our indications come from the USB completion on whatever CPU USBD uses, so on
 * an SMP kernel disabling the local interrupts is not enough:
 *   g_ind_off  nesting count of IndicationOff calls (and of an indication left disabled by the protocol);
 *   g_ind_busy set while one of our indication upcalls is in progress.
 * The two sides are ordered with locked instructions (Dekker): the receiver takes g_ind_busy and then looks at
 * g_ind_off; IndicationOff raises g_ind_off and then waits for g_ind_busy to drop. The wait is bounded, because the
 * protocol may legally call IndicationOff from inside our own upcall (it must pair it with IndicationOn). */
static USHORT           g_ind_off;
static UCHAR            g_ind_busy;
#define IND_SPIN_MAX    0xFFFFU

UCHAR *ndis_rx_get(USHORT *handle)
{
   USHORT i;

   for (i = 0; i < RXP_N; i++)
      if (!g_rxp_busy[i]) {
         g_rxp_busy[i] = 1;
         *handle = i;
         return g_rxp[i];
      }
   gSST.recFrNoBuff++;
   return 0;
}

void ndis_rx_put(USHORT h, USHORT len)
{
   WORD   rc;
   UCHAR *d = g_rxp[h];

   if (!(gSST.status & SSTATUS_BOUND) || gSST.filter == 0) {
      g_rxp_busy[h] = 0;                           /* nobody listening: drop */
      return;
   }
   if (rt_tas(&g_ind_busy)) {                      /* an indication is already running */
      g_rxp_busy[h] = 0;
      return;
   }
   if (g_ind_off != 0) {                           /* the protocol has indications off: drop (TCP recovers) */
      g_ind_busy = 0;
      g_rxp_busy[h] = 0;
      return;
   }
   gSST.recFrames++;
   gSST.recBytes += len;
   if (d[0] & STATION_ADDR_MC) {
      if (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF && d[3] == 0xFF && d[4] == 0xFF && d[5] == 0xFF)
         gSST.recBCFrames++;
      else
         gSST.recMCFrames++;
   }
   g_rcb.dbCount        = 1;
   g_rcb.db[0].dataLen  = len;
   g_rcb.db[0].pData    = (PBYTE)d;
   g_indicate           = 0xFF;
   rt_atomic_inc(&g_ind_off);                      /* implicitly disable indications while the protocol looks */
   rc = (*gProtLDT.pReceiveChain)(gCCT.moduleID, len, (WORD)(h + 1), (PRCBD)&g_rcb, (PBYTE)&g_indicate,
                                  gProtCCT.moduleDS);
   if (g_indicate)                                 /* the protocol left the flag set: indications back on; cleared: they stay off */
      rt_atomic_dec(&g_ind_off);
   if (rc != NDIS_WAIT_FOR_RELEASE)
      g_rxp_busy[h] = 0;
   (*gProtLDT.pIndicationComplete)(gCCT.moduleID, gProtCCT.moduleDS);
   g_ind_busy = 0;
}

/* ---- NDIS entry points (DS is the driver's on entry: loaded by the stubs) ------------------------- */

/* System request: Bind is the only one we implement. */
WORD __cdecl ndis_c_sysreq(DWORD param1, DWORD param2, WORD param3, WORD opcode)
{
   WORD   rc;
   USHORT i;

   if (!g_ndis_reg)
      rc = NDIS_DRIVER_NOT_INIT;
   else if (opcode != SYSTEM_REQUEST_BIND)
      rc = NDIS_INVALID_FUNCTION;
   else if (param1 == 0 || param3 != 0)
      rc = NDIS_INVALID_PARAMETER;
   else if (gSST.status & SSTATUS_BOUND)
      rc = NDIS_ALREADY_STARTED;
   else {
      PCCT pc = (PCCT)param1;                      /* far pointer values passed as dwords */

      if (rtl8188eu_early_mac() == 0)             /* real (eFuse) address before the protocol copies it */
         for (i = 0; i < 6; i++)
            gSCT.permStnAddr[i] = gSCT.currStnAddr[i] = g_mymac[i];
      gProtCCT = *pc;                             /* the protocol's CCT                  */
      gProtLDT = *gProtCCT.pLowDisp;              /* and its lower dispatch table         */
      *(PCCT FAR *)param2 = &gCCT;                /* tell it ours                         */
      gSST.status |= SSTATUS_BOUND | SSTATUS_HW_OK | SSTATUS_OPEN;   /* no open/close support advertised: open once bound */
      rc = NDIS_SUCCESS;
   }
   return rc;
}

WORD __cdecl ndis_c_genreq(WORD protID, WORD reqHandle, WORD param1, DWORD param2, WORD opcode)
{
   WORD   rc = NDIS_SUCCESS;
   USHORT i;

   switch (opcode) {
   case OPEN_ADAPTER:
      gSST.status |= SSTATUS_OPEN | SSTATUS_HW_OK;
      break;
   case CLOSE_ADAPTER:
      gSST.status &= ~SSTATUS_OPEN;
      break;
   case SET_PACKET_FILTER:
      if (param2 != 0 || (param1 & PACK_FILTER_RESERVED))
         rc = NDIS_INVALID_PARAMETER;
      else
         gSST.filter = param1;
      break;
   case SET_STATION_ADDRESS:
      if (param1 != 0 || (*(PBYTE)param2 & STATION_ADDR_MC))
         rc = NDIS_INVALID_PARAMETER;
      else
         for (i = 0; i < STATION_ADDR_LEN; i++)
            gSCT.currStnAddr[i] = ((PBYTE)param2)[i];   /* recorded; the radio keeps its own MAC */
      break;
   case ADD_MULTICAST_ADDRESS:
   case DELETE_MULTICAST_ADDRESS:
   case UPDATE_STATISTICS:
   case SET_LOOK_AHEAD:
   case RESET_MAC:
      break;                                           /* we accept all multicast; stats are live */
   case CLEAR_STATISTICS:
      gSST.recFrames = gSST.recBCFrames = gSST.recMCFrames = gSST.recFrCRC = gSST.recFrNoBuff = 0;
      gSST.trFrames = gSST.trBCFrames = gSST.trMCFrames = gSST.trFrHW = 0;
      break;
   default:
      rc = NDIS_NOT_SUPPORTED;
   }
   return rc;
}

/* Transmit: gather the buffer chain into one Ethernet frame and queue it for the radio. The data is copied before
 * returning, so SUCCESS (no TransmitConfirm) is the right answer. */
WORD __cdecl ndis_c_transmit(WORD protID, WORD reqHandle, PTCBD pTCBufferD)
{
   WORD   rc = NDIS_SUCCESS;
   USHORT n = 0, i, k;
   int    r;

   if (!(gSST.status & SSTATUS_OPEN)) {
      rc = NDIS_INVALID_FUNCTION;
   } else {
      if (pTCBufferD->immedLen && pTCBufferD->immedLen <= RXP_SIZE) {
         rt_movmem((PVOID)g_txg, (PVOID)pTCBufferD->pImmed, pTCBufferD->immedLen);
         n = pTCBufferD->immedLen;
      }
      for (i = 0; i < pTCBufferD->dbCount && i < MAX_DATA_BLOCK_COUNT; i++) {
         k = pTCBufferD->db[i].dataLen;
         if ((USHORT)(n + k) > RXP_SIZE || (USHORT)(n + k) < n)
            break;
         if (pTCBufferD->db[i].ptrType == 0) {         /* physical address: map it through our GDT selector */
            ULONG ph = *(ULONG FAR *)&pTCBufferD->db[i].pData;

            rt_dh_phys_to_gdt(ph, k, g_txsel);
            rt_movmem((PVOID)(g_txg + n), MAKEP(g_txsel, 0), k);
         } else {                                       /* GDT virtual address */
            rt_movmem((PVOID)(g_txg + n), (PVOID)pTCBufferD->db[i].pData, k);
         }
         n = (USHORT)(n + k);
      }
      r = rtl8188eu_ndis_tx(g_txg, n);
      if (r == 0) {
         gSST.trFrames++;
         gSST.trBytes += n;
         if (g_txg[0] & STATION_ADDR_MC)
            gSST.trBCFrames++;
      } else if (r == -2) {
         rc = NDIS_OUT_OF_RESOURCE;                      /* all transmit slots busy: protocol retries */
      } else {
         gSST.trFrHW++;                                  /* no connection / keys yet: the frame is lost */
      }
   }
   return rc;
}

WORD __cdecl ndis_c_transfer(PUSHORT pBytesCopied, WORD frameOffset, PTDBD pTDBufferD)
{
   *pBytesCopied = 0;
   return NDIS_INVALID_FUNCTION;                       /* we only indicate with ReceiveChain */
}

WORD __cdecl ndis_c_release(WORD reqHandle)
{
   if (reqHandle >= 1 && reqHandle <= RXP_N) {
      g_rxp_busy[reqHandle - 1] = 0;
      return NDIS_SUCCESS;
   }
   return NDIS_INVALID_PARAMETER;
}

/* NDIS 2.0.1: both calls return with interrupts disabled; the caller re-enables them. */
WORD __cdecl ndis_c_indoff(void)
{
   USHORT n = 0;

   rt_cli();
   rt_atomic_inc(&g_ind_off);
   while (g_ind_busy && ++n < IND_SPIN_MAX)        /* let an indication in progress on another CPU finish */
      ;
   return NDIS_SUCCESS;
}

WORD __cdecl ndis_c_indon(void)
{
   if (g_ind_off)
      rt_atomic_dec(&g_ind_off);
   rt_cli();
   return NDIS_SUCCESS;
}

/* Adopt the chip's (eFuse) address for the NDIS tables and the radio, unless the protocol stack has already bound
 * with a different one (it copies the address at Bind time and must keep seeing the same one). Returns 0 when the
 * tables now carry m, -1 otherwise. */
int ndis_set_mac(UCHAR *m)
{
   USHORT i;
   int    same = 1;

   for (i = 0; i < 6; i++)
      if (gSCT.currStnAddr[i] != m[i])
         same = 0;
   if (same)
      return 0;
   if (gSST.status & SSTATUS_BOUND)
      return -1;
   for (i = 0; i < 6; i++) {
      gSCT.permStnAddr[i] = gSCT.currStnAddr[i] = m[i];
      g_mymac[i] = m[i];
   }
   return 0;
}

/* Give back a receive buffer that was filled but is not to be indicated (e.g. a DHCP reply). */
void ndis_rx_free(USHORT h)
{
   if (h < RXP_N)
      g_rxp_busy[h] = 0;
}

/* ===== IP interface number =================================================
 * The TCP/IP interface number (lanN) is the position of our entry in the Bindings list of [TCPIP_NIF] in
 * \IBMCOM\PROTOCOL.INI, counting from 0 - the same rule the Wireless LAN Monitor uses. Read once at INIT; if the
 * file or the entry is missing it stays lan1. */
USHORT g_lannum = 1;

#define PL_MAX 600
static char   g_pl[PL_MAX + 1];
static USHORT g_pln;
static UCHAR  g_in_tcpip;
static char   g_pbuf[256];

static char lc(char c)
{
   return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

/* s[0..n) equals the lower-case string t, ignoring case */
static int eqn(char *s, USHORT n, char *t)
{
   USHORT i;

   for (i = 0; i < n; i++)
      if (t[i] == 0 || lc(s[i]) != t[i])
         return 0;
   return t[n] == 0;
}

static void pi_line(char *l)
{
   char  *e, *k, *p, *v;
   USHORT idx;

   while (*l == ' ' || *l == '\t') l++;
   if (*l == 0 || *l == ';')
      return;
   if (*l == '[') {
      l++;
      e = l;
      while (*e && *e != ']') e++;
      while (e > l && (e[-1] == ' ' || e[-1] == '\t')) e--;
      g_in_tcpip = (UCHAR)eqn(l, (USHORT)(e - l), "tcpip_nif");
      return;
   }
   if (!g_in_tcpip)
      return;
   k = l;
   p = l;
   while (*p && *p != '=') p++;
   if (*p != '=')
      return;
   e = p;
   while (e > k && (e[-1] == ' ' || e[-1] == '\t')) e--;
   if (!eqn(k, (USHORT)(e - k), "bindings"))
      return;
   v = p + 1;
   idx = 0;
   for (;;) {
      char *a, *b;

      while (*v == ' ' || *v == '\t') v++;
      a = v;
      while (*v && *v != ',') v++;
      b = v;
      while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
      if (eqn(a, (USHORT)(b - a), "rtl88eu_nif")) {
         g_lannum = idx;
         return;
      }
      if (*v == 0)
         return;
      v++;
      idx++;
   }
}

void ndis_find_lan(void)
{
   static char path[] = "\\IBMCOM\\PROTOCOL.INI";
   static USHORT h, action, got;                /* passed to DOS calls as far pointers: statics, not stack */
   USHORT i;
   char   c;

   if (DosOpen((PSZ)path, &h, &action, 0L, 0,
               OPEN_ACTION_OPEN_IF_EXISTS | OPEN_ACTION_FAIL_IF_NEW,
               OPEN_FLAGS_NOINHERIT | OPEN_SHARE_DENYNONE | OPEN_ACCESS_READONLY, 0L) != 0)
      return;
   g_pln = 0;
   g_in_tcpip = 0;
   while (DosRead(h, (PVOID)g_pbuf, sizeof(g_pbuf), &got) == 0 && got) {
      for (i = 0; i < got; i++) {
         c = g_pbuf[i];
         if (c == '\n') {
            g_pl[g_pln] = 0;
            pi_line(g_pl);
            g_pln = 0;
         } else if (c != '\r' && c != 0x1A && g_pln < PL_MAX)
            g_pl[g_pln++] = c;
      }
   }
   if (g_pln) {
      g_pl[g_pln] = 0;
      pi_line(g_pl);
   }
   DosClose(h);
}
