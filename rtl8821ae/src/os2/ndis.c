/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ndis.c - NDIS 2.0.1 MAC front-end of the RTL8821AE OS/2 driver (from qc_ndis.c of the QCA6174 driver).
 *
 * Adapted from rt_ndis.c of the OS2World RTL88EU driver (GPL-2.0-only), written from the NDIS 2.0.1
 * specification: at INIT the driver registers its common characteristics table with the Protocol Manager
 * (PROTMAN$); the TCP/IP stack then binds (SystemRequest) and uses the upper dispatch table. The SMP-safe
 * indication exclusion (IndicationOff/On against a running ReceiveChain on another CPU) is kept as it was.
 *
 * TransmitChain hands the gathered Ethernet frame to the station (sta_tx); while there is no link the frame is
 * dropped and counted, as the RTL driver does. Received frames come from the station through
 * ndis_rx_get()/ndis_rx_put() and are indicated from the context hook.
 */
#include "ndis.h"

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


/* ---- tables ---------------------------------------------------------------------------------------- */
static char g_adapter_descr[] = DRV_DESC;

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
   LOWER_PROTOCOL_PHYS, LOWER_INTERFACE_PHYS,
   0, 0,
   ndis_sysreq_entry,
   &gSCT, &gSST, &gUDT,
   0, 0, 0
};

struct MACSpecChars gSCT = {
   sizeof(struct MACSpecChars),
   MAC_TYPE_NAME,
   STATION_ADDR_LEN,
   { 0, 0, 0, 0, 0, 0 },
   { 0, 0, 0, 0, 0, 0 },
   0UL,
   &gMCAL,
   MAX_LINK_SPEED,
   SCTFLAG_BROADCAST | SCTFLAG_MULTICAST | SCTFLAG_SET_STN_ADDR |
   SCTFLAG_STAT_ALWAYS_CURR | SCTFLAG_RECEIVE_CHAIN,
   MAX_FRAME_SIZE,
   8UL * 1600UL, 1600,
   8UL * 1600UL, 1600,
   { 0xFF, 0xFF, 0xFF },
   0,
   g_adapter_descr,
   0,                                      /* IRQ: filled in at INIT                                      */
   4,
   MAX_DATA_BLOCK_COUNT
};

struct MCAddrList gMCAL = { MAX_MC_ADDRS, 0 };

struct MACSpecStatus gSST = {
   sizeof(struct MACSpecStatus) - sizeof(WORD) - sizeof(BYTE),
   0xFFFFFFFFUL, 0UL, 0, 0UL, 0xFFFFFFFFUL,
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

static struct CommonChars   gProtCCT;
static struct ProtLowerDisp gProtLDT;
static USHORT               g_ndis_reg;
static USHORT               g_txsel_lock;

/* ---- registration with the Protocol Manager (INIT time) -------------------------------------------- */
#define RXP_N     128
#define RXP_SIZE  1520
static DMA    g_rxp[RXP_N];                 /* receive buffers in DMA memory (far): DGROUP stays small     */
static UCHAR  g_rxp_busy[RXP_N];
static struct RCBufferD g_rcb[RXP_N];
static DMA    g_txbuf;                      /* TransmitChain gathers the frame here                         */

/* The receive buffers and the transmit gather buffer live in DMA memory. They are allocated when the protocol
 * binds (task time), not at INIT: touching DMA memory through its GDT selector at INIT hung the boot. */
static USHORT g_bufs_done;

static void ndis_bufs(void)
{
   USHORT i;

   if (g_bufs_done)
      return;
   g_bufs_done = 1;
   for (i = 0; i < RXP_N; i++)
      if (dma_get(RXP_SIZE, &g_rxp[i]) == 0)
         g_rxp_busy[i] = 0;
   dma_get(MAX_FRAME_SIZE + 64, &g_txbuf);
}

USHORT ndis_init(void)
{
   static char     pmname[] = "\\DEV\\PROTMAN$";
   static struct PMReqBlock rb;
   static USHORT   hPM, action;
   USHORT          rc, i;

   gCCT.moduleDS = rt_getds();
   gSCT.irq = g_pci_irq;
   for (i = 0; i < RXP_N; i++)
      g_rxp_busy[i] = 2;                    /* not available until the buffers exist (ndis_bufs, at bind)   */
   dd_step(11);                                /* boot diagnostics: (the buffers are no longer allocated here) */

   rc = DosOpen((PSZ)pmname, &hPM, &action, 0L, 0,
                OPEN_ACTION_OPEN_IF_EXISTS,
                OPEN_FLAGS_NOINHERIT | OPEN_SHARE_DENYREADWRITE | OPEN_ACCESS_READONLY, 0L);
   dd_step(12);                                /* boot diagnostics: DosOpen of PROTMAN$ returned              */
   if (rc != 0 || action != FILE_EXISTED)
      return 1;
   rb.opcode   = PM_OP_REGISTER_MODULE;
   rb.pointer1 = (PVOID)&gCCT;
   rb.pointer2 = 0;
   rc = DosDevIOCtl(0, (PVOID)&rb, IOCTL_FUN_PM, IOCTL_CAT_LAN_MANAGER, hPM);
   DosClose(hPM);
   if (rc != 0 || rb.status != 0)
      return 2;
   g_ndis_reg = 1;
   return 0;
}

WORD ndis_is_open(void)
{
   return (gSST.status & SSTATUS_OPEN) ? 1 : 0;
}

/* Adopt the chip's address unless the protocol has already bound with another one. */
int ndis_set_mac(UCHAR *m)
{
   USHORT i;

   if (gSST.status & SSTATUS_BOUND) {
      for (i = 0; i < 6; i++)
         if (gSCT.currStnAddr[i] != m[i])
            return -1;
      return 0;
   }
   for (i = 0; i < 6; i++)
      gSCT.permStnAddr[i] = gSCT.currStnAddr[i] = m[i];
   return 0;
}

/* ---- receive ---------------------------------------------------------------------------------------- */
static BYTE   g_indicate;
static USHORT g_ind_off, g_ind_owner;
USHORT        g_ind_drop_busy, g_ind_drop_off, g_ind_tmo, g_ind_reent, g_q_hi, g_hook_n, g_hook_max;
#define IND_WAIT_ROUNDS 16

UCHAR FAR *ndis_rx_get(USHORT *handle)
{
   USHORT i;

   for (i = 0; i < RXP_N; i++)
      if (g_rxp_busy[i] == 0) {
         g_rxp_busy[i] = 1;
         *handle = i;
         return DPTR(g_rxp[i]);
      }
   gSST.recFrNoBuff++;
   return 0;
}

void ndis_rx_free(USHORT h)
{
   if (h < RXP_N)
      g_rxp_busy[h] = 0;
}

static UCHAR g_ind_pending;
extern USHORT g_in_hook;
static void indication_complete(void);

static void deliver(USHORT h, USHORT len)
{
   WORD   rc;
   UCHAR FAR *d = DPTR(g_rxp[h]);
   struct RCBufferD *rcb = &g_rcb[h];
   USHORT cid;

   if (!(gSST.status & SSTATUS_BOUND) || gSST.filter == 0) {
      g_rxp_busy[h] = 0;
      return;
   }
   cid = (USHORT)(rt_cpu_id() + 1);
   if (rt_lock_try(&g_ind_owner, cid)) {
      g_ind_drop_busy++;
      g_rxp_busy[h] = 0;
      return;
   }
   if (g_ind_off != 0) {
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
   rcb->dbCount       = 1;
   rcb->db[0].dataLen = len;
   rcb->db[0].pData   = (PBYTE)d;
   g_indicate = 0xFF;
   rt_atomic_inc(&g_ind_off);
   rc = (*gProtLDT.pReceiveChain)(gCCT.moduleID, len, (WORD)(h + 1), (PRCBD)rcb, (PBYTE)&g_indicate,
                                  gProtCCT.moduleDS);
   if (g_indicate)
      rt_atomic_dec_nz(&g_ind_off);
   if (rc != NDIS_WAIT_FOR_RELEASE)
      g_rxp_busy[h] = 0;
   g_ind_pending = 1;                              /* IndicationComplete once per batch (ndis_hook_deliver)  */
   rt_lock_release(&g_ind_owner);
}

/* End of a batch of indications. */
static void indication_complete(void)
{
   if (!g_ind_pending)
      return;
   g_ind_pending = 0;
   if (rt_lock_try(&g_ind_owner, (USHORT)(rt_cpu_id() + 1)))
      return;
   (*gProtLDT.pIndicationComplete)(gCCT.moduleID, gProtCCT.moduleDS);
   rt_lock_release(&g_ind_owner);
}

/* Frames are delivered from the context hook (task time), never from the interrupt handler. */
static UCHAR  g_q_h[RXP_N];
static USHORT g_q_len[RXP_N];
static USHORT g_q_head, g_q_tail;

void ndis_hook_deliver(void)
{
   USHORT depth = (USHORT)(g_q_head - g_q_tail);

   g_hook_n++;
   if (depth > g_q_hi)
      g_q_hi = depth;                             /* deepest backlog seen when the hook ran                */
   while (g_q_tail != g_q_head) {
      USHORT i = (USHORT)(g_q_tail % RXP_N);

      if (g_ind_off != 0)
         break;                                   /* IndicationOn re-arms the hook                         */
      g_q_tail++;
      deliver(g_q_h[i], g_q_len[i]);
   }
   indication_complete();
}

void ndis_rx_put(USHORT h, USHORT len)
{
   if (!(gSST.status & SSTATUS_BOUND) || gSST.filter == 0) {
      g_rxp_busy[h] = 0;
      return;
   }
   g_q_h[g_q_head % RXP_N]   = (UCHAR)h;
   g_q_len[g_q_head % RXP_N] = len;
   rt_atomic_inc(&g_q_head);
   if (!g_in_hook)
      hook_arm();
}

/* ---- NDIS entry points ------------------------------------------------------------------------------ */
WORD __cdecl ndis_c_sysreq(DWORD param1, DWORD param2, WORD param3, WORD opcode)
{
   if (!g_ndis_reg)
      return NDIS_DRIVER_NOT_INIT;
   if (opcode != SYSTEM_REQUEST_BIND)
      return NDIS_INVALID_FUNCTION;
   if (param1 == 0 || param3 != 0)
      return NDIS_INVALID_PARAMETER;
   if (gSST.status & SSTATUS_BOUND)
      return NDIS_ALREADY_STARTED;
   ndis_bufs();
   gProtCCT = *(PCCT)param1;
   gProtLDT = *gProtCCT.pLowDisp;
   *(PCCT FAR *)param2 = &gCCT;
   gSST.status |= SSTATUS_BOUND | SSTATUS_HW_OK | SSTATUS_OPEN;
   return NDIS_SUCCESS;
}

WORD __cdecl ndis_c_genreq(WORD protID, WORD reqHandle, WORD param1, DWORD param2, WORD opcode)
{
   WORD   rc = NDIS_SUCCESS;
   USHORT i;

   (void)protID; (void)reqHandle;
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
            gSCT.currStnAddr[i] = ((PBYTE)param2)[i];
      break;
   case ADD_MULTICAST_ADDRESS:
   case DELETE_MULTICAST_ADDRESS:
   case UPDATE_STATISTICS:
   case SET_LOOK_AHEAD:
   case RESET_MAC:
      break;
   case CLEAR_STATISTICS:
      gSST.recFrames = gSST.recBCFrames = gSST.recMCFrames = gSST.recFrCRC = gSST.recFrNoBuff = 0;
      gSST.trFrames = gSST.trBCFrames = gSST.trMCFrames = gSST.trFrHW = 0;
      break;
   default:
      rc = NDIS_NOT_SUPPORTED;
   }
   return rc;
}

/* Gather the chain into one Ethernet frame and hand it to the station. The data is copied before the
 * call returns, so SUCCESS (no TransmitConfirm) is the right answer. */
/* Why TransmitChain did not send (status text NDIS): 0 adapter not open, 1 not connected, 2 no transmit buffer, 3 too
 * big, 4 physical block mapping busy, 5 sta_tx refused (no link / no keys), 6 sta_tx busy, 7 queued */
USHORT g_tx_calls, g_tx_why[8], g_tx_last_len, g_tx_last_et;
int    g_tx_last_rc;

WORD __cdecl ndis_c_transmit(WORD protID, WORD reqHandle, PTCBD pTCBufferD)
{
   USHORT n = 0, i, k, fl;
   UCHAR FAR *g;
   int    r;

   (void)protID; (void)reqHandle;
   g_tx_calls++;
   if (!(gSST.status & SSTATUS_OPEN)) {
      g_tx_why[0]++;
      return NDIS_INVALID_FUNCTION;
   }
   if (!g_joined || !g_txbuf.phys) {
      g_tx_why[g_joined ? 2 : 1]++;
      gSST.trFrHW++;                             /* no connection: the frame is lost (TCP/IP retries later)  */
      return NDIS_SUCCESS;
   }
   g = DPTR(g_txbuf);
   if (pTCBufferD->immedLen) {
      if (pTCBufferD->immedLen > MAX_FRAME_SIZE) { g_tx_why[3]++; gSST.trFrHW++; return NDIS_SUCCESS; }
      rt_movmem((PVOID)g, (PVOID)pTCBufferD->pImmed, pTCBufferD->immedLen);
      n = pTCBufferD->immedLen;
   }
   for (i = 0; i < pTCBufferD->dbCount && i < MAX_DATA_BLOCK_COUNT; i++) {
      k = pTCBufferD->db[i].dataLen;
      if ((USHORT)(n + k) > MAX_FRAME_SIZE || (USHORT)(n + k) < n) { g_tx_why[3]++; gSST.trFrHW++; return NDIS_SUCCESS; }
      if (pTCBufferD->db[i].ptrType == 0) {      /* physical address                                        */
         ULONG ph = *(ULONG FAR *)&pTCBufferD->db[i].pData;
         USHORT s;

         fl = rt_irq_save();
         if (rt_lock_acquire(&g_txsel_lock, (USHORT)(rt_cpu_id() + 1))) {
            rt_irq_restore(fl);
            g_tx_why[4]++;
            gSST.trFrHW++;
            return NDIS_SUCCESS;
         }
         s = scr_map(SCR_TX, ph, k);
         if (s)
            rt_movmem((PVOID)(g + n), MAKEP(s, 0), k);
         rt_lock_release(&g_txsel_lock);
         rt_irq_restore(fl);
      } else {
         rt_movmem((PVOID)(g + n), (PVOID)pTCBufferD->db[i].pData, k);
      }
      n = (USHORT)(n + k);
   }
   r = sta_tx(g, n);
   g_tx_last_rc = r;
   g_tx_last_len = n;
   g_tx_last_et = (USHORT)((g[12] << 8) | g[13]);
   if (r == 0) {
      g_tx_why[7]++;
      gSST.trFrames++;
      gSST.trBytes += n;
      if (g[0] & STATION_ADDR_MC)
         gSST.trBCFrames++;
      return NDIS_SUCCESS;
   }
   if (r == -2) {
      g_tx_why[6]++;
      return NDIS_OUT_OF_RESOURCE;
   }
   g_tx_why[5]++;
   gSST.trFrHW++;
   return NDIS_SUCCESS;
}

WORD __cdecl ndis_c_transfer(PUSHORT pBytesCopied, WORD frameOffset, PTDBD pTDBufferD)
{
   (void)frameOffset; (void)pTDBufferD;
   *pBytesCopied = 0;
   return NDIS_INVALID_FUNCTION;                  /* only ReceiveChain indications                           */
}

WORD __cdecl ndis_c_release(WORD reqHandle)
{
   if (reqHandle >= 1 && reqHandle <= RXP_N) {
      g_rxp_busy[reqHandle - 1] = 0;
      return NDIS_SUCCESS;
   }
   return NDIS_INVALID_PARAMETER;
}

/* NDIS 2.0.1: both return with interrupts disabled; the caller re-enables them. */
WORD __cdecl ndis_c_indoff(void)
{
   USHORT own;

   rt_cli();
   rt_atomic_inc(&g_ind_off);
   own = g_ind_owner;
   if (own != 0) {
      if (own == (USHORT)(rt_cpu_id() + 1))
         g_ind_reent++;
      else if (rt_wait_zero(&g_ind_owner, IND_WAIT_ROUNDS))
         g_ind_tmo++;
   }
   return NDIS_SUCCESS;
}

WORD __cdecl ndis_c_indon(void)
{
   rt_atomic_dec_nz(&g_ind_off);
   if (g_ind_off == 0 && g_q_tail != g_q_head)
      hook_arm();
   rt_cli();
   return NDIS_SUCCESS;
}

/* ===== IP interface number (lanN): position of [RTL8821AE_NIF] in the Bindings of [TCPIP_NIF] ===== */
USHORT g_lannum = 1;

#define PL_MAX 600
static char   g_pl[PL_MAX + 1];
static USHORT g_pln;
static UCHAR  g_in_tcpip, g_in_nif;
UCHAR         g_cfgmac;
static char   g_pbuf[256];

static char lc(char c) { return (char)((c >= 'A' && c <= 'Z') ? c + 32 : c); }

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
      g_in_nif = (UCHAR)eqn(l, (USHORT)(e - l), DRV_NIF_LC);
      return;
   }
   if (!g_in_tcpip && !g_in_nif)
      return;
   k = l;
   p = l;
   while (*p && *p != '=') p++;
   if (*p != '=')
      return;
   e = p;
   while (e > k && (e[-1] == ' ' || e[-1] == '\t')) e--;
   if (g_in_nif) {
      static UCHAR m[6];
      USHORT n = 0;
      char   c;

      if (eqn(k, (USHORT)(e - k), "maxwidth")) {      /* MAXWIDTH = 20 | 40 | 80: widest channel to use       */
         USHORT w = 0;

         for (v = p + 1; *v; v++)
            if (*v >= '0' && *v <= '9')
               w = (USHORT)(w * 10 + (*v - '0'));
         g_maxw = (UCHAR)(w >= 80 ? 2 : (w >= 40 ? 1 : 0));
         return;
      }
      if (eqn(k, (USHORT)(e - k), "ampdu")) {         /* AMPDU = 0 | 1: receive aggregation (default 1)       */
         for (v = p + 1; *v && (*v < '0' || *v > '9'); v++) ;
         g_ampdu = (UCHAR)(*v == '0' ? 0 : 1);
         return;
      }
      if (eqn(k, (USHORT)(e - k), "aggtx")) {         /* AGGTX = 0 | 1: transmit aggregation (default 1)       */
         for (v = p + 1; *v && (*v < '0' || *v > '9'); v++) ;
         g_aggtx = (UCHAR)(*v == '0' ? 0 : 1);
         return;
      }
      if (eqn(k, (USHORT)(e - k), "powersave")) {     /* POWERSAVE = 0 | 1: 802.11 power save (default 0)       */
         for (v = p + 1; *v && (*v < '0' || *v > '9'); v++) ;
         g_powersave = (UCHAR)(*v == '1' ? 1 : 0);
         return;
      }
      if (eqn(k, (USHORT)(e - k), "rateadapt")) {     /* RATEADAPT = 0 | 1: host rate adaptation (default 1)    */
         for (v = p + 1; *v && (*v < '0' || *v > '9'); v++) ;
         g_rateadapt = (UCHAR)(*v == '0' ? 0 : 1);
         return;
      }
      if (eqn(k, (USHORT)(e - k), "streams")) {       /* STREAMS = 1 | 2: spatial streams to use (default 2)  */
         for (v = p + 1; *v && (*v < '0' || *v > '9'); v++) ;
         g_maxnss = (UCHAR)(*v == '1' ? 1 : 2);
         return;
      }
      if (eqn(k, (USHORT)(e - k), "channel")) {       /* CHANNEL = n: join only an access point on that primary channel */
         USHORT w = 0;

         for (v = p + 1; *v; v++)
            if (*v >= '0' && *v <= '9')
               w = (USHORT)(w * 10 + (*v - '0'));
         g_forcech = (UCHAR)(w < 255 ? w : 0);
         return;
      }
      if (!eqn(k, (USHORT)(e - k), "netaddress"))
         return;
      for (v = p + 1; *v && n < 12; v++) {
         c = *v;
         if (c >= '0' && c <= '9') c = (char)(c - '0');
         else if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 10);
         else if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 10);
         else continue;
         if (n & 1) m[n >> 1] = (UCHAR)((m[n >> 1] << 4) | c);
         else       m[n >> 1] = (UCHAR)c;
         n++;
      }
      if (n == 12) {
         for (n = 0; n < 6; n++) g_mymac[n] = m[n];
         ndis_set_mac(g_mymac);
         g_cfgmac = 1;
      }
      return;
   }
   if (!g_in_tcpip)
      return;
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
      if (eqn(a, (USHORT)(b - a), DRV_NIF_LC)) {
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
   static USHORT h, action, got;
   USHORT i;
   char   c;

   if (DosOpen((PSZ)path, &h, &action, 0L, 0,
               OPEN_ACTION_OPEN_IF_EXISTS | OPEN_ACTION_FAIL_IF_NEW,
               OPEN_FLAGS_NOINHERIT | OPEN_SHARE_DENYNONE | OPEN_ACCESS_READONLY, 0L) != 0)
      return;
   g_pln = 0;
   g_in_tcpip = 0;
   g_in_nif = 0;
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

/* For the status text */
USHORT ndis_state(USHORT what)
{
   USHORT i, n = 0;

   switch (what) {
   case 0:  return (USHORT)gSST.status;
   case 1:  return gSST.filter;
   case 2:  return (USHORT)gSST.trFrames;
   case 3:  return (USHORT)gSST.trFrHW;
   case 4:  return (USHORT)gSST.recFrames;
   case 5:  return (USHORT)gSST.recFrNoBuff;
   case 6:  for (i = 0; i < RXP_N; i++) if (g_rxp_busy[i] != 2) n++;
            return n;                                  /* receive buffers that exist                             */
   case 7:  return (USHORT)(g_txbuf.phys != 0UL);
   case 8:  return g_ndis_reg;
   case 9:  return g_q_hi;
   case 10: return g_hook_n;
   case 11: return g_ind_drop_busy;
   case 12: return g_ind_drop_off;
   }
   return 0;
}
