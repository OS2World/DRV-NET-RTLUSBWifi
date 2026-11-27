/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sta.c - the chip-independent station: scan table, join (authentication, association), leave, keys, the EAPOL
 * queue for wpa_supplicant and the conversion between Ethernet frames (NDIS) and 802.11 data frames.
 *
 * Derived from ath_sta.c of the QCA6174 driver (github.com/psmedley/qca6174). On Linux this is mac80211's job; the chip
 * back end (hw.h) only provides the firmware side: being on the AP's channel with a station entry for it
 * (hw_join_prepare), sending frames, and the post-association setup (hw_join_complete).
 *
 * All of it runs in the GenMAC request path (task time, under nic_lock) and may block in hw_wait(), except
 * sta_rx_frame()/sta_beacon(), which run in the service path (task time, never blocking).
 */
#include "drv.h"

net_t  g_net[NET_MAX];
USHORT g_nnet;
USHORT g_joined, g_authorized, g_ptk_on, g_open_net;
UCHAR  g_jbssid[6], g_jssid[32], g_jslen, g_jch;
USHORT g_jfail;
UCHAR  g_jreq[200], g_jres[160];
USHORT g_jreqlen, g_jreslen, g_jcaps, g_jaid, g_jbcn, g_jdtim;
UCHAR  g_jht, g_jht_mcs[2], g_jrates[12];
UCHAR  g_j40, g_jsco, g_jnss = 1;
UCHAR  g_j80, g_jcf0, g_jvrx[2], g_jvsgi80, g_jvexp;
USHORT g_jnrates;
UCHAR  g_forcech, g_maxw = 2, g_maxnss = 2;      /* MAXWIDTH: 0 = 20, 1 = 40, 2 = 80 MHz (VHT)                  */
USHORT g_link_lost;
net_t *g_jnet;
USHORT g_rx_mgmt[16];
USHORT g_rx_8023, g_tx_8023;
USHORT g_rx_eapol, g_rx_ndis, g_rx_nobuf, g_rx_data, g_rx_drop[8], g_tx_data, g_tx_busy;

static USHORT g_auth_rx, g_auth_status, g_assoc_rx, g_assoc_status, g_assoc_aid, g_assoc_caps, g_assoc_reslen;
static UCHAR  g_assoc_res[192];
static UCHAR  g_want_ssid[33];
static UCHAR  g_want_len;
static USHORT g_want_open;
static UCHAR  g_target[6];
static USHORT g_joining, g_prepared, g_seq;
static UCHAR  g_frame[300];
static UCHAR  g_hdr[26];


static int mac_eq(UCHAR FAR *a, UCHAR FAR *b)
{
   return fmemeq(a, b, 6);
}

/* The element with this id in a block of information elements, or NULL. */
static UCHAR FAR *find_ie(UCHAR FAR *b, USHORT len, UCHAR id)
{
   USHORT pos = 0;

   while ((USHORT)(pos + 2) <= len) {
      if ((USHORT)(pos + 2 + b[pos + 1]) > len)
         break;
      if (b[pos] == id)
         return b + pos;
      pos = (USHORT)(pos + 2 + b[pos + 1]);
   }
   return 0;
}

/* The WMM element (vendor specific, OUI 00:50:F2, type 2) */
static UCHAR FAR *find_wmm(UCHAR FAR *b, USHORT len)
{
   USHORT pos = 0;

   while ((USHORT)(pos + 2) <= len) {
      UCHAR l = b[pos + 1];

      if ((USHORT)(pos + 2 + l) > len)
         break;
      if (b[pos] == 221 && l >= 7 && b[pos + 2] == 0x00 && b[pos + 3] == 0x50 && b[pos + 4] == 0xF2 &&
          b[pos + 5] == 2)
         return b + pos;
      pos = (USHORT)(pos + 2 + l);
   }
   return 0;
}

/* ---- scan table ------------------------------------------------------------------------------------- */
void sta_scan_clear(void)
{
   g_nnet = 0;
}

/* Drop entries not heard for 30 s (the table is not cleared at a scan: the joined AP must stay in it). */
void sta_age(void)
{
   USHORT k = 0, i;
   ULONG  now = dd_ms();

   for (i = 0; i < g_nnet; i++) {
      if ((long)(now - g_net[i].seen) > 30000L && !(g_joined && mac_eq(g_net[i].bssid, g_jbssid)))
         continue;
      if (k != i)
         g_net[k] = g_net[i];
      k++;
   }
   g_nnet = k;
   if (g_jnet && !g_joined && !g_joining)
      g_jnet = 0;
}

/* f: beacon or probe response (24-byte header); chan: the channel the frame was received on */
void sta_beacon(UCHAR FAR *f, USHORT len, USHORT chan, short rssi, ULONG rxtime)
{
   UCHAR FAR *bssid = f + 16;
   UCHAR FAR *body = f + 24;
   UCHAR FAR *e;
   USHORT blen, k, i, slot;
   net_t *n;

   if (len < 24 + 12)
      return;
   blen = (USHORT)(len - 24);
   e = find_ie(body + 12, (USHORT)(blen - 12), 3);       /* DS parameter set: more reliable than the rx channel */
   if (e && e[1] == 1)
      chan = e[2];
   slot = 0xFFFF;
   for (k = 0; k < g_nnet; k++)
      if (mac_eq(bssid, g_net[k].bssid)) { slot = k; break; }
   if (slot == 0xFFFF) {
      if (g_nnet < NET_MAX)
         slot = g_nnet++;
      else {
         slot = 0;
         for (k = 1; k < NET_MAX; k++)
            if (g_net[k].rssi < g_net[slot].rssi)
               slot = k;
         if (g_net[slot].rssi > rssi)
            return;
      }
   }
   n = &g_net[slot];
   if (n == g_jnet && (g_joining || g_joined) && !mac_eq(n->bssid, bssid))
      return;
   fmemcpy(n->bssid, bssid, 6);
   n->ch = (UCHAR)chan;
   n->rssi = (signed char)(rssi < -127 ? -127 : (rssi > 0 ? 0 : rssi));
   n->sec = (UCHAR)((body[10] & 0x10) ? 1 : 0);
   n->tsf_lo = get32(body);
   n->tsf_hi = get32(body + 4);
   n->rx_time = rxtime;
   e = find_ie(body + 12, (USHORT)(blen - 12), 45);       /* HT capabilities                                 */
   n->htc = (UCHAR)(e && e[1] >= 26);
   n->ht_mcs[0] = n->htc ? e[2 + 3] : 0;
   n->ht_mcs[1] = n->htc ? e[2 + 4] : 0;
   n->ht40 = (UCHAR)(n->htc ? ((e[2] >> 1) & 1) : 0);
   e = find_ie(body + 12, (USHORT)(blen - 12), 61);       /* HT operation                                    */
   n->htw = n->sco = 0;
   if (e && e[1] >= 22) {
      n->sco = (UCHAR)(e[3] & 3);
      n->htw = (UCHAR)((e[3] >> 2) & 1);
   }
   e = find_ie(body + 12, (USHORT)(blen - 12), 191);      /* VHT capabilities                                */
   n->vht = (UCHAR)(e && e[1] >= 12);
   n->vhtw = n->vcf0 = 0;
   e = find_ie(body + 12, (USHORT)(blen - 12), 192);      /* VHT operation: width 1 = 80 MHz, centre channel */
   if (n->vht && e && e[1] >= 5 && e[2] == 1) {
      n->vhtw = 1;
      n->vcf0 = e[3];
   }
   n->wmm = (UCHAR)(find_wmm(body + 12, (USHORT)(blen - 12)) != 0);
   n->len = 0;
   e = find_ie(body + 12, (USHORT)(blen - 12), 0);
   if (e && e[1] <= 32) {
      n->len = e[1];
      for (i = 0; i < n->len; i++)
         n->ssid[i] = (char)e[2 + i];
   }
   n->ielen = (blen > NET_IE_MAX) ? NET_IE_MAX : blen;
   fmemcpy((UCHAR FAR *)n->ie, body, n->ielen);
   n->seen = dd_ms();
}

short sta_rssi(void)
{
   USHORT k;

   for (k = 0; k < g_nnet; k++)
      if (mac_eq(g_net[k].bssid, g_jbssid))
         return g_net[k].rssi;
   return -70;
}

/* ---- what to join ---------------------------------------------------------------------------------- */
void sta_set_want(UCHAR FAR *ssid, UCHAR len, USHORT open)
{
   if (len > 32)
      len = 32;
   fmemcpy((UCHAR FAR *)g_want_ssid, ssid, len);
   g_want_ssid[len] = 0;
   g_want_len = len;
   g_want_open = open;
}

static int ssid_match(net_t *n)
{
   if (!g_want_len || n->len != g_want_len)
      return 0;
   return fmemeq((UCHAR FAR *)n->ssid, (UCHAR FAR *)g_want_ssid, g_want_len);
}

/* ---- management frames ------------------------------------------------------------------------------ */
static const UCHAR g_rates24[12] = { 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 0x30, 0x48, 0x60, 0x6C };
static const UCHAR g_rates5[8]   = { 0x8C, 0x12, 0x98, 0x24, 0xB0, 0x48, 0x60, 0x6C };
/* RSN element: version 1, group CCMP, 1 pairwise CCMP, 1 AKM PSK, no capabilities (WPA2-PSK / CCMP only) */
static const UCHAR g_rsn[22] = { 0x30, 0x14, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F,
                                 0xAC, 0x04, 0x01, 0x00, 0x00, 0x0F, 0xAC, 0x02, 0x00, 0x00 };
/* WMM information element: version 1, QoS info 0 (no U-APSD) */
static const UCHAR g_wmm_ie[9] = { 221, 7, 0x00, 0x50, 0xF2, 0x02, 0x00, 0x01, 0x00 };
/* HT capabilities: 20 MHz only, short GI 20, SM power save disabled; A-MPDU 64 KB, no density limit;
 * rx MCS 0-7 (byte 1 = MCS 8-15 is set for a two-stream radio) */
static const UCHAR g_htcap[28] = { 45, 26, 0x2C, 0x00, 0x03,
                                   0xFF, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                   0, 0, 0, 0, 0, 0, 0 };

static void seq_put(UCHAR FAR *h)
{
   USHORT fl = rt_irq_save(), q;

   g_seq = (USHORT)((g_seq + 1) & 0x0FFF);
   q = g_seq;
   rt_irq_restore(fl);
   h[22] = (UCHAR)(q << 4);                            /* sequence control: number << 4                      */
   h[23] = (UCHAR)(q >> 4);
}

static void mgmt_hdr(UCHAR fc0)
{
   rt_setmem((PVOID)g_frame, 0, 24);
   g_frame[0] = fc0;
   fmemcpy((UCHAR FAR *)g_frame + 4, (UCHAR FAR *)g_target, 6);   /* DA = AP                                    */
   fmemcpy((UCHAR FAR *)g_frame + 10, (UCHAR FAR *)g_mymac, 6);   /* SA = us                                    */
   fmemcpy((UCHAR FAR *)g_frame + 16, (UCHAR FAR *)g_target, 6);  /* BSSID                                      */
   seq_put((UCHAR FAR *)g_frame);
}

static int tx_auth(void)
{
   mgmt_hdr(0xB0);
   g_frame[24] = g_frame[25] = 0;                      /* algorithm: open system                            */
   g_frame[26] = 1; g_frame[27] = 0;                   /* transaction sequence 1                            */
   g_frame[28] = g_frame[29] = 0;                      /* status                                            */
   return hw_mgmt_tx((UCHAR FAR *)g_frame, 30);
}

static int tx_deauth(void)
{
   mgmt_hdr(0xC0);
   g_frame[24] = 3;                                    /* reason 3: leaving                                 */
   g_frame[25] = 0;
   return hw_mgmt_tx((UCHAR FAR *)g_frame, 26);
}

/* ---- receive aggregation (A-MPDU, block-ack sessions) ---------------------------------------------------------
 * The AP asks for a session with an ADDBA request (action frame, category 3). We accept it: the firmware is told
 * (hw_ba_start) so that it answers the AP's aggregates with block acks, and the frames of that TID are put back in
 * sequence-number order here, in a reorder buffer (mac80211 ieee80211_sta_manage_reorder_buf, which iwm also leaves
 * to net80211 for this firmware). A frame that is missing for BA_HOLD_MS is skipped. Frames are held as Ethernet
 * frames in DMA memory (allocated at join time: the receive path must not allocate). */
#define BA_N        2                                   /* sessions at once                                   */
#define BA_WIN      32                                  /* window: a power of two (the slot of a number is sn % win) */
#define BA_SLOT     1520
#define BA_HOLD_MS  100

typedef struct {
   UCHAR  active, tid;
   USHORT win, head, stored;                            /* head: the next sequence number to hand on          */
   USHORT len[BA_WIN];                                  /* 0 = slot free                                      */
   ULONG  stamp[BA_WIN];
} ba_t;

static void atx_tick(void);                             /* transmit aggregation, below                         */
static ba_t   g_ba[BA_N];
static DMA    g_ba_buf[BA_N];
static UCHAR  g_ba_buf_ok[BA_N];
UCHAR  g_ampdu = 1;
USHORT g_ba_why_nobuf, g_ba_why_hw;                     /* why ADDBA requests were refused: no session or buffer / the back end said no */
USHORT g_ba_started, g_ba_refused, g_ba_stopped, g_ba_rel, g_ba_old, g_ba_dup, g_ba_to, g_ba_max, g_ba_direct;

static void rx_eth(UCHAR FAR *eth, USHORT len);

static USHORT sn_sub(USHORT a, USHORT b)  { return (USHORT)((a - b) & 0x0FFF); }
static int    sn_less(USHORT a, USHORT b) { return sn_sub(a, b) > 2048; }

static UCHAR FAR *ba_slot(USHORT bi, USHORT idx)
{
   return DPTR(g_ba_buf[bi]) + idx * BA_SLOT;
}

/* Buffers for the sessions: called at the end of a join (request path). */
static void ba_alloc(void)
{
   USHORT i;

   for (i = 0; i < BA_N; i++)
      if (!g_ba_buf_ok[i] && dma_get((USHORT)(BA_WIN * BA_SLOT), &g_ba_buf[i]) == 0)
         g_ba_buf_ok[i] = 1;
}

static void ba_out(USHORT bi, ba_t *b, USHORT idx)      /* hand a held frame to the stack                      */
{
   USHORT l = b->len[idx];

   b->len[idx] = 0;
   b->stored--;
   g_ba_rel++;
   rx_eth(ba_slot(bi, idx), l);
}

static void ba_release_to(USHORT bi, ba_t *b, USHORT newhead)   /* everything below newhead, in order            */
{
   while (b->head != newhead) {
      USHORT idx = (USHORT)(b->head % b->win);

      if (b->len[idx])
         ba_out(bi, b, idx);
      b->head = (USHORT)((b->head + 1) & 0x0FFF);
   }
}

static void ba_release_consec(USHORT bi, ba_t *b)       /* the frames that are now in sequence                  */
{
   while (b->stored) {
      USHORT idx = (USHORT)(b->head % b->win);

      if (!b->len[idx])
         break;
      ba_out(bi, b, idx);
      b->head = (USHORT)((b->head + 1) & 0x0FFF);
   }
}

static void ba_input(USHORT bi, ba_t *b, USHORT sn, UCHAR FAR *eth, USHORT len)
{
   USHORT idx;

   if (sn_less(sn, b->head)) {                          /* behind the window: a duplicate                      */
      g_ba_old++;
      return;
   }
   if (!sn_less(sn, (USHORT)((b->head + b->win) & 0x0FFF)))   /* beyond the window: move it, handing on what falls off */
      ba_release_to(bi, b, (USHORT)((sn - b->win + 1) & 0x0FFF));
   idx = (USHORT)(sn % b->win);
   if (b->len[idx]) {
      g_ba_dup++;
      return;
   }
   if (sn == b->head && !b->stored) {                   /* in order and nothing held: no need to keep it       */
      b->head = (USHORT)((b->head + 1) & 0x0FFF);
      g_ba_direct++;
      rx_eth(eth, len);
      return;
   }
   fmemcpy(ba_slot(bi, idx), eth, len);
   b->len[idx] = len;
   b->stamp[idx] = dd_ms();
   b->stored++;
   if (b->stored > g_ba_max)
      g_ba_max = b->stored;
   ba_release_consec(bi, b);
}

/* Service path, after every run: a frame that has waited too long for the ones before it is handed on anyway
 * (with everything before it), until what is still held has not waited long enough. */
void sta_rx_tick(void)
{
   USHORT i, k, idx;
   ULONG  now = 0;

   atx_tick();

   for (i = 0; i < BA_N; i++) {
      ba_t *b = &g_ba[i];

      while (b->active && b->stored) {
         if (!now)
            now = dd_ms();
         for (k = 0; k < b->win; k++) {                 /* the held frame with the lowest sequence number       */
            idx = (USHORT)(((b->head + k) & 0x0FFF) % b->win);
            if (b->len[idx])
               break;
         }
         if (k == b->win || (long)(now - b->stamp[idx]) <= (long)BA_HOLD_MS)
            break;
         ba_release_to(i, b, (USHORT)((b->head + k + 1) & 0x0FFF));
         ba_release_consec(i, b);
         g_ba_to++;
      }
   }
}

static void ba_flush(USHORT bi, ba_t *b)                /* hand on everything held, in order, and close         */
{
   if (b->stored)
      ba_release_to(bi, b, (USHORT)((b->head + b->win) & 0x0FFF));
   b->active = 0;
}

static void ba_reset_all(void)                          /* the link is gone: nothing to hand on                */
{
   USHORT i, k;

   for (i = 0; i < BA_N; i++) {
      g_ba[i].active = 0;
      g_ba[i].stored = 0;
      for (k = 0; k < BA_WIN; k++)
         g_ba[i].len[k] = 0;
   }
}

static int ba_find(UCHAR tid)
{
   USHORT i;

   for (i = 0; i < BA_N; i++)
      if (g_ba[i].active && g_ba[i].tid == tid)
         return (int)i;
   return -1;
}

/* ADDBA response (action 1): body = dialog token, status, BA parameters, timeout (the request's, as mac80211 does) */
static void tx_addba_resp(UCHAR FAR *req, USHORT status, USHORT params)
{
   static UCHAR f[33];                                 /* service path: not g_frame (the request path's)     */

   rt_setmem((PVOID)f, 0, sizeof(f));
   f[0] = 0xD0;
   fmemcpy((UCHAR FAR *)f + 4, (UCHAR FAR *)g_target, 6);
   fmemcpy((UCHAR FAR *)f + 10, (UCHAR FAR *)g_mymac, 6);
   fmemcpy((UCHAR FAR *)f + 16, (UCHAR FAR *)g_target, 6);
   seq_put((UCHAR FAR *)f);
   f[24] = 3;                                          /* category: block ack                               */
   f[25] = 1;                                          /* ADDBA response                                    */
   f[26] = req[24 + 2];                                /* dialog token                                      */
   put16((UCHAR FAR *)f + 27, status);
   put16((UCHAR FAR *)f + 29, params);
   f[31] = req[24 + 5]; f[32] = req[24 + 6];           /* timeout                                           */
   hw_mgmt_tx((UCHAR FAR *)f, 33);
}

static void ba_addba_request(UCHAR FAR *f, USHORT len)
{
   USHORT params, ssn, win, req;
   UCHAR  tid;
   int    bi;
   ba_t  *b;

   if (len < 24 + 9)
      return;
   params = get16(f + 27);                              /* A-MSDU 0, policy 1, TID 5..2, buffer size 15..6     */
   tid = (UCHAR)((params >> 2) & 0x0F);
   req = (USHORT)((params >> 6) & 0x03FF);
   ssn = (USHORT)(get16(f + 31) >> 4);
   bi = -1;
   if (g_ampdu && g_joined && g_jht && (params & 0x0002) && tid < 8) {
      bi = ba_find(tid);
      if (bi >= 0) {                                    /* the AP starts the session again                     */
         ba_flush((USHORT)bi, &g_ba[bi]);
         hw_ba_stop(tid);
      } else {
         USHORT i;

         for (i = 0; i < BA_N; i++)
            if (!g_ba[i].active && g_ba_buf_ok[i]) { bi = (int)i; break; }
      }
   }
   if (bi < 0) {                                        /* no (more) sessions: refuse, the AP sends single MPDUs */
      g_ba_refused++;
      g_ba_why_nobuf++;
      tx_addba_resp(f, 37, params);                     /* status 37: request declined                         */
      return;
   }
   win = req ? req : BA_WIN;
   if (win > BA_WIN)
      win = BA_WIN;
   while (win & (win - 1))                              /* a power of two                                      */
      win &= (USHORT)(win - 1);
   b = &g_ba[bi];
   if (hw_ba_start(tid, ssn, win)) {
      g_ba_refused++;
      g_ba_why_hw++;
      tx_addba_resp(f, 37, params);
      return;
   }
   b->tid = tid;
   b->win = win;
   b->head = ssn;
   b->stored = 0;
   rt_setmem((PVOID)b->len, 0, sizeof(b->len));
   b->active = 1;
   g_ba_started++;
   tx_addba_resp(f, 0, (USHORT)(0x0002 | (tid << 2) | (win << 6)));   /* immediate, no A-MSDU, our window     */
}

/* ---- transmit aggregation: the ADDBA request side (TID 0) -----------------------------------------------------
 * After a few dozen QoS data frames (a bulk transfer, not a ping) one ADDBA request goes out; an accepting response
 * hands TID 0 to the back end (hw_txagg_offer). A refusal or silence is retried much later. Built on the service
 * path (sta_rx_tick), not in the transmit path. */
#define ATX_IDLE   0
#define ATX_WAIT   1                                    /* request sent, waiting for the response               */
#define ATX_ON     2
UCHAR  g_aggtx = 1;
UCHAR  g_powersave;                                      /* PROTOCOL.INI POWERSAVE = 1                           */
UCHAR  g_rateadapt = 1;                                 /* PROTOCOL.INI RATEADAPT                               */
static UCHAR  g_atx, g_atx_pend, g_atx_tok;
static USHORT g_atx_cnt;
static ULONG  g_atx_t, g_atx_next;
UCHAR  g_ampdu_par;                                     /* the AP's HT capabilities A-MPDU parameters byte      */
USHORT g_atx_req, g_atx_ok, g_atx_refused, g_atx_timeout, g_atx_delba, g_tx_qos;

static void atx_reset(void)
{
   g_atx = ATX_IDLE;
   g_atx_pend = 0;
   g_atx_cnt = 0;
   g_atx_next = 0;
}

static void atx_backoff(ULONG ms)
{
   g_atx = ATX_IDLE;
   g_atx_cnt = 0;
   g_atx_next = dd_ms() + ms;
   if (!g_atx_next)
      g_atx_next = 1;
}

static void tx_addba_req(void)
{
   static UCHAR f[33];

   rt_setmem((PVOID)f, 0, sizeof(f));
   f[0] = 0xD0;
   fmemcpy((UCHAR FAR *)f + 4, (UCHAR FAR *)g_target, 6);
   fmemcpy((UCHAR FAR *)f + 10, (UCHAR FAR *)g_mymac, 6);
   fmemcpy((UCHAR FAR *)f + 16, (UCHAR FAR *)g_target, 6);
   seq_put((UCHAR FAR *)f);
   f[24] = 3;                                          /* category: block ack                               */
   f[25] = 0;                                          /* ADDBA request                                     */
   g_atx_tok++;
   if (!g_atx_tok)
      g_atx_tok = 1;
   f[26] = g_atx_tok;
   put16((UCHAR FAR *)f + 27, (USHORT)(0x0002 | (0 << 2) | (64 << 6)));   /* immediate BA, TID 0, 64 buffers   */
   put16((UCHAR FAR *)f + 31, (USHORT)(hw_qos_sn() << 4));                /* starting sequence number          */
   g_atx = ATX_WAIT;
   g_atx_t = dd_ms();
   g_atx_req++;
   hw_mgmt_tx((UCHAR FAR *)f, 33);
}

/* Service path, after every run. */
static void atx_tick(void)
{
   ULONG now;

   if (!g_joined || !g_jht || !g_aggtx) {
      return;
   }
   if (g_atx == ATX_ON && !hw_txagg_active()) {            /* the back end gave up (the normal queue never emptied) or failed: ask again later */
      atx_backoff(3000UL);
      return;
   }
   if (g_atx == ATX_WAIT) {
      if ((long)(dd_ms() - g_atx_t) > 1000L) {
         g_atx_timeout++;
         atx_backoff(10000UL);
      }
      return;
   }
   if (g_atx == ATX_IDLE && g_atx_pend) {
      g_atx_pend = 0;
      now = dd_ms();
      if (g_atx_next && (long)(now - g_atx_next) < 0)
         return;
      if (hw_txagg_ok())
         tx_addba_req();
   }
}

static void atx_response(UCHAR FAR *f, USHORT len)   /* ADDBA response: token, status[2], params[2], timeout[2] */
{
   USHORT status, params;

   if (len < 24 + 9 || g_atx != ATX_WAIT || f[26] != g_atx_tok)
      return;
   status = get16(f + 27);
   params = get16(f + 29);
   if (status != 0 || ((params >> 2) & 0x0F) != 0) {
      g_atx_refused++;
      atx_backoff(60000UL);
      return;
   }
   g_atx = ATX_ON;
   g_atx_ok++;
   hw_txagg_offer(0, (USHORT)((params >> 6) & 0x03FF));
}

static void atx_delba(UCHAR FAR *f, USHORT len)
{
   USHORT params;

   if (len < 24 + 6)
      return;
   params = get16(f + 26);
   if ((params & 0x0800) || ((params >> 12) & 0x0F) != 0)   /* ours: initiator bit clear, TID 0                */
      return;
   if (g_atx == ATX_ON) {
      hw_txagg_stop(0);
      g_atx_delba++;
      atx_backoff(10000UL);
   }
}

static void ba_delba(UCHAR FAR *f, USHORT len)
{
   USHORT params;
   UCHAR  tid;
   int    bi;

   if (len < 24 + 6)
      return;
   params = get16(f + 26);                              /* initiator 11, TID 15..12                            */
   tid = (UCHAR)((params >> 12) & 0x0F);
   if (!(params & 0x0800))                              /* the AP deletes a session of ours as recipient: not ours */
      return;
   bi = ba_find(tid);
   if (bi >= 0) {
      ba_flush((USHORT)bi, &g_ba[bi]);
      hw_ba_stop(tid);
      g_ba_stopped++;
   }
}

static int tx_assoc(net_t *n)
{
   USHORT i, k = 24, cap;
   UCHAR  hi = (UCHAR)(n->ch > 14);

   mgmt_hdr(0x00);
   cap = (USHORT)(g_want_open ? 0x0021 : 0x0031);      /* ESS, [privacy], short preamble                    */
   if (!hi)
      cap |= 0x0400;                                   /* short slot time                                   */
   g_frame[k++] = (UCHAR)cap;
   g_frame[k++] = (UCHAR)(cap >> 8);
   g_frame[k++] = 10; g_frame[k++] = 0;                /* listen interval                                   */
   g_frame[k++] = 0;  g_frame[k++] = n->len;           /* SSID                                              */
   for (i = 0; i < n->len; i++)
      g_frame[k++] = (UCHAR)n->ssid[i];
   g_frame[k++] = 1;  g_frame[k++] = 8;                /* supported rates                                   */
   for (i = 0; i < 8; i++)
      g_frame[k++] = hi ? g_rates5[i] : g_rates24[i];
   if (!hi) {
      g_frame[k++] = 50; g_frame[k++] = 4;             /* extended supported rates                          */
      for (i = 8; i < 12; i++)
         g_frame[k++] = g_rates24[i];
   }
   if (!g_want_open)
      for (i = 0; i < 22; i++)
         g_frame[k++] = g_rsn[i];
   if (g_jht) {                                        /* after the RSN element, as mac80211 orders them     */
      for (i = 0; i < 28; i++)
         g_frame[k++] = g_htcap[i];
      if (g_hw_nss >= 2 && g_maxnss >= 2)
         g_frame[k - 28 + 6] = 0xFF;                   /* rx MCS 8-15                                        */
      if (g_j40)
         g_frame[k - 28 + 2] |= 0x42;                  /* supports 40 MHz, short GI 40                       */
   }
   if (g_j80) {                                        /* VHT capabilities: 80 MHz, short GI 80, A-MPDU 64 KB */
      UCHAR nss2 = (UCHAR)(g_hw_nss >= 2 && g_maxnss >= 2);

      g_frame[k++] = 191; g_frame[k++] = 12;
      g_frame[k++] = 0x20; g_frame[k++] = 0; g_frame[k++] = 0x80; g_frame[k++] = 0x01;   /* info: 3895, SGI80, A-MPDU exp 3 */
      g_frame[k++] = (UCHAR)(nss2 ? 0xFA : 0xFE); g_frame[k++] = 0xFF;                 /* rx MCS 0-9 for 1 (2) streams */
      g_frame[k++] = 0; g_frame[k++] = 0;
      g_frame[k++] = (UCHAR)(nss2 ? 0xFA : 0xFE); g_frame[k++] = 0xFF;                 /* tx MCS map                    */
      g_frame[k++] = 0; g_frame[k++] = 0;
   }
   if (n->wmm)
      for (i = 0; i < 9; i++)
         g_frame[k++] = g_wmm_ie[i];
   g_jreqlen = (USHORT)(k - 28);                       /* what the supplicant reads back as request IEs     */
   if (g_jreqlen > sizeof(g_jreq))
      g_jreqlen = sizeof(g_jreq);
   fmemcpy((UCHAR FAR *)g_jreq, (UCHAR FAR *)g_frame + 28, g_jreqlen);
   return hw_mgmt_tx((UCHAR FAR *)g_frame, k);
}

/* A management frame that is not a beacon or probe response. Service path. */
static void sta_mgmt_rx(UCHAR FAR *f, USHORT len)
{
   UCHAR fc0 = f[0];
   USHORT n;

   if (!g_joining && !g_joined)
      return;
   if (!mac_eq(f + 10, (UCHAR FAR *)g_target))          /* sender must be the AP we talk to                  */
      return;
   g_rx_mgmt[(fc0 >> 4) & 15]++;
   if (fc0 == 0xB0 && len >= 30) {                       /* authentication                                    */
      if (f[26] == 2 && f[27] == 0) {                    /* transaction sequence 2                            */
         g_auth_status = get16(f + 28);
         g_auth_rx = 1;
      }
   } else if ((fc0 == 0x10 || fc0 == 0x30) && len >= 30) {   /* association / reassociation response          */
      g_assoc_caps   = get16(f + 24);
      g_assoc_status = get16(f + 26);
      g_assoc_aid    = (USHORT)(get16(f + 28) & 0x3FFF);
      n = (USHORT)(len - 30);
      if (n > sizeof(g_assoc_res))
         n = sizeof(g_assoc_res);
      fmemcpy((UCHAR FAR *)g_assoc_res, f + 30, n);
      g_assoc_reslen = n;
      g_assoc_rx = 1;
   } else if (fc0 == 0xC0 || fc0 == 0xA0) {              /* deauthentication / disassociation                 */
      g_link_lost = 1;
   } else if (fc0 == 0xD0 && len >= 26 && g_joined) {    /* action                                            */
      if (f[24] == 3 && f[25] == 0)                      /* block ack: ADDBA request                          */
         ba_addba_request(f, len);
      else if (f[24] == 3 && f[25] == 2)                 /* block ack: DELBA                                  */
         ba_delba(f, len);
      if (f[24] == 3 && f[25] == 1)                      /* block ack: ADDBA response (to our request)        */
         atx_response(f, len);
      if (f[24] == 3 && f[25] == 2)
         atx_delba(f, len);
   }
}

void sta_lost(void)
{
   if (g_joined)
      g_link_lost = 1;
}

/* ---- Ethernet <-> 802.11 payload conversion (RFC 1042 / IEEE 802.1H, as mac80211 does it) ---------------------
 * An Ethernet II frame (type >= 0600h) travels with a SNAP header AA AA 03 00 00 00 + type; AppleTalk ARP (80F3h) and
 * IPX (8137h) use the bridge-tunnel variant (OUI 00 00 F8) so that the receiver does not mistake them for
 * RFC 1042. An IEEE 802.3 frame (the "type" is the length of what follows: NetBEUI, NetBIOS over LLC, bridge BPDUs)
 * travels with its own LLC header, unchanged. */
/* LLC_BEGIN */
/* The LLC part of an 802.11 data frame -> an Ethernet frame in eth (after the 12 address bytes): returns the
 * number of bytes written after the addresses, or 0 when the LLC header is unusable. */
static USHORT llc_to_eth(UCHAR FAR *llc, USHORT plen, UCHAR FAR *eth, USHORT FAR *is8023)
{
   USHORT et;

   *is8023 = 0;
   if (plen < 3)
      return 0;
   if (plen >= 8 && llc[0] == 0xAA && llc[1] == 0xAA && llc[2] == 0x03 && llc[3] == 0 && llc[4] == 0) {
      et = (USHORT)((llc[6] << 8) | llc[7]);
      if ((llc[5] == 0x00 && et != 0x80F3 && et != 0x8137) || llc[5] == 0xF8) {
         fmemcpy(eth + 12, llc + 6, (USHORT)(plen - 6));      /* type + payload                                */
         return (USHORT)(plen - 6);
      }
   }
   if (plen > 1500)
      return 0;
   eth[12] = (UCHAR)(plen >> 8);                       /* IEEE 802.3: the length, then the LLC header as it is */
   eth[13] = (UCHAR)plen;
   fmemcpy(eth + 14, llc, plen);
   *is8023 = 1;
   return (USHORT)(plen + 2);
}

/* An Ethernet frame -> the LLC part of an 802.11 data frame: eth[6..11] may be overwritten (SNAP header). Returns
 * the start of the part to send; its length is *blen. */
static UCHAR FAR *eth_to_llc(UCHAR FAR *eth, USHORT len, USHORT FAR *blen, USHORT FAR *is8023)
{
   USHORT et = (USHORT)((eth[12] << 8) | eth[13]);

   *is8023 = 0;
   if (et < 0x0600) {                                  /* IEEE 802.3: already has its LLC header              */
      *is8023 = 1;
      *blen = (USHORT)(len - 14);
      return eth + 14;
   }
   eth[6] = 0xAA; eth[7] = 0xAA; eth[8] = 0x03; eth[9] = 0; eth[10] = 0;
   eth[11] = (UCHAR)((et == 0x80F3 || et == 0x8137) ? 0xF8 : 0x00);
   *blen = (USHORT)(len - 6);
   return eth + 6;
}
/* LLC_END */

/* ---- receive ----------------------------------------------------------------------------------------- */
#define EQ_N   4
#define EQ_SZ  512
static UCHAR  g_eq[EQ_N][EQ_SZ];
static USHORT g_eql[EQ_N], g_eqh, g_eqt, g_eqn, g_eq_drop;
static UCHAR  g_eth[1600];                          /* Ethernet frame built from an 802.11 one (the LLC header shrinks it) */

USHORT sta_eapol_count(void)
{
   return g_eqn;
}

USHORT sta_eapol_get(UCHAR FAR *dst, USHORT max)
{
   USHORT n;

   if (!g_eqn)
      return 0;
   n = g_eql[g_eqt];
   if (n > max)
      n = max;
   fmemcpy(dst, (UCHAR FAR *)g_eq[g_eqt], n);
   g_eqt = (USHORT)((g_eqt + 1) % EQ_N);
   g_eqn--;
   return n;
}

static void rx_eth(UCHAR FAR *eth, USHORT len)
{
   static USHORT h;                                    /* address passed on: static (SS != DS)               */
   UCHAR FAR *d;

   if (eth[12] == 0x88 && eth[13] == 0x8E) {           /* EAPOL: to the supplicant, never to the stack      */
      g_rx_eapol++;
      if (g_eqn >= EQ_N || len > EQ_SZ) {
         g_eq_drop++;
         return;
      }
      fmemcpy((UCHAR FAR *)g_eq[g_eqh], eth, len);
      g_eql[g_eqh] = len;
      g_eqh = (USHORT)((g_eqh + 1) % EQ_N);
      g_eqn++;
      return;
   }
   if (!g_open_net && !g_ptk_on) {                     /* secure network, no keys yet: nothing else passes  */
      g_rx_drop[5]++;
      return;
   }
   d = ndis_rx_get(&h);
   if (!d) {
      g_rx_nobuf++;
      return;
   }
   fmemcpy(d, eth, len);
   g_rx_ndis++;
   ndis_rx_put(h, len);
}

/* An 802.11 frame from the air (FCS removed). Service path: must not block. */
void sta_rx_frame(UCHAR FAR *f, USHORT len, UCHAR chan, short rssi, USHORT rxflags, ULONG rxtime)
{
   UCHAR  fc0, fc1, type, tid;
   USHORT hl, elen;
   int    bi;

   if (len < 24)
      return;
   fc0 = f[0];
   fc1 = f[1];
   type = (UCHAR)((fc0 >> 2) & 3);
   if (type == 0) {
      if (fc0 == 0x80 || fc0 == 0x50) {                /* beacon, probe response                            */
         sta_beacon(f, len, chan, rssi, rxtime);
         if (g_joined && mac_eq(f + 16, (UCHAR FAR *)g_jbssid))
            g_link_lost = 0;
      } else
         sta_mgmt_rx(f, len);
      return;
   }
   if (type != 2)
      return;
   g_rx_data++;
   if (!g_joined || (fc1 & 0x03) != 0x02 || !mac_eq(f + 10, (UCHAR FAR *)g_jbssid)) {
      g_rx_drop[0]++;                                  /* not From-DS from our AP                           */
      return;
   }
   if (fc0 & 0x40) {                                   /* null data / QoS null                              */
      g_rx_drop[1]++;
      return;
   }
   hl = 24;
   tid = 0xFF;
   if (fc0 & 0x80) {                                   /* QoS data                                          */
      tid = (UCHAR)(f[24] & 0x0F);
      if (f[24] & 0x80) {                              /* A-MSDU: not negotiated, not handled               */
         g_rx_drop[2]++;
         return;
      }
      hl += 2;
      if (fc1 & 0x80)
         hl += 4;                                      /* HT control                                        */
   }
   if (fc1 & 0x40) {                                   /* protected                                         */
      if (!(rxflags & RXF_DECRYPTED)) {
         g_rx_drop[3]++;
         return;
      }
      hl += 8;                                         /* CCMP header; the 8-byte MIC is at the end         */
      if (!(rxflags & RXF_MIC_STRIPPED)) {
         if (len < hl + 8) { g_rx_drop[4]++; return; }
         len = (USHORT)(len - 8);
      }
   }
   if (len < hl + 3 || (USHORT)(len - hl + 14) > sizeof(g_eth)) {
      g_rx_drop[4]++;
      return;
   }
   fmemcpy((UCHAR FAR *)g_eth, f + 4, 6);              /* destination = address 1                           */
   fmemcpy((UCHAR FAR *)g_eth + 6, f + 16, 6);         /* source = address 3 (From-DS)                      */
   {
      USHORT n8023, n = llc_to_eth(f + hl, (USHORT)(len - hl), (UCHAR FAR *)g_eth, &n8023);

      if (!n) {
         g_rx_drop[6]++;                               /* no usable LLC header                              */
         return;
      }
      g_rx_8023 += n8023;
      elen = (USHORT)(n + 12);
   }
   bi = (tid < 8 && !(f[4] & 1)) ? ba_find(tid) : -1;   /* unicast QoS data of a block-ack session: in order   */
   if (bi >= 0)
      ba_input((USHORT)bi, &g_ba[bi], (USHORT)(get16(f + 22) >> 4), (UCHAR FAR *)g_eth, elen);
   else
      rx_eth((UCHAR FAR *)g_eth, elen);
   (void)rssi;
}

/* ---- transmit ---------------------------------------------------------------------------------------- */
/* eth: an Ethernet frame the caller lets us modify (bytes 6..11 become the LLC/SNAP header). */
static USHORT g_txlk;                                  /* NDIS TransmitChain may run at interrupt time, on any CPU */

static int tx_eth(UCHAR FAR *eth, USHORT len, USHORT flags)
{
   int    rc;
   USHORT fl, hl;

   if (len < 14)
      return -1;
   fl = rt_irq_save();
   if (rt_lock_acquire(&g_txlk, (USHORT)(rt_cpu_id() + 1))) {
      rt_irq_restore(fl);                              /* re-entered on this CPU: busy                       */
      g_tx_busy++;
      return -2;
   }
   g_hdr[0] = 0x08;                                    /* data                                              */
   g_hdr[1] = (UCHAR)(0x01 | ((flags & HWTX_PROTECT) ? 0x40 : 0));   /* To-DS, [protected]                    */
   g_hdr[2] = g_hdr[3] = 0;
   fmemcpy((UCHAR FAR *)g_hdr + 4, (UCHAR FAR *)g_jbssid, 6);      /* receiver = AP                        */
   fmemcpy((UCHAR FAR *)g_hdr + 10, eth + 6, 6);                    /* transmitter = source                 */
   fmemcpy((UCHAR FAR *)g_hdr + 16, eth, 6);                        /* destination                          */
   seq_put((UCHAR FAR *)g_hdr);
   hl = 24;
   if (g_aggtx && g_jht && !(flags & HWTX_EAPOL)) {   /* QoS data, TID 0 (best effort): the aggregation candidate */
      g_hdr[0] = 0x88;
      g_hdr[24] = g_hdr[25] = 0;
      hl = 26;
      g_tx_qos++;
      if (g_atx == ATX_IDLE && !g_atx_pend && ++g_atx_cnt >= 24)
         g_atx_pend = 1;
   }
   {
      USHORT blen, n8023;
      UCHAR FAR *body = eth_to_llc(eth, len, &blen, &n8023);

      g_tx_8023 += n8023;
      if (blen < 3 || blen > 1536) {                   /* an 802.3 frame without an LLC header, or too long    */
         rt_lock_release(&g_txlk);
         rt_irq_restore(fl);
         return -1;
      }
      rc = hw_data_tx((UCHAR FAR *)g_hdr, hl, body, blen, flags);
   }
   rt_lock_release(&g_txlk);
   rt_irq_restore(fl);
   if (rc == 0)
      g_tx_data++;
   else
      g_tx_busy++;
   return rc;
}

int sta_tx(UCHAR FAR *eth, USHORT len)
{
   if (!g_joined)
      return -1;
   if (!g_open_net && !g_ptk_on)
      return -1;                                       /* never send data in the clear on a secure network */
   return tx_eth(eth, len, (USHORT)(g_open_net ? 0 : HWTX_PROTECT));
}

int sta_eapol_tx(UCHAR FAR *eth, USHORT len)
{
   static UCHAR buf[EQ_SZ];

   if (!g_joined || len > sizeof(buf))
      return -1;
   fmemcpy((UCHAR FAR *)buf, eth, len);
   /* Unprotected while the pairwise key is not installed (4-way handshake messages 2 and 4); afterwards the
    * group-key handshake and re-keying must be encrypted, or the access point ignores them. */
   return tx_eth((UCHAR FAR *)buf, len, (USHORT)(HWTX_EAPOL | (g_ptk_on ? HWTX_PROTECT : 0)));
}

/* ---- join --------------------------------------------------------------------------------------------- */
static void abort_join(void)
{
   if (g_prepared) {
      hw_leave();
      g_prepared = 0;
   }
   g_joining = 0;
}

/* The legacy rates of the AP's association response (supported + extended rates), at most 12. */
static void ap_rates(void)
{
   USHORT pos = 0, i;

   g_jnrates = 0;
   while (pos + 2 <= g_assoc_reslen) {
      UCHAR id = g_assoc_res[pos], l = g_assoc_res[pos + 1];

      if ((USHORT)(pos + 2 + l) > g_assoc_reslen)
         break;
      if (id == 1 || id == 50)
         for (i = 0; i < l && g_jnrates < 12; i++)
            g_jrates[g_jnrates++] = g_assoc_res[pos + 2 + i];
      pos = (USHORT)(pos + 2 + l);
   }
}

int sta_join(UCHAR FAR *want_bssid, USHORT any)
{
   USHORT k, best = 0xFFFF, att, pos;
   int    bestr = -1000, rv;
   net_t *n;
   UCHAR FAR *h;

   g_jfail = 0;
   if (!g_hw_ready) { g_jfail = 8; return -1; }
   if (g_joined)
      sta_leave();
   g_link_lost = 0;
   for (k = 0; k < g_nnet; k++) {
      if (!ssid_match(&g_net[k]))
         continue;
      if (!any && !mac_eq(g_net[k].bssid, want_bssid))
         continue;
      if (g_forcech && g_net[k].ch != g_forcech)
         continue;
      rv = g_net[k].rssi ? (int)g_net[k].rssi : -120;
      if (best == 0xFFFF || rv > bestr) {
         best = k;
         bestr = rv;
      }
   }
   if (best == 0xFFFF) {
      g_jfail = 1;                                      /* no network with that SSID in the scan table       */
      return -1;
   }
   n = &g_net[best];
   g_jnet = n;
   fmemcpy((UCHAR FAR *)g_target, (UCHAR FAR *)n->bssid, 6);
   fmemcpy((UCHAR FAR *)g_jbssid, (UCHAR FAR *)n->bssid, 6);
   g_jch = n->ch;
   g_joining = 1;
   g_auth_status = g_assoc_status = 0xFFFF;

   g_jbcn = get16((UCHAR FAR *)n->ie + 8);
   if (g_jbcn < 10 || g_jbcn > 2000)
      g_jbcn = 100;
   g_jdtim = 1;
   h = (n->ielen > 12) ? find_ie((UCHAR FAR *)n->ie + 12, (USHORT)(n->ielen - 12), 5) : 0;
   if (h && h[1] >= 2 && h[3] >= 1)
      g_jdtim = h[3];                                   /* TIM element: DTIM period                          */
   g_jht = n->htc;
   g_j40 = g_jsco = 0;
   if (g_jht && g_maxw >= 1 && n->ht40 && n->htw && (n->sco == 1 || n->sco == 3) && hw_chan_ht40(n->ch)) {
      g_j40 = 1;                                        /* the AP runs a 40 MHz channel and may use it here: so do we */
      g_jsco = n->sco;
   }
   g_j80 = 0;                                           /* 80 MHz: the AP's VHT operation, our regulatory data, consistent offsets */
   if (g_j40 && g_maxw >= 2 && n->vht && n->vhtw && n->ch > 14 && hw_chan_vht80(n->ch)) {
      int d = (int)n->ch - (int)n->vcf0;

      /* the 20 MHz channels of an 80 MHz block are at -6, -2, +2, +6 from its centre; the 40 MHz pairs are (-6, -2) and (+2, +6):
       * the first of a pair has its secondary above (sco 1), the second below (sco 3) */
      if ((d == -6 || d == 2) ? g_jsco == 1 : ((d == -2 || d == 6) ? g_jsco == 3 : 0)) {
         g_j80 = 1;
         g_jcf0 = n->vcf0;
      }
   }
   g_jaid = 0;

   if (hw_join_prepare(n)) { g_jfail = 9; abort_join(); return -1; }
   g_prepared = 1;

   for (att = 1; att <= 3; att++) {                       /* open-system authentication                        */
      g_auth_rx = 0;
      if (tx_auth()) { g_jfail = 10; abort_join(); return -1; }
      if (!hw_wait(&g_auth_rx, 400))
         break;
   }
   if (!g_auth_rx)               { g_jfail = 2; abort_join(); return -1; }   /* no reply                            */
   if (g_auth_status != 0)       { g_jfail = 3; abort_join(); return -1; }   /* refused                             */

   for (att = 1; att <= 3; att++) {                       /* association                                       */
      g_assoc_rx = 0;
      if (tx_assoc(n)) { g_jfail = 10; abort_join(); return -1; }
      if (!hw_wait(&g_assoc_rx, 600))
         break;
   }
   if (!g_assoc_rx)              { g_jfail = 4; abort_join(); return -1; }   /* no reply                            */
   if (g_assoc_status != 0)      { g_jfail = 5; abort_join(); return -1; }   /* refused                             */

   ap_rates();
   h = g_jht ? find_ie((UCHAR FAR *)g_assoc_res, g_assoc_reslen, 45) : 0;
   if (h && h[1] < 26)
      h = 0;
   g_jht = (UCHAR)(h != 0);                              /* the AP confirmed HT in its response               */
   if (!g_jht)
      g_j40 = 0;
   g_jht_mcs[0] = h ? h[2 + 3] : 0;
   g_jht_mcs[1] = h ? h[2 + 4] : 0;
   g_ampdu_par = h ? h[2 + 2] : 0;
   if (g_j80) {                                          /* the AP must confirm VHT in its response           */
      UCHAR FAR *v = find_ie((UCHAR FAR *)g_assoc_res, g_assoc_reslen, 191);

      if (!g_jht || !v || v[1] < 12)
         g_j80 = 0;
      else {
         g_jvrx[0] = v[2 + 4];
         g_jvrx[1] = v[2 + 5];
         g_jvsgi80 = (UCHAR)((v[2] & 0x20) != 0);
         g_jvexp = (UCHAR)(((v[2 + 2] >> 7) & 1) | ((v[2 + 3] & 3) << 1));   /* maximum A-MPDU length exponent */
      }
   }
   g_jaid = g_assoc_aid;
   g_jcaps = g_assoc_caps;
   for (pos = 0; pos < 32; pos++)
      g_jssid[pos] = (UCHAR)n->ssid[pos];
   g_jslen = n->len;
   g_jreslen = (g_assoc_reslen > sizeof(g_jres)) ? (USHORT)sizeof(g_jres) : g_assoc_reslen;
   fmemcpy((UCHAR FAR *)g_jres, (UCHAR FAR *)g_assoc_res, g_jreslen);

   if (hw_join_complete()) {
      g_jfail = 11;
      tx_deauth();
      abort_join();
      return -1;
   }
   g_open_net = g_want_open;
   g_ptk_on = 0;
   g_authorized = 0;
   g_joined = 1;
   g_joining = 0;
   if (g_want_open)
      g_authorized = 1;                                   /* nothing to wait for on an open network            */
   ba_alloc();                                             /* the reorder buffers (not in the receive path)     */
   tstr("[joined aid "); th16(g_jaid); tstr("]"); tnl();
   return 0;
}

void sta_leave(void)
{
   if (!g_joined && !g_joining && !g_prepared)
      return;
   if (g_joined)
      tx_deauth();
   g_joined = g_authorized = g_ptk_on = 0;
   ba_reset_all();
   atx_reset();
   g_jht = 0;
   g_j40 = g_jsco = g_j80 = 0;
   g_jnss = 1;
   abort_join();
   g_link_lost = 0;
}

/* Request path: notice that the AP is gone (deauthentication, disassociation, missed beacons). */
void sta_poll(void)
{
   if (g_link_lost && g_joined) {
      tstr("[link lost]"); tnl();
      sta_leave();
   }
   g_link_lost = 0;
}

/* ---- keys ------------------------------------------------------------------------------------------- */
int sta_key_add(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx)
{
   if (!g_joined)
      return -1;
   if (klen != 16)
      return -2;                                          /* CCMP only                                         */
   if (hw_key_set(key, 16, pairwise, idx))
      return -3;
   if (pairwise) {
      g_ptk_on = 1;
      g_authorized = 1;
   }
   return 0;
}

int sta_key_remove(USHORT pairwise, UCHAR idx)
{
   if (!g_joined || !pairwise)
      return 0;                                           /* group keys are replaced, not removed              */
   g_ptk_on = 0;
   return hw_key_clear(1, idx);
}
