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
static ULONG                g_hook;              /* context hook that delivers received frames at task time */
static USHORT               g_hook_ok;           /* 1 when the hook was allocated */
USHORT                      g_use_hook = 1;      /* 1 = deliver received frames from the context hook (debug "uh 1"), 0 = directly from the USB callback */
USHORT                      g_hk_run, g_hk_maxq, g_hk_arm_fail;

/* ---- registration with the Protocol Manager (init time) ---------------------- */
USHORT ndis_init(void)
{
   static char     pmname[] = "\\DEV\\PROTMAN$";
   static struct PMReqBlock rb;
   static USHORT   hPM, action;                 /* passed to DosOpen as far pointers: statics, not stack */
   USHORT          rc, i;

   gCCT.moduleDS = rt_getds();
   rt_dh_alloc_gdt(&g_txsel, 1);                   /* used to read physical TransmitChain data blocks */
   g_hook_ok = (USHORT)(rt_dh_alloc_ctxhook(rt_hook_offset(), &g_hook) == 0);   /* receive delivery at task time */
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
static struct RCBufferD g_rcb[RXP_N];           /* one descriptor per buffer: a protocol that answers WAIT_FOR_RELEASE may still look at it from another CPU */
static BYTE             g_indicate;
static USHORT           g_txsel_lock;               /* owner (CPU + 1) of g_txsel while a physical block is copied */

/* Indication exclusion (NDIS 2.0.1, IndicationOff/IndicationOn). The protocol brackets its own critical sections
 * with IndicationOff ... IndicationOn and expects no indication to be running, or to start, in between ("similar in
 * concept to disabling interrupts"). Our indications come from the USB completion on whatever CPU USBD uses, so on
 * an SMP kernel disabling the local interrupts is not enough:
 *   g_ind_off    nesting count of IndicationOff calls (and of an indication left disabled by the protocol);
 *   g_ind_owner  0, or (CPU id + 1) of the CPU whose indication upcall is in progress.
 * The two sides are ordered with locked instructions (Dekker): the receiver takes g_ind_owner and then looks at
 * g_ind_off; IndicationOff raises g_ind_off and then waits for g_ind_owner to drop. The protocol may legally call
 * IndicationOff from inside our own upcall: that case is recognised by the owner being the calling CPU, so the wait
 * for a DIFFERENT CPU does not need a short bound (an earlier version gave up after 65535 passes, a few tens of
 * microseconds: shorter than an indication into TCP/IP, so the protocol's critical section could overlap a running
 * indication). A wait that still runs out is counted (INDTMO) and marked with '%' on COM1. */
static USHORT           g_ind_off;
USHORT                  g_rel_n, g_rel_avg8, g_rel_max, g_rc_wait, g_rc_succ, g_rc_other;   /* ReceiveRelease: count, delay after the indication (ms, avg x8, max); how ReceiveChain returned */
static ULONG            g_rxp_t0[6];
USHORT                  g_if_on, g_if_off, g_ind_cli;   /* interrupt flag when an indication starts; 1 = call the protocol with interrupts disabled
 */
USHORT                  g_rc_n, g_rc_avg8, g_rc_max, g_rc_slow;   /* ReceiveChain + IndicationComplete: calls, duration average x8 / maximum (ms), calls over 5 ms */
/* What ReceiveChain returned when it was neither SUCCESS nor WAIT_FOR_RELEASE: up to 4 distinct status values with their
 * counts, and the last such frame (length, ethertype, IP protocol, destination address last byte). */
static USHORT           g_rcv[4], g_rcc[4], g_rc_last, g_rc_len, g_rc_et, g_rc_ipp, g_rc_dst;
USHORT                  g_ind_drop_busy, g_ind_drop_off;   /* frames dropped because an indication was running / the protocol had indications off */
static USHORT           g_ind_owner;
#define IND_WAIT_ROUNDS 16                          /* x 65536 PAUSE passes: tens of milliseconds at most */
static USHORT           g_ind_tmo, g_ind_reent, g_cpu_rx;   /* g_cpu_rx: bit n set = an indication ran on the CPU with APIC id n (mod 16) */

/* Diagnostics: 0 IndicationOff waits that ran out, 1 IndicationOff calls from inside our own upcall,
 * 2 received frames dropped because indications were off or busy. */
USHORT ndis_diag(USHORT i)
{
   if (i == 0) return g_ind_tmo;
   if (i == 1) return g_ind_reent;
   if (i == 2) return (USHORT)(g_ind_drop_busy + g_ind_drop_off);
   if (i == 3) return g_cpu_rx;
   if (i >= 4 && i <= 7) return g_rcv[i - 4];
   if (i >= 8 && i <= 11) return g_rcc[i - 8];
   if (i == 12) return g_rc_last;
   if (i == 13) return g_rc_len;
   if (i == 14) return g_rc_et;
   if (i == 15) return g_rc_ipp;
   if (i == 16) return g_rc_dst;
   return 0xFFFF;
}

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

static void deliver(USHORT h, USHORT len)
{
   WORD   rc;
   UCHAR *d = g_rxp[h];
   struct RCBufferD *rcb = &g_rcb[h];
   ULONG  rc_t0;
   UCHAR  usecli;
   USHORT cid;

   if (!(gSST.status & SSTATUS_BOUND) || gSST.filter == 0) {
      g_rxp_busy[h] = 0;                           /* nobody listening: drop */
      return;
   }
   cid = (USHORT)(rt_cpu_id() + 1);
   g_cpu_rx |= (USHORT)(1U << ((cid - 1) & 15));
   if (rt_lock_try(&g_ind_owner, cid)) {   /* an indication is already running */
      g_ind_drop_busy++;
      g_rxp_busy[h] = 0;
      return;
   }
   if (g_ind_off != 0) {                           /* the protocol has indications off: drop (TCP recovers) */
      rt_lock_release(&g_ind_owner);
      g_ind_drop_off++;
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
   rcb->dbCount         = 1;
   rcb->db[0].dataLen   = len;
   rcb->db[0].pData     = (PBYTE)d;
   g_indicate           = 0xFF;
   rt_atomic_inc(&g_ind_off);                      /* implicitly disable indications while the protocol looks */
   {
      USHORT ifl = rt_getif();

      if (ifl) g_if_on++; else g_if_off++;
      usecli = (UCHAR)(g_ind_cli && ifl);
   }
   if (usecli)
      rt_cli();
   rc_t0 = rtl8188eu_ms();
   rc = (*gProtLDT.pReceiveChain)(gCCT.moduleID, len, (WORD)(h + 1), (PRCBD)rcb, (PBYTE)&g_indicate,
                                  gProtCCT.moduleDS);
   if (g_indicate)                                 /* the protocol left the flag set: indications back on; cleared: they stay off */
      rt_atomic_dec_nz(&g_ind_off);
   if (rc == NDIS_WAIT_FOR_RELEASE) { g_rc_wait++; g_rxp_t0[h] = rc_t0; }
   else if (rc == NDIS_SUCCESS)     g_rc_succ++;
   else {
      USHORT q;

      g_rc_other++;
      g_rc_last = rc;
      g_rc_len  = len;
      g_rc_et   = (USHORT)((d[12] << 8) | d[13]);
      g_rc_ipp  = (USHORT)(len > 23 ? d[23] : 0);
      g_rc_dst  = d[5];
      for (q = 0; q < 4; q++)
         if (g_rcc[q] == 0 || g_rcv[q] == rc) {
            g_rcv[q] = rc;
            g_rcc[q]++;
            break;
         }
   }
   if (rc != NDIS_WAIT_FOR_RELEASE)
      g_rxp_busy[h] = 0;
   (*gProtLDT.pIndicationComplete)(gCCT.moduleID, gProtCCT.moduleDS);
   if (usecli)
      rt_sti();
   {
      USHORT dt = (USHORT)(rtl8188eu_ms() - rc_t0);

      g_rc_n++;
      g_rc_avg8 = (USHORT)(g_rc_avg8 - (g_rc_avg8 >> 3) + dt);
      if (dt > g_rc_max) g_rc_max = dt;
      if (dt > 5) g_rc_slow++;
   }
   rt_lock_release(&g_ind_owner);
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
   USHORT n = 0, i, k, slot, fl;
   UCHAR  *g, mc;
   int    r, bad = 0;

   if (!(gSST.status & SSTATUS_OPEN))
      return NDIS_INVALID_FUNCTION;
   if (!rtl8188eu_tx_ready()) {
      gSST.trFrHW++;                                     /* no connection / keys yet: the frame is lost */
      return NDIS_SUCCESS;
   }
   slot = rtl8188eu_ndis_slot();
   if (slot == 0xFFFF)
      return NDIS_OUT_OF_RESOURCE;                       /* all transmit slots busy: protocol retries */
   g = usb_tx_buf(slot) + TX_ETH_OFF;
   if (pTCBufferD->immedLen) {
      if (pTCBufferD->immedLen > MAX_FRAME_SIZE)
         bad = 1;
      else {
         rt_movmem((PVOID)g, (PVOID)pTCBufferD->pImmed, pTCBufferD->immedLen);
         n = pTCBufferD->immedLen;
      }
   }
   for (i = 0; !bad && i < pTCBufferD->dbCount && i < MAX_DATA_BLOCK_COUNT; i++) {
      k = pTCBufferD->db[i].dataLen;
      if ((USHORT)(n + k) > MAX_FRAME_SIZE || (USHORT)(n + k) < n) {
         bad = 1;
         break;
      }
      if (pTCBufferD->db[i].ptrType == 0) {            /* physical address: map it through our GDT selector */
         ULONG ph = *(ULONG FAR *)&pTCBufferD->db[i].pData;

         fl = rt_irq_save();
         if (rt_lock_acquire(&g_txsel_lock, (USHORT)(rt_cpu_id() + 1))) {
            rt_irq_restore(fl);                         /* this CPU already holds it (re-entered): do not wait */
            bad = 1;
            break;
         }
         rt_dh_phys_to_gdt(ph, k, g_txsel);
         rt_movmem((PVOID)(g + n), MAKEP(g_txsel, 0), k);
         rt_lock_release(&g_txsel_lock);
         rt_irq_restore(fl);
      } else {                                          /* GDT virtual address */
         rt_movmem((PVOID)(g + n), (PVOID)pTCBufferD->db[i].pData, k);
      }
      n = (USHORT)(n + k);
   }
   if (bad) {
      usb_tx_release(slot);
      gSST.trFrHW++;                                     /* larger than an Ethernet frame (or re-entered): lost */
      return NDIS_SUCCESS;
   }
   mc = (UCHAR)(g[0] & STATION_ADDR_MC);                 /* the slot is rewritten by the send: keep the group bit */
   r = rtl8188eu_ndis_tx_slot(slot, n);                  /* releases the slot itself when it fails */
   if (r == 0) {
      gSST.trFrames++;
      gSST.trBytes += n;
      if (mc)
         gSST.trBCFrames++;
   } else if (r == -2) {
      rc = NDIS_OUT_OF_RESOURCE;                         /* could not be queued now: protocol retries */
   } else {
      gSST.trFrHW++;
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
      USHORT dt = (USHORT)(rtl8188eu_ms() - g_rxp_t0[reqHandle - 1]);

      g_rel_n++;
      g_rel_avg8 = (USHORT)(g_rel_avg8 - (g_rel_avg8 >> 3) + dt);
      if (dt > g_rel_max) g_rel_max = dt;
      g_rxp_busy[reqHandle - 1] = 0;
      return NDIS_SUCCESS;
   }
   return NDIS_INVALID_PARAMETER;
}

/* NDIS 2.0.1: both calls return with interrupts disabled; the caller re-enables them. */
WORD __cdecl ndis_c_indoff(void)
{
   USHORT own;

   rt_cli();
   rt_atomic_inc(&g_ind_off);
   own = g_ind_owner;                              /* read after the locked increment (Dekker, see above) */
   if (own != 0) {
      if (own == (USHORT)(rt_cpu_id() + 1))
         g_ind_reent++;                            /* called from inside our own upcall: never wait for ourselves */
      else if (rt_wait_zero(&g_ind_owner, IND_WAIT_ROUNDS)) {
         g_ind_tmo++;                              /* the indication on the other CPU is still running */
         tcom_always('%');
      }
   }
   return NDIS_SUCCESS;
}

static USHORT g_q_head, g_q_tail;                 /* delivery queue (see ndis_rx_put) */
static void arm_hook(void);

WORD __cdecl ndis_c_indon(void)
{
   rt_atomic_dec_nz(&g_ind_off);                   /* one atomic step: two CPUs must not both pass a "> 0" test */
   if (g_hook_ok && g_use_hook && g_ind_off == 0 && g_q_tail != g_q_head)
      arm_hook();                                   /* frames were held back while indications were off */
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

/* Delivery from the context hook: the frame is queued by the USB completion callback and the hook runs the protocol's
 * receive upcall at task time (the way other USB network drivers do it). */
static UCHAR  g_q_h[RXP_N];
static USHORT g_q_len[RXP_N];

void __cdecl ndis_hook_c(void)
{
   g_hk_run++;
   while (g_q_tail != g_q_head) {
      USHORT i = (USHORT)(g_q_tail % RXP_N), h, len;

      if (g_ind_off != 0)                          /* the protocol has indications off: IndicationOn re-arms us */
         return;
      h   = g_q_h[i];
      len = g_q_len[i];
      g_q_tail++;
      deliver(h, len);
   }
}

static void arm_hook(void)
{
   if (rt_dh_arm_ctxhook(0UL, g_hook) != 0)
      g_hk_arm_fail++;                              /* already armed: it will run and find the frame */
}

void ndis_rx_put(USHORT h, USHORT len)
{
   if (!g_hook_ok || !g_use_hook) {
      deliver(h, len);
      return;
   }
   if (!(gSST.status & SSTATUS_BOUND) || gSST.filter == 0) {
      g_rxp_busy[h] = 0;
      return;
   }
   g_q_h[g_q_head % RXP_N]   = (UCHAR)h;
   g_q_len[g_q_head % RXP_N] = len;
   rt_atomic_inc(&g_q_head);                        /* publish the entry (locked instruction = barrier) */
   {
      USHORT q = (USHORT)(g_q_head - g_q_tail);

      if (q > g_hk_maxq)
         g_hk_maxq = q;
   }
   arm_hook();
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
