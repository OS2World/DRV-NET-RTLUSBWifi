/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hw.h - the contract between the chip-independent part of the driver (OS/2 plumbing in os2/, the station in
 * wlan/) and a chip back end (rtw/ for the Realtek RTL8821AE; the same contract as the iwlwifi driver's iwl/).
 *
 * One back end is linked into a driver binary. A back end owns: recognising its PCI functions, the firmware
 * file format, bring-up, the interrupt, scanning, the firmware objects a join needs, transmit and receive.
 * The station (wlan/sta.c) owns the 802.11 state machine that mac80211 would run on Linux: the scan table,
 * authentication and association frames, keys handed over by the supplicant, the EAPOL queue, and the
 * conversion between Ethernet and 802.11 data frames.
 *
 * All functions run at task time under nic_lock() unless marked otherwise.
 */
#ifndef HW_H
#define HW_H

struct net_s;

/* ---- identity ------------------------------------------------------------------------------------- */
int    hw_pci_match(USHORT ven, USHORT dev, USHORT subven, USHORT subdev); /* INIT: 1 = this back end drives it */
char  *hw_name(void);                           /* the adapter's name (after a match)                        */
char  *hw_fw_name(void);                        /* the firmware file the device needs                        */
int    hw_fw_is_mine(UCHAR FAR *head, USHORT n);   /* the first bytes of a file written to the device: 1 = firmware */
UCHAR  hw_vendor_oui(USHORT i);                 /* OID_GEN_VENDOR_ID bytes 0..2                              */

/* ---- state ---------------------------------------------------------------------------------------- */
extern USHORT g_hw_ready;                       /* firmware running, station usable                          */
extern USHORT g_hw_failed;                      /* bring-up failed; "up" on the device retries               */
extern USHORT g_hw_state;                       /* bring-up step reached (status text)                       */
extern UCHAR  g_hw_mac[6];                      /* the adapter's own address (NVM / OTP)                     */
extern UCHAR  g_hw_nss;                         /* receive spatial streams (HT MCS 0-7 / 0-15)               */

int    hw_up(void);                             /* full bring-up; 0 = ready                                  */
void   hw_down(void);
void   hw_service(void);                        /* process completed work: never blocks (task time or hook)  */
int    hw_wait(USHORT *flag, USHORT ms);        /* service until *flag != 0; 0 = set, -1 = timeout           */
USHORT hw_irq(void);                            /* INTERRUPT TIME: 1 = ours (device interrupt masked)        */
void   hw_hook(void);                           /* context hook: service, then unmask the device interrupt   */
void   hw_status(void (*ps)(const char *), void (*ph)(ULONG v, USHORT digits), void (*nl)(void));
int    hw_cmd(char *c);                         /* a debug command line the back end knows: 1 = handled       */

/* ---- station operations ---------------------------------------------------------------------------- */
int    hw_scan(void);                           /* blocking full scan; results through sta_beacon()          */
int    hw_chan_vht80(UCHAR ch);                /* 1 = 80 MHz (VHT) is allowed on this primary channel         */
int    hw_chan_ht40(UCHAR ch);                 /* 1 = 40 MHz is allowed on this primary channel (regulatory) */
int    hw_join_prepare(struct net_s *n);        /* go to the AP's channel, firmware objects for the AP       */
int    hw_join_complete(void);                  /* associated: AID, rates, HT from the sta state (g_j*)      */
void   hw_leave(void);                          /* undo hw_join_prepare / hw_join_complete                   */
int    hw_mgmt_tx(UCHAR FAR *frame, USHORT len);   /* a complete management frame; 0 = queued                */
/* Receive aggregation (block-ack sessions): tell the firmware to acknowledge A-MPDUs of this TID, starting at
 * sequence number ssn with a reorder window of win frames. Neither call waits (they are made from the receive
 * path); the firmware's answer is only checked afterwards. 0 = queued. */
int    hw_ba_start(UCHAR tid, USHORT ssn, USHORT win);
int    hw_ba_stop(UCHAR tid);
/* Transmit aggregation: the station sends the ADDBA request itself and tells the back end what the AP answered. */
int    hw_txagg_active(void);                   /* 1 = the back end is offering, configuring or running the aggregation of TID 0 */
int    hw_txagg_ok(void);                       /* 1 = this firmware can aggregate transmit frames (DQA queues) */
USHORT hw_qos_sn(void);                         /* the next QoS sequence number of TID 0 (start of the BA window) */
void   hw_txagg_offer(UCHAR tid, USHORT win);   /* the AP accepted the ADDBA request: move TID 0 to an aggregation queue */
void   hw_txagg_stop(UCHAR tid);                /* the session ended (DELBA, link lost): back to the ordinary queue */
int    hw_key_set(UCHAR FAR *key, USHORT klen, USHORT pairwise, UCHAR idx);   /* CCMP key; 0 = installed */
int    hw_key_clear(USHORT pairwise, UCHAR idx);

/* An 802.11 data frame: hdr = 802.11 header (24 bytes, To-DS), body = LLC/SNAP + payload.
 * flags: HWTX_PROTECT = encrypt with the pairwise key. Returns 0 queued, -2 busy. */
#define HWTX_PROTECT    0x0001
#define HWTX_EAPOL      0x0002
int    hw_data_tx(UCHAR FAR *hdr, USHORT hlen, UCHAR FAR *body, USHORT blen, USHORT flags);

#endif
