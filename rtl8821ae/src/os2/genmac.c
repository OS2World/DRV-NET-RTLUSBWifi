/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * genmac.c - GenMAC-compatible OID device (\DEV\WRND32$), from qc_nic.c of the QCA6174 driver (github.com/psmedley/qca6174),
 * which adapted rtl_nic.c of the OS2World RTL88EU driver (GPL-2.0-only).
 *
 * The OS/2 wpa_supplicant (driver_genmac) and the Wireless LAN Monitor (genmac.dim) talk to a wireless
 * adapter through DosDevIOCtl category 99h: function 40h = OID GET, 41h = OID SET. Parameter packet:
 * ULONG { OID, data length, (out) NDIS status, (out) bytes written, (out) NDIS status again }, the data
 * packet holds the OID data. Private OIDs 0xFFFFFFxx are the wrapper's own (see docs/ARCHITECTURE.md).
 *
 * Everything chip-specific goes through hw.h; the 802.11 station is wlan/sta.c.
 */
#include "drv.h"

#define GENMAC_CATEGORY     0x99
#define GENMAC_OID_GET      0x40
#define GENMAC_OID_SET      0x41
#define GENMAC_VERSION      0x00020000UL    /* >= 2.00 makes genmac.dim offer WPA                          */

#define NDIS_STATUS_SUCCESS            0x00000000UL
#define NDIS_STATUS_NOT_SUPPORTED      0xC00000BBUL
#define NDIS_STATUS_FAILURE            0xC0000001UL
#define NDIS_STATUS_ADAPTER_NOT_READY  0xC0010011UL
#define NDIS_STATUS_BUFFER_TOO_SHORT   0xC0010016UL

#define OID_GEN_CURRENT_PACKET_FILTER   0x0001010EUL
#define OID_802_3_PERMANENT_ADDRESS     0x01010101UL
#define OID_802_3_CURRENT_ADDRESS       0x01010102UL
#define OID_802_3_MULTICAST_LIST        0x01010103UL
#define OID_802_11_BSSID                0x0D010101UL
#define OID_802_11_SSID                 0x0D010102UL
#define OID_802_11_RSSI                 0x0D010206UL
#define OID_GEN_HARDWARE_STATUS         0x00010102UL
#define OID_GEN_MEDIA_SUPPORTED         0x00010103UL
#define OID_GEN_MEDIA_IN_USE            0x00010104UL
#define OID_GEN_MAXIMUM_FRAME_SIZE      0x00010106UL
#define OID_GEN_LINK_SPEED              0x00010107UL
#define OID_GEN_VENDOR_ID               0x0001010CUL
#define OID_GEN_VENDOR_DESCRIPTION      0x0001010DUL
#define OID_GEN_DRIVER_VERSION          0x00010110UL
#define OID_GEN_MEDIA_CONNECT_STATUS    0x00010114UL
#define OID_802_11_NETWORK_TYPE_IN_USE  0x0D010203UL
#define OID_802_11_CONFIGURATION        0x0D010211UL
#define OID_802_11_POWER_MODE           0x0D010216UL
#define OID_802_11_INFRASTRUCTURE_MODE  0x0D010108UL
#define OID_802_11_DISASSOCIATE         0x0D010115UL
#define OID_802_11_AUTHENTICATION_MODE  0x0D010118UL
#define OID_802_11_PRIVACY_FILTER       0x0D010119UL
#define OID_802_11_BSSID_LIST_SCAN      0x0D01011AUL
#define OID_802_11_ENCRYPTION_STATUS    0x0D01011BUL
#define OID_802_11_ADD_KEY              0x0D01011DUL
#define OID_802_11_REMOVE_KEY           0x0D01011EUL
#define OID_802_11_ASSOCIATION_INFORMATION 0x0D01011FUL
#define OID_802_11_PMKID                0x0D010123UL
#define OID_802_11_BSSID_LIST           0x0D010217UL

/* ---- one request at a time (scans block for seconds; callers can be on several threads / CPUs) ---- */
static UCHAR  g_nic_busy;
static USHORT g_nic_awake;
static USHORT g_scanning;
static ULONG  g_scan_end;
#define NIC_EVENT  ((ULONG)(UCHAR FAR *)&g_nic_busy)

void nic_lock(void)
{
   while (rt_tas(&g_nic_busy))
      rt_dh_procblock(NIC_EVENT, 50L, WAIT_UNINTERRUPTIBLE);
}

void nic_unlock(void)
{
   g_nic_busy = 0;
   rt_dh_procrun(NIC_EVENT, &g_nic_awake);
}

static void wr32(UCHAR FAR *p, ULONG v)
{
   U32 u;

   u.l = v;
   p[0] = u.b[0]; p[1] = u.b[1]; p[2] = u.b[2]; p[3] = u.b[3];
}

static ULONG rd32(UCHAR FAR *p)
{
   U32 u;

   u.b[0] = p[0]; u.b[1] = p[1]; u.b[2] = p[2]; u.b[3] = p[3];
   return u.l;
}

static ULONG g_infra = 1, g_authmode, g_encstat;
static UCHAR g_wbssid[6];                      /* BSSID the supplicant asked for (OID_802_11_BSSID)             */
static UCHAR g_wany = 1;                       /* ...or ff:ff:ff:ff:ff:ff = any                                  */
static UCHAR g_pj;                             /* a (re)join is pending, done by a later request                  */
static ULONG g_pj_due;

static ULONG ch_khz(UCHAR c)
{
   ULONG  f;
   USHORT i;

   if (c == 14) return 2484000UL;
   if (c >= 1 && c <= 13) { f = 2407000UL; for (i = 0; i < c; i++) f += 5000UL; return f; }
   f = 5000000UL;
   for (i = 0; i < c; i++) f += 5000UL;
   return f;
}

static ULONG ch_nettype(UCHAR c) { return (c > 14) ? 2UL : 3UL; }   /* Ndis802_11OFDM5 / OFDM24     */

/* NDIS_802_11_BSSID_LIST_EX: NumberOfItems, then per entry a 116-byte fixed part followed by the IEs
 * (including the 12 fixed beacon bytes), padded to 4. */
static USHORT build_bssid_list(UCHAR FAR *out, USHORT room)
{
   USHORT pos = 4, k, i, elen, items = 0, bi;
   UCHAR FAR *e;
   net_t *n;

   for (k = 0; k < g_nnet; k++) {
      n = &g_net[k];
      elen = (USHORT)((116 + n->ielen + 3) & ~3);
      if ((USHORT)(pos + elen) > room || (USHORT)(pos + elen) < pos)
         break;
      e = out + pos;
      rt_setmem(e, 0, elen);
      wr32(e + 0, elen);
      for (i = 0; i < 6; i++)
         e[4 + i] = n->bssid[i];
      wr32(e + 12, n->len);
      for (i = 0; i < n->len; i++)
         e[16 + i] = (UCHAR)n->ssid[i];
      wr32(e + 48, n->sec ? 1UL : 0UL);
      wr32(e + 52, (ULONG)(long)n->rssi);
      wr32(e + 56, ch_nettype(n->ch));
      wr32(e + 60, 32UL);
      bi = (USHORT)(n->ie[8] | (n->ie[9] << 8));
      wr32(e + 64, (ULONG)bi);
      wr32(e + 72, ch_khz(n->ch));
      wr32(e + 76, 16UL);
      wr32(e + 92, 1UL);
      wr32(e + 112, (ULONG)n->ielen);
      for (i = 0; i < n->ielen; i++)
         e[116 + i] = n->ie[i];
      pos = (USHORT)(pos + elen);
      items++;
   }
   wr32(out, (ULONG)items);
   return pos;
}

static ULONG  g_st, g_olen;

/* Log of the last GenMAC requests, shown in the status text: what the Wireless LAN Monitor / the supplicant asks
 * and what the driver answered. g_ioctl_n counts every DosDevIOCtl that reached the strategy routine. */
#define OIDLOG_N 16
ULONG  g_oidlog[OIDLOG_N], g_oidlog_st[OIDLOG_N];
UCHAR  g_oidlog_set[OIDLOG_N];
USHORT g_oidlog_n, g_ioctl_n, g_bad_n, g_bad_cf;
static ULONG  g_seen[32];
static USHORT g_nseen;
static char   g_desc[] = DRV_DESC;
static char   g_title[] = DRV_NIF_FILE;
UCHAR         g_mymac[6];

static void note_unsupp(int is_set, ULONG oid)
{
   USHORT i;

   for (i = 0; i < g_nseen; i++)
      if (g_seen[i] == oid)
         return;
   if (g_nseen < 32)
      g_seen[g_nseen++] = oid;
   tcom('['); tcom('?'); tcom(is_set ? 's' : 'g'); th32(oid); tcom(']');
}

/* The 32-bit OIDs are compared explicitly (the RTL driver found switch() on 32-bit constants unreliable). */
static void oid_request(int is_set, ULONG oid, UCHAR FAR *dat, USHORT dlen)
{
   ULONG  val;
   USHORT i;
   U32    k;

   g_st = NDIS_STATUS_SUCCESS;
   g_olen = 0;
   k.l = oid;

   if (k.w[1] == 0xFFFF) {                       /* private wrapper OIDs                                    */
      switch (k.w[0] & 0x00FF) {
      case 0x00: if (is_set) goto unsupp; val = (ULONG)g_lannum; goto put4;   /* LANNUMBER           */
      case 0x01: if (is_set) goto unsupp; val = 1UL; goto put4;               /* ISWIRELESS          */
      case 0x02: if (is_set) goto unsupp; val = g_joined ? 2UL : 0UL; goto put4;   /* LINKSTATUS     */
      case 0x08:                                                               /* NIF file name       */
         if (is_set) goto unsupp;
         g_olen = 32;
         if (dlen < 32) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem(dat, 0, 32);
         for (i = 0; i < 31 && g_title[i]; i++)
            dat[i] = (UCHAR)g_title[i];
         return;
      case 0x09: if (is_set) goto unsupp; val = GENMAC_VERSION; goto put4;
      case 0x0B: if (!is_set) goto unsupp; return;                            /* 802.1X state        */
      case 0x0C: if (is_set) goto unsupp; val = (ULONG)sta_eapol_count(); goto put4;   /* EAPOL frames waiting */
      case 0x0D: if (is_set) goto unsupp; g_olen = sta_eapol_get(dat, dlen); return;  /* next EAPOL frame    */
      case 0x0E:                                                               /* send an EAPOL frame  */
         if (!is_set) goto unsupp;
         if (sta_eapol_tx(dat, dlen)) g_st = NDIS_STATUS_FAILURE;
         return;
      default:   goto unsupp;
      }
   }

   if (!is_set) {
      if (oid == OID_802_11_INFRASTRUCTURE_MODE) { val = g_infra; goto put4; }
      if (oid == OID_802_11_AUTHENTICATION_MODE) { val = g_authmode; goto put4; }
      if (oid == OID_802_11_ENCRYPTION_STATUS) {
         /* "enabled" reads back as the KeyAbsent value only while the pairwise key is not installed: WEP 0->2,
          * TKIP 4->5, CCMP 6->7 (the supplicant's capability probe depends on it). Once the key is in, the real
          * value (4 / 6) must be returned, or the supplicant waits for the key for ever. */
         val = g_encstat;
         if (val == 0UL) val = 2UL;
         else if ((val == 4UL || val == 6UL) && !(g_joined && g_ptk_on)) val += 1UL;
         goto put4;
      }
      if (oid == OID_802_3_CURRENT_ADDRESS || oid == OID_802_3_PERMANENT_ADDRESS) {
         g_olen = 6;
         if (dlen < 6) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         for (i = 0; i < 6; i++)
            dat[i] = g_mymac[i];
         return;
      }
      if (oid == OID_802_11_BSSID_LIST) {
         if (!g_hw_ready) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         if (dlen < 8) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; g_olen = 8; return; }
         g_olen = build_bssid_list(dat, dlen);
         return;
      }
      if (oid == OID_802_11_RSSI) {
         if (!g_joined) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         val = (ULONG)(long)sta_rssi();
         goto put4;
      }
      if (oid == OID_802_11_BSSID) {
         if (!g_joined) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         g_olen = 6;
         if (dlen < 6) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         for (i = 0; i < 6; i++)
            dat[i] = g_jbssid[i];
         return;
      }
      if (oid == OID_802_11_SSID) {                    /* ULONG length + 32 bytes; empty when not joined       */
         g_olen = 36;
         if (dlen < 36) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem(dat, 0, 36);
         if (g_joined) {
            wr32(dat, (ULONG)g_jslen);
            for (i = 0; i < g_jslen; i++)
               dat[4 + i] = g_jssid[i];
         }
         return;
      }
      if (oid == OID_802_11_ASSOCIATION_INFORMATION) { /* NDIS_802_11_ASSOCIATION_INFORMATION + request/response IEs */
         USHORT rq = g_joined ? g_jreqlen : 0;
         USHORT rs = g_joined ? g_jreslen : 0;
         USHORT tot = (USHORT)(40 + rq + rs);

         g_olen = tot;
         if (dlen < tot) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem(dat, 0, tot);
         wr32(dat + 0, (ULONG)tot);                    /* Length                                              */
         wr32(dat + 16, (ULONG)rq);                    /* RequestIELength                                     */
         wr32(dat + 20, 40UL);                         /* OffsetRequestIEs                                    */
         wr32(dat + 32, (ULONG)rs);                    /* ResponseIELength                                    */
         wr32(dat + 36, (ULONG)(40 + rq));             /* OffsetResponseIEs                                   */
         if (g_joined) {
            dat[4] = 7;                                /* AvailableRequestFixedIEs: cap | listen | AP         */
            dat[6] = 0x31; dat[7] = 0x04;              /* Capabilities                                        */
            dat[8] = 10;                               /* ListenInterval                                      */
            for (i = 0; i < 6; i++)
               dat[10 + i] = g_jbssid[i];
            dat[24] = 7;                               /* AvailableResponseFixedIEs: cap | status | aid       */
            dat[26] = (UCHAR)(g_jcaps & 0xFF); dat[27] = (UCHAR)(g_jcaps >> 8);
            dat[30] = (UCHAR)(g_jaid & 0xFF);  dat[31] = (UCHAR)(g_jaid >> 8);
            for (i = 0; i < rq; i++)
               dat[40 + i] = g_jreq[i];
            for (i = 0; i < rs; i++)
               dat[40 + rq + i] = g_jres[i];
         }
         return;
      }
      if (oid == OID_802_11_POWER_MODE)        { val = 0UL; goto put4; }
      if (oid == OID_GEN_HARDWARE_STATUS)      { val = g_hw_ready ? 0UL : 1UL; goto put4; }   /* Ready / Initializing */
      if (oid == OID_GEN_MEDIA_SUPPORTED || oid == OID_GEN_MEDIA_IN_USE) { val = 0UL; goto put4; }
      if (oid == OID_GEN_MAXIMUM_FRAME_SIZE)   { val = 1500UL; goto put4; }
      if (oid == OID_GEN_LINK_SPEED) {                 /* the best HT rate with short GI: 72.2 / 150 Mbit/s per stream */
         val = g_jht ? (g_j80 ? 4333000UL : (g_j40 ? 1500000UL : 722000UL)) : 540000UL;
         if (g_jht && g_jnss >= 2)
            val += val;
         goto put4;
      }   /* 100 bit/s units */
      if (oid == OID_GEN_MEDIA_CONNECT_STATUS) { val = g_joined ? 0UL : 1UL; goto put4; }
      if (oid == OID_802_11_NETWORK_TYPE_IN_USE) { val = ch_nettype(g_jch); goto put4; }
      if (oid == OID_GEN_VENDOR_ID) {
         g_olen = 4;
         if (dlen < 4) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         dat[0] = hw_vendor_oui(0); dat[1] = hw_vendor_oui(1); dat[2] = hw_vendor_oui(2); dat[3] = 0x00;
         return;
      }
      if (oid == OID_GEN_DRIVER_VERSION) {
         g_olen = 2;
         if (dlen < 2) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         dat[0] = 0x01; dat[1] = 0x00;
         return;
      }
      if (oid == OID_GEN_VENDOR_DESCRIPTION) {
         g_olen = sizeof(g_desc);
         if (dlen < sizeof(g_desc)) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         for (i = 0; i < sizeof(g_desc); i++)
            dat[i] = (UCHAR)g_desc[i];
         return;
      }
      if (oid == OID_802_11_CONFIGURATION) {
         g_olen = 32;
         if (dlen < 32) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem(dat, 0, 32);
         wr32(dat + 0, 32UL);
         wr32(dat + 4, 100UL);
         wr32(dat + 12, ch_khz(g_jch ? g_jch : 1));
         wr32(dat + 16, 16UL);
         return;
      }
      note_unsupp(0, oid);
      g_st = NDIS_STATUS_NOT_SUPPORTED;
      return;
   }

   /* SET */
   if (oid == OID_802_11_BSSID_LIST_SCAN) {
      if (!g_hw_ready) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
      /* joined: stay on the AP's channel; a scan less than 4 s old is still good */
      if (!g_joined && !(g_nnet && g_scan_end && (long)(dd_ms() - g_scan_end) < 4000L)) {
         g_scanning = 1;
         sta_age();
         if (hw_scan() != 0)
            tstr("[scan failed]");
         g_scan_end = dd_ms();
         g_scanning = 0;
         g_st = NDIS_STATUS_SUCCESS;
         g_olen = 0;
      }
      return;
   }
   if (oid == OID_802_11_INFRASTRUCTURE_MODE) { if (dlen >= 4) g_infra = rd32(dat); return; }
   if (oid == OID_802_11_AUTHENTICATION_MODE) { if (dlen >= 4) g_authmode = rd32(dat); return; }
   if (oid == OID_802_11_ENCRYPTION_STATUS)   { if (dlen >= 4) g_encstat = rd32(dat); return; }
   if (oid == OID_802_11_BSSID) {                      /* remember the AP the supplicant chose                  */
      if (dlen >= 6) {
         g_wany = 1;
         for (i = 0; i < 6; i++) {
            g_wbssid[i] = dat[i];
            if (dat[i] != 0xFF) g_wany = 0;
         }
      }
      return;
   }
   if (oid == OID_802_11_POWER_MODE || oid == OID_802_11_PRIVACY_FILTER ||
       oid == OID_802_11_PMKID || oid == OID_GEN_CURRENT_PACKET_FILTER || oid == OID_802_3_MULTICAST_LIST)
      return;                                          /* accepted; nothing to do                               */
   if (oid == OID_802_11_DISASSOCIATE) {
      g_pj = 0;
      sta_leave();
      return;
   }
   if (oid == OID_802_11_SSID) {                       /* join (or leave when the SSID is empty)                */
      ULONG sl = (dlen >= 4) ? rd32(dat) : 0UL;
      USHORT open;

      if (sl == 0UL) { g_pj = 0; sta_leave(); return; }
      if (dlen < 36 || sl > 32UL) { sta_leave(); g_st = NDIS_STATUS_FAILURE; return; }
      for (i = 0; i < (USHORT)sl; i++)
         if (dat[4 + i] < 0x20)
            break;
      if (i < (USHORT)sl) {                            /* control bytes: the supplicant's random "disconnect" SSID */
         g_pj = 0;
         sta_leave();
         return;
      }
      /* an unencrypted network (Open or AutoSwitch with encryption disabled), else WPA2-PSK with CCMP */
      open = (USHORT)((g_authmode == 0UL || g_authmode == 2UL) && g_encstat == 1UL);
      if (!open && (g_authmode != 7UL || g_encstat != 6UL)) {
         g_jfail = 6;                                  /* wrong security mode set by the caller                 */
         g_st = NDIS_STATUS_FAILURE;
         return;
      }
      sta_set_want(dat + 4, (UCHAR)sl, open);
      if (g_joined) {
         /* The supplicant notices a (re)association only when the BSSID it polls CHANGES, and it is a single
          * process: it cannot poll while it sits inside this request. So return at once, stay disconnected for
          * a moment and do the join from a later request (same as the RTL driver). */
         sta_leave();
         g_pj_due = dd_ms() + 1500UL;
         g_pj = 1;
         return;
      }
      g_pj = 0;
      if (sta_join(g_wany ? (UCHAR FAR *)0 : (UCHAR FAR *)g_wbssid, g_wany))
         g_st = NDIS_STATUS_FAILURE;
      return;
   }
   if (oid == OID_802_11_ADD_KEY) {                    /* NDIS_802_11_KEY: len@0 index@4 keylen@8 bssid@12 rsc@24 key@32 */
      ULONG ki, kl;

      if (dlen < 48) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
      ki = rd32(dat + 4);
      kl = rd32(dat + 8);
      if (kl != 16UL) { g_st = NDIS_STATUS_NOT_SUPPORTED; return; }       /* CCMP only                          */
      if (sta_key_add(dat + 32, 16, (USHORT)((ki & 0x40000000UL) ? 1 : 0), (UCHAR)(ki & 0xFFUL)))
         g_st = NDIS_STATUS_FAILURE;
      return;
   }
   if (oid == OID_802_11_REMOVE_KEY) {                 /* NDIS_802_11_REMOVE_KEY: len@0 index@4 bssid@8          */
      ULONG ki;

      if (dlen < 8) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
      ki = rd32(dat + 4);
      sta_key_remove((USHORT)((ki & 0x40000000UL) ? 1 : 0), (UCHAR)(ki & 0xFFUL));
      return;
   }
   note_unsupp(1, oid);
   g_st = NDIS_STATUS_NOT_SUPPORTED;
   return;

unsupp:
   note_unsupp(is_set, oid);
   g_st = NDIS_STATUS_NOT_SUPPORTED;
   return;

put4:
   g_olen = 4;
   if (dlen < 4) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
   wr32(dat, val);
}

/* CMDGenIOCTL: verify the caller's buffers (bounded), bring the chip up on the first request, run the OID,
 * write the NDIS status and byte count back. */
void dd_genmac_ioctl(RP_GENIOCTL FAR *rp)
{
   ULONG FAR *par;
   UCHAR FAR *dat;
   ULONG  oid, room;
   USHORT dlen, off;
   int    fast;

   g_ioctl_n++;
   if (rp->Category != GENMAC_CATEGORY ||
       (rp->Function != GENMAC_OID_GET && rp->Function != GENMAC_OID_SET)) {
      g_bad_n++;
      g_bad_cf = (USHORT)((rp->Category << 8) | rp->Function);   /* an IOCtl that is not a GenMAC OID request    */
      rp->rph.Status |= STDON | STERR | ERROR_I24_BAD_COMMAND;
      return;
   }
   par = (ULONG FAR *)rp->ParmPacket;
   if (par == 0 || rp->ParmLen < 12 ||
       rt_dh_verify(SELECTOROF(rp->ParmPacket), 12, OFFSETOF(rp->ParmPacket), VERIFY_READWRITE)) {
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }
   dat  = (UCHAR FAR *)rp->DataPacket;
   dlen = rp->DataLen;
   if (dat == 0)
      dlen = 0;
   else if (par[1] > (ULONG)dlen && par[1] <= 9000UL)
      dlen = (USHORT)par[1];
   if (dlen) {
      off  = OFFSETOF(rp->DataPacket);
      room = 0x10000UL - (ULONG)off;
      if ((ULONG)dlen > room) dlen = (USHORT)room;
      if (dlen > 9000) dlen = 9000;
      if (rt_dh_verify(SELECTOROF(rp->DataPacket), dlen, off, VERIFY_READWRITE)) {
         rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
         return;
      }
   }
   oid = par[0];
   /* While a scan holds the lock, plain state queries are answered at once from cached values. */
   fast = (g_scanning && g_hw_ready && rp->Function == GENMAC_OID_GET && oid != OID_802_11_BSSID_LIST);
   if (!fast) {
      nic_lock();
      if (!g_hw_ready && !g_hw_failed && g_fwblob.done)
         hw_up();                                 /* the chip starts on the first GenMAC request             */
      if (g_hw_ready) {
         hw_service();
         sta_poll();                              /* the AP is gone: clean up                                 */
         if (g_pj && !g_scanning && (long)(dd_ms() - g_pj_due) >= 0L) {
            g_pj = 0;                             /* the deferred (re)join, now that the drop has been visible */
            if (!g_joined)
               sta_join(g_wany ? (UCHAR FAR *)0 : (UCHAR FAR *)g_wbssid, g_wany);
         }
      }
   }
   oid_request(rp->Function == GENMAC_OID_SET, oid, dat, dlen);
   {
      USHORT k = (USHORT)(g_oidlog_n % OIDLOG_N);

      g_oidlog[k] = oid;
      g_oidlog_st[k] = g_st;
      g_oidlog_set[k] = (UCHAR)(rp->Function == GENMAC_OID_SET);
      g_oidlog_n++;
   }
   if (!fast)
      nic_unlock();
   par[1] = g_st;
   par[2] = g_olen;
   if (rp->ParmLen >= 16 &&
       !rt_dh_verify(SELECTOROF(rp->ParmPacket), 16, OFFSETOF(rp->ParmPacket), VERIFY_READWRITE))
      par[3] = g_st;                              /* genmac.dim looks for the NDIS status here               */
   rp->rph.Status |= STDON;
}

void dd_shutdown(void)
{
   if (g_hw_ready) {
      nic_lock();
      hw_down();
      nic_unlock();
   }
   irq_unhook();
}
