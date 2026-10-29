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
 * rtl_nic.c - GenMAC-compatible adapter facade (step 1: adapter identity + scan list).
 *
 * The OS/2 wpa_supplicant (and the XWLAN / Wireless LAN Monitor on top of it)
 * talks to a wireless adapter through a "GenMAC wrapper" character device:
 *   DosDevIOCtl category 0x99, function 0x40 = NDIS OID GET, 0x41 = OID SET,
 *   parameter packet = ULONG[10] { OID, length, (out) NDIS status, (out) bytes },
 *   data packet      = the OID's data.
 * See References\wpa_supplicant-os2\src\drivers\{packet32.c,driver_genmac.c}.
 *
 * This file implements that interface on top of the RTL8188EU driver. Step 1
 * answers the identity OIDs and the scan OIDs (BSSID_LIST_SCAN / BSSID_LIST as
 * an NDIS_802_11_BSSID_LIST_EX built from the beacon table); join / keys / data
 * come in later steps and are accepted but not acted on yet.
 *
 * MS C 6.0 / 16-bit: no // comments, no 32-bit variable shifts or multiplies.
 */
#include "rt_usb.h"


#define GENMAC_CATEGORY     0x99
#define GENMAC_OID_GET      0x40
#define GENMAC_OID_SET      0x41
#define GENMAC_VERSION      0x00020000UL   /* >= 2.00 makes genmac.dim/xwlan offer WPA (supplicant needs >= 1.08) */


/* NDIS status codes returned in Paras[1] */
#define NDIS_STATUS_SUCCESS            0x00000000UL
#define NDIS_STATUS_NOT_SUPPORTED      0xC00000BBUL
#define NDIS_STATUS_FAILURE            0xC0000001UL
#define NDIS_STATUS_ADAPTER_NOT_READY  0xC0010011UL
#define NDIS_STATUS_BUFFER_TOO_SHORT   0xC0010016UL

/* private (wrapper) OIDs */
#define OID_PRIV_LANNUMBER     0xFFFFFF00UL
#define OID_PRIV_ISWIRELESS    0xFFFFFF01UL
#define OID_PRIV_GENMAC_VER    0xFFFFFF09UL
#define OID_PRIV_8021X_STATE   0xFFFFFF0BUL   /* SET 1 = start passing EAPOL frames            */
#define OID_PRIV_8021X_READY   0xFFFFFF0CUL   /* GET = number of received EAPOL frames waiting */
#define OID_PRIV_8021X_RX      0xFFFFFF0DUL   /* GET = next received EAPOL (Ethernet) frame    */
#define OID_PRIV_8021X_TX      0xFFFFFF0EUL   /* SET = send this Ethernet frame (EAPOL)        */

/* standard OIDs */
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

/* ---- one request at a time (scans block; callers can be on several threads) ---- */
static UCHAR  g_nic_busy;               /* taken with an atomic exchange: two threads can run the strategy routine at once on SMP */
static USHORT g_nic_awake;
static UCHAR  g_pj;                /* a (re)join is pending: runs from a later request, once the supplicant has seen the drop */
static ULONG  g_pj_due;            /* ...not before this time (ms since boot) */
static USHORT g_scanning;          /* a scan is sweeping the channels (holds g_nic_busy for ~2.5 s) */
#define NIC_EVENT  ((ULONG)(UCHAR FAR *)&g_nic_busy)

static void nic_lock(void)
{
   while (rt_tas(&g_nic_busy))
      rt_dh_procblock(NIC_EVENT, 50L, 0);              /* short: a ProcRun between the failed attempt and the block is lost */
}

static void nic_unlock(void)
{
   g_nic_busy = 0;
   rt_dh_procrun(NIC_EVENT, &g_nic_awake);
}

void rtl8188eu_nic_lock(void)   { nic_lock(); }     /* for the debug channel: same lock as the OID calls */
void rtl8188eu_nic_unlock(void) { nic_unlock(); }

/* ---- little-endian stores into the (far) user buffer ---------------------- */
static void wr32(UCHAR FAR *p, ULONG v)
{
   union { ULONG l; UCHAR b[4]; } u;

   u.l = v;
   p[0] = u.b[0];
   p[1] = u.b[1];
   p[2] = u.b[2];
   p[3] = u.b[3];
}

/* ---- state the supplicant sets and reads back ----------------------------- */
static ULONG g_infra = 1;        /* Ndis802_11Infrastructure                    */
static ULONG g_authmode;         /* NDIS_802_11_AUTHENTICATION_MODE             */
static ULONG g_encstat;          /* NDIS_802_11_ENCRYPTION_STATUS               */
static UCHAR g_kbuf[16];         /* key material copied out of the caller's buffer */
static UCHAR g_wbssid[6];        /* BSSID the supplicant asked for (OID_802_11_BSSID) */
static UCHAR g_wany = 1;         /* ...or ff:ff:ff:ff:ff:ff = any                */

/* centre frequency in kHz for DSConfig, indexed by channel 1..14 */
static ULONG g_chkhz[15] = {
   0UL,       2412000UL, 2417000UL, 2422000UL, 2427000UL, 2432000UL, 2437000UL, 2442000UL,
   2447000UL, 2452000UL, 2457000UL, 2462000UL, 2467000UL, 2472000UL, 2484000UL
};

/*
 * Build an NDIS_802_11_BSSID_LIST_EX from the scan table into 'out' (room bytes).
 * Layout per entry (116-byte fixed part, then the IEs incl. the 12 fixed beacon
 * bytes, padded to 4):  Length(4) Mac(6) Rsvd(2) SsidLen(4) Ssid(32) Privacy(4)
 * Rssi(4) NetType(4) Config{Length(4) Beacon(4) ATIM(4) DSConfig(4) FH(16)}
 * Infra(4) Rates(16) IELength(4) IEs.   Returns bytes written.
 */
static USHORT build_bssid_list(UCHAR FAR *out, USHORT room)
{
   USHORT pos = 4, k, i, elen, items = 0, bp;
   UCHAR FAR *e;
   net_t *n;
   UCHAR  c;

   for (k = 0; k < g_nnet; k++) {
      n = &g_net[k];
      elen = (USHORT)((116 + n->ielen + 3) & ~3);
      if ((USHORT)(pos + elen) > room || (USHORT)(pos + elen) < pos)
         break;
      e = out + pos;
      rt_setmem((PSZ)e, 0, elen);
      wr32(e + 0, elen);
      for (i = 0; i < 6; i++)
         e[4 + i] = n->bssid[i];
      wr32(e + 12, n->len);
      for (i = 0; i < n->len; i++)
         e[16 + i] = (UCHAR)n->ssid[i];
      wr32(e + 48, n->sec ? 1UL : 0UL);                   /* Privacy                */
      wr32(e + 52, (ULONG)(long)(n->rssi ? n->rssi : -80));   /* Rssi (dBm, smoothed from PHY status) */
      wr32(e + 56, 3UL);                                  /* Ndis802_11OFDM24       */
      wr32(e + 60, 32UL);                                 /* Configuration.Length   */
      bp = (USHORT)(n->ie[8] | (n->ie[9] << 8));          /* beacon interval        */
      wr32(e + 64, (ULONG)bp);
      c = n->ch;
      if (c < 1 || c > 14)
         c = 1;
      wr32(e + 72, g_chkhz[c]);                           /* DSConfig (kHz)         */
      wr32(e + 76, 16UL);                                 /* FHConfig.Length        */
      wr32(e + 92, 1UL);                                  /* Infrastructure         */
      wr32(e + 112, (ULONG)n->ielen);                     /* IELength               */
      for (i = 0; i < n->ielen; i++)
         e[116 + i] = n->ie[i];
      pos = (USHORT)(pos + elen);
      items++;
   }
   wr32(out, (ULONG)items);
   return pos;
}

/* ---- the OID dispatcher ---------------------------------------------------- */
/* Results go in statics (DS), never through pointers to stack locals: with SS != DS
 * a near pointer to a stack variable would be read DS-relative. */
static ULONG g_st, g_olen;
static USHORT g_trace;

static void th8(UCHAR b)
{
   static char hx[] = "0123456789ABCDEF";
   tcom(hx[(b >> 4) & 0x0F]);
   tcom(hx[b & 0x0F]);
}

static void th32(ULONG v)
{
   union { ULONG l; UCHAR b[4]; } u;
   u.l = v;
   th8(u.b[3]); th8(u.b[2]); th8(u.b[1]); th8(u.b[0]);
}

/* MS C 6.0 mis-dispatches switch() on 32-bit constants (cases silently fell to
 * default); map each OID to a small id with explicit compares and switch on that. */
enum { ID_NONE = 0, ID_PKT_FILTER, ID_PERM_ADDR, ID_CUR_ADDR, ID_MCAST, ID_BSSID,
       ID_SSID, ID_INFRA, ID_DISASSOC, ID_AUTH, ID_PRIVFILT, ID_SCAN, ID_ENC,
       ID_ADDKEY, ID_REMKEY, ID_ASSOCINFO, ID_PMKID, ID_LIST, ID_RSSI,
       ID_G_HWSTAT, ID_G_MEDIUM, ID_G_MAXFRAME, ID_G_SPEED, ID_G_VENDID, ID_G_DESC, ID_G_DRVVER,
       ID_G_MEDIACONN, ID_NETTYPE, ID_CONFIG, ID_POWER };

static USHORT oid_id(ULONG oid)
{
   if (oid == OID_GEN_CURRENT_PACKET_FILTER)     return ID_PKT_FILTER;
   if (oid == OID_802_3_PERMANENT_ADDRESS)       return ID_PERM_ADDR;
   if (oid == OID_802_3_CURRENT_ADDRESS)         return ID_CUR_ADDR;
   if (oid == OID_802_3_MULTICAST_LIST)          return ID_MCAST;
   if (oid == OID_802_11_BSSID)                  return ID_BSSID;
   if (oid == OID_802_11_SSID)                   return ID_SSID;
   if (oid == OID_802_11_INFRASTRUCTURE_MODE)    return ID_INFRA;
   if (oid == OID_802_11_DISASSOCIATE)           return ID_DISASSOC;
   if (oid == OID_802_11_AUTHENTICATION_MODE)    return ID_AUTH;
   if (oid == OID_802_11_PRIVACY_FILTER)         return ID_PRIVFILT;
   if (oid == OID_802_11_BSSID_LIST_SCAN)        return ID_SCAN;
   if (oid == OID_802_11_ENCRYPTION_STATUS)      return ID_ENC;
   if (oid == OID_802_11_ADD_KEY)                return ID_ADDKEY;
   if (oid == OID_802_11_REMOVE_KEY)             return ID_REMKEY;
   if (oid == OID_802_11_ASSOCIATION_INFORMATION) return ID_ASSOCINFO;
   if (oid == OID_802_11_PMKID)                  return ID_PMKID;
   if (oid == OID_802_11_BSSID_LIST)             return ID_LIST;
   if (oid == OID_802_11_RSSI)                   return ID_RSSI;
   if (oid == OID_GEN_HARDWARE_STATUS)           return ID_G_HWSTAT;
   if (oid == OID_GEN_MEDIA_SUPPORTED || oid == OID_GEN_MEDIA_IN_USE) return ID_G_MEDIUM;
   if (oid == OID_GEN_MAXIMUM_FRAME_SIZE)        return ID_G_MAXFRAME;
   if (oid == OID_GEN_LINK_SPEED)                return ID_G_SPEED;
   if (oid == OID_GEN_VENDOR_ID)                 return ID_G_VENDID;
   if (oid == OID_GEN_VENDOR_DESCRIPTION)        return ID_G_DESC;
   if (oid == OID_GEN_DRIVER_VERSION)            return ID_G_DRVVER;
   if (oid == OID_GEN_MEDIA_CONNECT_STATUS)      return ID_G_MEDIACONN;
   if (oid == OID_802_11_NETWORK_TYPE_IN_USE)    return ID_NETTYPE;
   if (oid == OID_802_11_CONFIGURATION)          return ID_CONFIG;
   if (oid == OID_802_11_POWER_MODE)             return ID_POWER;
   return ID_NONE;
}

static ULONG  g_seen[48];
static USHORT g_nseen;
static char   g_desc[] = "Realtek RTL8188EU USB WLAN (OS/2 native)";
static char   g_title[] = "RTL88EU.NIF";   /* NIF file name in MACS\: genmac.dim reads its Title key */
/* link speed in units of 100 bit/s, indexed by the hardware rate code (0=1M 1=2M 2=5.5M 3=11M 4=6M ... B=54M) */
static ULONG  g_spd[12] = { 10000UL, 20000UL, 55000UL, 110000UL, 60000UL, 90000UL, 120000UL, 180000UL,
                            240000UL, 360000UL, 480000UL, 540000UL };

/* Log each request we cannot answer once (GET/SET + OID) so clients' needs show up on COM1. */
static void note_unsupp(int is_set, ULONG oid)
{
   USHORT i;

   for (i = 0; i < g_nseen; i++)
      if (g_seen[i] == oid)
         return;
   if (g_nseen < 48)
      g_seen[g_nseen++] = oid;
   tcom('['); tcom('?'); tcom(is_set ? 's' : 'g'); th32(oid); tcom(']');
}

static void oid_request(int is_set, ULONG oid, UCHAR FAR *dat, USHORT dlen)
{
   ULONG val;
   USHORT i;

   g_st   = NDIS_STATUS_SUCCESS;
   g_olen = 0;

   /* Private OIDs 0xFFFFFFxx: dispatched on the low word. MS C 6.0 mis-compiled the
    * 32-bit switch (0xFFFFFF0C fell to default), so test the halves explicitly. */
   {
      union { ULONG l; USHORT w[2]; } k;
      k.l = oid;
      if (k.w[1] == 0xFFFF) {
         switch (k.w[0] & 0x00FF) {
         case 0x00:                               /* LANNUMBER      */
            if (is_set) goto unsupp;
            val = (ULONG)g_lannum;                /* position of our binding in PROTOCOL.INI */
            goto put4;
         case 0x01:                               /* ISWIRELESS     */
            if (is_set) goto unsupp;
            val = 1UL;                              /* always: the monitor asks once at start-up, before the dongle may be attached */
            goto put4;
         case 0x02:                               /* LINKSTATUS: bit 1 = connected (genmac.dim) */
            if (is_set) goto unsupp;
            val = g_joined ? 2UL : 0UL;
            goto put4;
         case 0x08:                               /* WINDRIVER_NIFNAME: 32-byte ASCIIZ NIF file name; genmac.dim reads Title from it */
            if (is_set) goto unsupp;
            g_olen = 32;
            if (dlen < 32) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
            rt_setmem((PSZ)dat, 0, 32);
            for (i = 0; i < 31 && g_title[i]; i++)
               dat[i] = (UCHAR)g_title[i];
            return;
         case 0x09:                               /* GENMAC_VERSION */
            if (is_set) goto unsupp;
            val = GENMAC_VERSION;
            goto put4;
         case 0x0B:                               /* 802.1X STATE (SET) */
            if (!is_set) goto unsupp;
            return;
         case 0x0E:                               /* 802.1X TX (SET): Ethernet EAPOL frame */
            if (!is_set) goto unsupp;
            rtl8188eu_eapol_tx(&g_usbdev, dat, dlen);
            return;
         case 0x0C:                               /* 802.1X READY: number of EAPOL frames waiting */
            if (is_set) goto unsupp;
            val = (ULONG)rtl8188eu_poll(&g_usbdev, 20);
            goto put4;
         case 0x0D:                               /* 802.1X RX: next received EAPOL frame */
            if (is_set) goto unsupp;
            g_olen = rtl8188eu_eapol_get(dat, dlen);
            return;
         default:
            goto unsupp;
         }
      }
   }

   if (!is_set) {
      switch (oid_id(oid)) {
      case ID_INFRA:
         val = g_infra;
         goto put4;
      case ID_AUTH:
         val = g_authmode;
         goto put4;
      case ID_ENC:
         /* "enabled" modes read back as their KeyAbsent value until a key is
          * installed (the supplicant's capability probe depends on it): WEP 0->2,
          * TKIP 4->5, CCMP 6->7. */
         val = g_encstat;
         if (val == 0UL || ((val == 4UL || val == 6UL) && !rtl8188eu_has_ptk()))
            val = (val == 0UL) ? 2UL : val + 1UL;
         goto put4;

      case ID_CUR_ADDR:
      case ID_PERM_ADDR:
         g_olen = 6;
         if (dlen < 6) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         for (i = 0; i < 6; i++)
            dat[i] = g_mymac[i];
         return;

      case ID_LIST:
         if (!g_hw_ready) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         if (dlen < 8) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; g_olen = 8; return; }
         g_olen = build_bssid_list(dat, dlen);
         return;

      case ID_RSSI:                       /* dBm of the associated access point */
         if (!g_joined) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         val = (ULONG)(long)(rtl8188eu_rssi_of(g_jbssid) ? rtl8188eu_rssi_of(g_jbssid) : -80);
         goto put4;

      case ID_BSSID:
         if (!g_joined) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         g_olen = 6;
         if (dlen < 6) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         for (i = 0; i < 6; i++)
            dat[i] = g_jbssid[i];
         return;

      case ID_SSID:                       /* ULONG length + 32 bytes; empty when not joined */
         g_olen = 36;
         if (dlen < 36) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem((PSZ)dat, 0, 36);
         if (g_joined) {
            wr32(dat, (ULONG)g_jslen);
            for (i = 0; i < g_jslen; i++)
               dat[4 + i] = g_jssid[i];
         }
         return;

      case ID_ASSOCINFO:    /* NDIS_802_11_ASSOCIATION_INFORMATION + request/response IEs */
      {
         USHORT rq = g_joined ? g_jreqlen : 0;
         USHORT rs = g_joined ? g_jreslen : 0;
         USHORT tot = (USHORT)(40 + rq + rs);

         g_olen = tot;
         if (dlen < tot) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem((PSZ)dat, 0, tot);
         wr32(dat + 0, (ULONG)tot);               /* Length                       */
         wr32(dat + 16, (ULONG)rq);               /* RequestIELength              */
         wr32(dat + 20, 40UL);                    /* OffsetRequestIEs             */
         wr32(dat + 32, (ULONG)rs);               /* ResponseIELength             */
         wr32(dat + 36, (ULONG)(40 + rq));        /* OffsetResponseIEs            */
         if (g_joined) {
            dat[4] = 7;                           /* AvailableRequestFixedIEs: cap|listen|AP */
            dat[6] = 0x31; dat[7] = 0x16;         /* Capabilities                 */
            dat[8] = 2;                           /* ListenInterval               */
            for (i = 0; i < 6; i++)
               dat[10 + i] = g_jbssid[i];
            dat[24] = 7;                          /* AvailableResponseFixedIEs: cap|status|aid */
            dat[26] = (UCHAR)(g_jrescap & 0xFF); dat[27] = (UCHAR)(g_jrescap >> 8);
            dat[28] = (UCHAR)(g_jstatus & 0xFF); dat[29] = (UCHAR)(g_jstatus >> 8);
            dat[30] = (UCHAR)(g_jaidv & 0xFF);   dat[31] = (UCHAR)(g_jaidv >> 8);
            for (i = 0; i < rq; i++)
               dat[40 + i] = g_jreq[i];
            for (i = 0; i < rs; i++)
               dat[40 + rq + i] = g_jres[i];
         }
         return;
      }

      case ID_POWER:      val = 0UL; goto put4;                  /* Ndis802_11PowerModeCAM: always awake */
      case ID_G_HWSTAT:   val = 0UL; goto put4;                  /* NdisHardwareStatusReady */
      case ID_G_MEDIUM:   val = 0UL; goto put4;                  /* NdisMedium802_3: we present Ethernet */
      case ID_G_MAXFRAME: val = 1500UL; goto put4;
      case ID_G_SPEED:    val = g_spd[rtl8188eu_cur_rate() % 12]; goto put4;
      case ID_G_MEDIACONN: val = g_joined ? 0UL : 1UL; goto put4;   /* 0 = connected */
      case ID_NETTYPE:    val = 3UL; goto put4;                  /* Ndis802_11OFDM24 */
      case ID_G_VENDID:                                          /* Realtek OUI 00:E0:4C */
         g_olen = 4;
         if (dlen < 4) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         dat[0] = 0x00; dat[1] = 0xE0; dat[2] = 0x4C; dat[3] = 0x00;
         return;
      case ID_G_DRVVER:                                          /* major 0, minor 1 */
         g_olen = 2;
         if (dlen < 2) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         dat[0] = 0x01; dat[1] = 0x00;
         return;
      case ID_G_DESC:
         g_olen = sizeof(g_desc);
         if (dlen < sizeof(g_desc)) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         for (i = 0; i < sizeof(g_desc); i++)
            dat[i] = (UCHAR)g_desc[i];
         return;
      case ID_CONFIG:                                            /* NDIS_802_11_CONFIGURATION */
         g_olen = 32;
         if (dlen < 32) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         rt_setmem((PSZ)dat, 0, 32);
         wr32(dat + 0, 32UL);
         wr32(dat + 4, 100UL);                                   /* beacon period (TU) */
         wr32(dat + 12, g_chkhz[(g_jch >= 1 && g_jch <= 14) ? g_jch : 1]);   /* DS channel in kHz */
         wr32(dat + 16, 16UL);                                   /* FHConfig.Length */
         return;

      default:
         note_unsupp(0, oid);
         g_st = NDIS_STATUS_NOT_SUPPORTED;
         return;
      }
   } else {
      switch (oid_id(oid)) {
      case ID_SCAN:
         if (!g_hw_ready) { g_st = NDIS_STATUS_ADAPTER_NOT_READY; return; }
         if (!g_joined)                           /* never leave the AP's channel while joined */
         {
            g_scanning = 1;
            rtl8188eu_scan_once(&g_usbdev);
            g_scanning = 0;
            g_st = NDIS_STATUS_SUCCESS;          /* requests answered meanwhile reused the result globals */
            g_olen = 0;
         }
         return;

      case ID_INFRA:
         if (dlen >= 4) g_infra = *(ULONG FAR *)dat;
         return;
      case ID_AUTH:
         if (dlen >= 4) g_authmode = *(ULONG FAR *)dat;
         return;
      case ID_ENC:
         if (dlen >= 4) g_encstat = *(ULONG FAR *)dat;
         return;

      case ID_BSSID:                              /* remember the AP the supplicant chose */
         if (dlen >= 6) {
            g_wany = 1;
            for (i = 0; i < 6; i++) {
               g_wbssid[i] = dat[i];
               if (dat[i] != 0xFF) g_wany = 0;
            }
         }
         return;

      case ID_SSID:                               /* join (or leave when the SSID is empty) */
      {
         ULONG sl = (dlen >= 4) ? *(ULONG FAR *)dat : 0UL;

         if (sl == 0UL) {
            tcom('<'); tcom('e');                   /* trace: leave caused by an empty-SSID set */
            g_pj = 0;
            rtl8188eu_leave(&g_usbdev);
            return;
         }
         if (dlen < 36 || sl > 32UL) {
            rtl8188eu_leave(&g_usbdev);
            g_st = NDIS_STATUS_FAILURE;
            return;
         }
         for (i = 0; i < (USHORT)sl; i++)
            if (dat[4 + i] < 0x20)
               break;
         if (i < (USHORT)sl) {                    /* control bytes: the supplicant's random "disconnect" SSID */
            tcom('<'); tcom('o');                   /* trace: leave caused by the disconnect trick */
            g_pj = 0;
            rtl8188eu_leave(&g_usbdev);
            return;
         }
         if (g_authmode != 7UL || g_encstat != 6UL) {   /* our RSN IE is WPA2-PSK / CCMP */
            rtl8188eu_diag_set(6, (USHORT)g_authmode, (USHORT)g_encstat);   /* reason 6: wrong mode set by the caller */
            g_st = NDIS_STATUS_FAILURE;
            return;
         }
         rtl8188eu_set_want_ssid(dat + 4, (UCHAR)sl);
         if (g_joined) {
            tcom('<'); tcom('r');                   /* trace: rejoin requested while joined */
            rtl8188eu_leave(&g_usbdev);
            /* The supplicant notices a (re)association only when the BSSID it polls (about once a second)
             * CHANGES, and it is a single process: it cannot poll while it sits inside this request. So
             * return at once, stay disconnected for a moment, and do the join from a later request. */
            g_pj_due = rtl8188eu_ms() + 1500UL;
            g_pj = 1;
            return;
         }
         g_pj = 0;
         if (rtl8188eu_join(&g_usbdev, g_wany ? (UCHAR *)0 : g_wbssid) != 0)
            g_st = NDIS_STATUS_FAILURE;
         return;
      }

      case ID_DISASSOC:
         tcom('<'); tcom('d');                      /* trace: leave caused by a DISASSOCIATE request */
         g_pj = 0;
         rtl8188eu_leave(&g_usbdev);
         return;

      case ID_ADDKEY:                             /* NDIS_802_11_KEY: len@0 index@4 keylen@8 bssid@12 rsc@24 key@32 */
      {
         ULONG ki, kl;

         if (dlen < 48) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         ki = *(ULONG FAR *)(dat + 4);
         kl = *(ULONG FAR *)(dat + 8);
         if (kl != 16UL) { g_st = NDIS_STATUS_NOT_SUPPORTED; return; }       /* CCMP only */
         for (i = 0; i < 16; i++)
            g_kbuf[i] = dat[32 + i];
         if (rtl8188eu_key_add(&g_usbdev, (UCHAR)(ki & 0xFFUL),
                               (UCHAR)((ki & 0x40000000UL) ? 1 : 0), g_kbuf) != 0)
            g_st = NDIS_STATUS_FAILURE;
         for (i = 0; i < 16; i++)
            g_kbuf[i] = 0;                        /* the key now lives only in the chip's key table */
         return;
      }

      case ID_REMKEY:                             /* NDIS_802_11_REMOVE_KEY: len@0 index@4 bssid@8 */
      {
         ULONG ki;

         if (dlen < 8) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
         ki = *(ULONG FAR *)(dat + 4);
         if (g_joined)
            rtl8188eu_key_remove(&g_usbdev, (UCHAR)(ki & 0xFFUL),
                                 (UCHAR)((ki & 0x40000000UL) ? 1 : 0));
         return;
      }

      /* accepted but not acted on */
      case ID_POWER:                               /* power mode: we never sleep, accept */
      case ID_PRIVFILT:
      case ID_PMKID:
      case ID_PKT_FILTER:
      case ID_MCAST:
         return;

      default:
         note_unsupp(1, oid);
         g_st = NDIS_STATUS_NOT_SUPPORTED;
         return;
      }
   }

unsupp:
   note_unsupp(is_set, oid);
   g_st = NDIS_STATUS_NOT_SUPPORTED;
   return;

put4:
   g_olen = 4;
   if (dlen < 4) { g_st = NDIS_STATUS_BUFFER_TOO_SHORT; return; }
   wr32(dat, val);
}

/*
 * CMDGenIOCTL entry. The kernel hands us far pointers to the caller's parameter
 * and data buffers; verify access (bounded: we never touch more than ~9 KB), run
 * the OID, and write the NDIS status + byte count back into the parameter packet.
 */
void rtl8188eu_genmac_ioctl(RP_GENIOCTL FAR *rp)
{
   ULONG FAR *par;
   UCHAR FAR *dat;
   ULONG oid, room;
   USHORT dlen, off;
   int fast;

   if (rp->Category != GENMAC_CATEGORY ||
       (rp->Function != GENMAC_OID_GET && rp->Function != GENMAC_OID_SET)) {
      rp->rph.Status |= STDON | STERR | ERROR_I24_BAD_COMMAND;
      return;
   }

   par = (ULONG FAR *)rp->ParmPacket;
   if (par == 0 || rp->ParmLen < 12 ||
       rt_dh_verify(SELECTOROF(rp->ParmPacket), 12, OFFSETOF(rp->ParmPacket), VERIFY_READWRITE)) {
      rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
      return;
   }

   if (!g_hw_ready && !g_hw_failed && g_usbdev.attached)
      rtl8188eu_on_open();                 /* the chip starts on the first GenMAC request */

   dat  = (UCHAR FAR *)rp->DataPacket;
   dlen = rp->DataLen;
   if (dat == 0)
      dlen = 0;
   else if (par[1] > (ULONG)dlen && par[1] <= 9000UL)   /* PacketSend/Receive pass DataLen 4 but the real size in the OID length */
      dlen = (USHORT)par[1];
   if (dlen) {
      off  = OFFSETOF(rp->DataPacket);
      room = 0x10000UL - (ULONG)off;                    /* bytes left in this selector */
      if ((ULONG)dlen > room)  dlen = (USHORT)room;
      if (dlen > 9000)         dlen = 9000;             /* we never need more           */
      if (rt_dh_verify(SELECTOROF(rp->DataPacket), dlen, off, VERIFY_READWRITE)) {
         rp->rph.Status |= STDON | STERR | ERROR_I24_GEN_FAILURE;
         return;
      }
   }

   oid = par[0];
   /* While a scan holds the lock, plain state queries (they only read cached values) are
    * answered at once so the monitor's GUI thread does not stall for the whole sweep. */
   fast = 0;
   if (g_scanning && g_hw_ready && rp->Function == GENMAC_OID_GET && oid != OID_802_11_BSSID_LIST) {
      union { ULONG l; USHORT w[2]; } k;
      k.l = oid;
      fast = 1;
      if (k.w[1] == 0xFFFF && ((k.w[0] & 0x00FF) == 0x0C || (k.w[0] & 0x00FF) == 0x0D))
         fast = 0;
   }
   if (!fast) nic_lock();
   if (!fast && g_hw_ready && !g_scanning)
      rtl8188eu_led_tick(&g_usbdev);
   if (!fast && g_pj && g_hw_ready && !g_scanning && (long)(rtl8188eu_ms() - g_pj_due) >= 0L) {
      g_pj = 0;                                 /* the deferred (re)join, now that the drop has been visible */
      if (!g_joined)
         rtl8188eu_join(&g_usbdev, g_wany ? (UCHAR *)0 : g_wbssid);
   }
   oid_request(rp->Function == GENMAC_OID_SET, oid, dat, dlen);
   if (!fast) nic_unlock();

#if TRACE
   if (g_trace < 70 && oid != 0xFFFFFF0CUL) {
      g_trace++;
      tcom(10); tcom('g'); th8(rp->Function); tcom(' '); th32(oid); tcom(' '); th8((UCHAR)(dlen >> 8)); th8((UCHAR)dlen);
      tcom(' '); th32(g_st); tcom(' '); th32(g_olen);
   }
#endif
   par[1] = g_st;
   par[2] = g_olen;
   if (rp->ParmLen >= 16 &&
       !rt_dh_verify(SELECTOROF(rp->ParmPacket), 16, OFFSETOF(rp->ParmPacket), VERIFY_READWRITE))
      par[3] = g_st;                      /* genmac.dim looks for the NDIS status here */
   rp->rph.Status |= STDON;
}

/* USBD told us the dongle was removed. Keep this non-blocking (USBD callback context). */
void rtl8188eu_nic_detach(void)
{
   g_scanning = 0;
   g_pj = 0;
   g_hw_ready = 0;                              /* requests now answer "not ready" instead of waiting on dead USB */
   rtl8188eu_on_detach();
}

/* CMDShutdown: the system is going down; switch the LED off (it would otherwise stay lit until power is cut). */
void rtl8188eu_shutdown(void)
{
   if (g_hw_ready && g_usbdev.attached) {
      nic_lock();
      rtl8188eu_led_off(&g_usbdev);
      nic_unlock();
   }
}

/* CMDOpen: bring the chip up the first time the adapter is opened. */
void rtl8188eu_on_open(void)
{
   nic_lock();
   if (g_usbdev.attached && !g_hw_ready && !g_hw_failed)
      rtl8188eu_probe(&g_usbdev);
   nic_unlock();
}
