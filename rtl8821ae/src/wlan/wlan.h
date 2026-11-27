/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * wlan.h - the chip-independent station (wlan/sta.c): scan table, join, keys, EAPOL queue, data conversion.
 * Derived from ath_sta.c of the QCA6174 driver (github.com/psmedley/qca6174).
 */
#ifndef WLAN_H
#define WLAN_H

#define NET_MAX         24
#define NET_IE_MAX      320
typedef struct net_s {
   UCHAR  bssid[6];
   UCHAR  len;                                  /* SSID length                                               */
   char   ssid[32];
   UCHAR  ch;
   signed char rssi;
   UCHAR  sec;                                  /* privacy bit                                               */
   UCHAR  htc;                                  /* HT capabilities element present                           */
   UCHAR  wmm;                                  /* WMM parameter / information element present              */
   UCHAR  ht_mcs[2];                            /* the AP's rx MCS bitmap bytes 0, 1                         */
   UCHAR  vht, vhtw, vcf0;                      /* VHT capabilities present; VHT operation: 80 MHz wide, centre channel of the 80 MHz block */
   UCHAR  ht40, htw, sco;                       /* HT caps: 40 MHz capable; HT operation: STA channel width 40, secondary offset (1 above, 3 below) */
   UCHAR  basic[2];                             /* basic-rate bitmap: bit i = rate i of g_rate_tab           */
   USHORT ielen;                                /* bytes in ie[] (12 fixed beacon bytes + elements)          */
   UCHAR  ie[NET_IE_MAX];
   ULONG  seen;                                 /* dd_ms() of the last beacon / probe response               */
   ULONG  tsf_lo, tsf_hi;                       /* the beacon timestamp                                      */
   ULONG  rx_time;                              /* hardware time stamp of that beacon (chip units)           */
} net_t;
extern net_t  g_net[NET_MAX];
extern USHORT g_nnet;

/* Join state (read by the back end in hw_join_prepare / hw_join_complete) */
extern USHORT g_joined, g_authorized, g_ptk_on, g_open_net;
extern UCHAR  g_jbssid[6], g_jssid[32], g_jslen, g_jch;
extern USHORT g_jfail;                          /* reason of the last failed join (docs/TESTING.md)          */
extern UCHAR  g_jreq[200], g_jres[160];
extern USHORT g_jreqlen, g_jreslen, g_jcaps, g_jaid, g_jbcn, g_jdtim;
extern UCHAR  g_jht;                            /* HT negotiated with this AP                                */
extern UCHAR  g_j80, g_jcf0, g_jvrx[2], g_jvsgi80, g_jvexp;   /* 80 MHz (VHT) in use, centre channel, the AP's VHT rx MCS map, its short GI at 80 MHz */
extern UCHAR  g_j40, g_jsco, g_jnss;            /* 40 MHz in use, its secondary channel offset (1 above, 3 below), spatial streams */
extern UCHAR  g_jht_mcs[2];                     /* the AP's HT rx MCS bytes from its association response     */
extern UCHAR  g_jrates[12];                     /* the AP's legacy rates (association response), 802.11 units */
extern USHORT g_jnrates;
extern UCHAR  g_forcech;                        /* PROTOCOL.INI CHANNEL: join only this primary channel      */
extern UCHAR  g_maxnss;                         /* PROTOCOL.INI STREAMS (1 or 2, default 2)                   */
extern UCHAR  g_maxw;                           /* PROTOCOL.INI MAXWIDTH (0 20, 1 40, 2 80 MHz)              */
extern USHORT g_link_lost;
extern net_t *g_jnet;                           /* the scan entry being joined / joined                       */

/* called by the back end */
void   sta_rx_frame(UCHAR FAR *f, USHORT len, UCHAR chan, short rssi, USHORT rxflags, ULONG rxtime);
#define RXF_MIC_STRIPPED 0x0002                 /* with RXF_DECRYPTED: the hardware also removed the 8-byte MIC at the end (multi-queue receive) */
#define RXF_DECRYPTED   0x0001                  /* the hardware decrypted it: 8-byte CCMP header + MIC present */
void   sta_beacon(UCHAR FAR *frame, USHORT len, USHORT chan, short rssi, ULONG rxtime);
void   sta_scan_clear(void);
void   sta_age(void);
void   sta_lost(void);                          /* missed beacons, firmware kick-out                          */

/* called by the GenMAC OID layer */
void   sta_set_want(UCHAR FAR *ssid, UCHAR len, USHORT open);
int    sta_join(UCHAR FAR *want_bssid, USHORT any_bssid);   /* 0 = associated                              */
void   sta_leave(void);
void   sta_poll(void);
short  sta_rssi(void);
int    sta_key_add(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx);
int    sta_key_remove(USHORT pairwise, UCHAR idx);
int    sta_tx(UCHAR FAR *eth, USHORT len);      /* NDIS data: 0 = queued, -1 = no link, -2 = busy            */
int    sta_eapol_tx(UCHAR FAR *eth, USHORT len);
USHORT sta_eapol_count(void);
USHORT sta_eapol_get(UCHAR FAR *dst, USHORT max);
extern UCHAR  g_powersave;                        /* PROTOCOL.INI POWERSAVE = 1 enables 802.11 power save (default 0) */
extern UCHAR  g_rateadapt;                       /* PROTOCOL.INI RATEADAPT = 0 turns host rate adaptation off  */
extern UCHAR  g_aggtx;                          /* PROTOCOL.INI AGGTX = 0 turns transmit aggregation off       */
extern USHORT g_atx_req, g_atx_ok, g_atx_refused, g_atx_timeout, g_atx_delba, g_tx_qos;
extern USHORT g_ba_why_nobuf, g_ba_why_hw;
extern UCHAR  g_ampdu;                          /* PROTOCOL.INI AMPDU = 0 turns receive aggregation off        */
extern USHORT g_ba_started, g_ba_refused, g_ba_stopped, g_ba_rel, g_ba_old, g_ba_dup, g_ba_to, g_ba_max, g_ba_direct;
void   sta_rx_tick(void);                       /* the service path calls this after every run                 */
extern USHORT g_rx_8023, g_tx_8023;             /* IEEE 802.3 (LLC, no SNAP) frames: NetBEUI and the like       */
extern USHORT g_rx_mgmt[16];                     /* management frames from the AP being joined, by subtype */
extern USHORT g_rx_eapol, g_rx_ndis, g_rx_nobuf, g_rx_data, g_rx_drop[8], g_tx_data, g_tx_busy;

#endif
