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
 * rt_usb.h - USB transport of the RTL8188EU driver, and the interfaces of the driver's modules.
 *
 * The USB structures below are written from the "Basic USB Device Driver Stack for OS/2 Warp -
 * Programmer's Reference" (section 6) and the USB 2.0 specification (chapter 9); see v012/PROVENANCE.md.
 * Thin layer over the USBD IDC interface giving the rest of the driver a small API:
 *   - usb_register_client()  : register with USBD, get attach callbacks
 *   - usb_ctrl_msg()         : one USB control transfer (register read/write)
 *   - usb_bulk_msg()         : one USB bulk transfer (firmware download)
 *   - usb_rx_start / usb_tx_submit : the asynchronous data path
 */
#ifndef RT_USB_H
#define RT_USB_H

#include "rt_os2dd.h"

#pragma pack(push, 1)

/* USB 2.0 section 9.3: setup packet */
typedef struct {
   UCHAR  bmRequestType;
   UCHAR  bRequest;
   USHORT wValue;
   USHORT wIndex;
   USHORT wLength;
} RT_SETUP;

/* USB 2.0 section 9.6.1: device descriptor */
typedef struct {
   UCHAR  bLength;
   UCHAR  bDescriptorType;
   USHORT bcdUSB;
   UCHAR  bDeviceClass;
   UCHAR  bDeviceSubClass;
   UCHAR  bDeviceProtocol;
   UCHAR  bMaxPacketSize0;
   USHORT idVendor;
   USHORT idProduct;
   USHORT bcdDevice;
   UCHAR  iManufacturer;
   UCHAR  iProduct;
   UCHAR  iSerialNumber;
   UCHAR  bNumConfigurations;
} RT_DEVDESC;

/* USBPRM 6.1.3.3: device information handed to the class/client driver (USBD's segment) */
typedef struct {
   UCHAR      ctrlID;
   UCHAR      deviceAddress;
   UCHAR      bConfigurationValue;
   UCHAR      bInterfaceNumber;
   UCHAR      lowSpeedDevice;
   UCHAR      portNum;
   USHORT     parentHubIndex;
   ULONG      rmDevHandle;
   RT_SETUP   clearStalled;
   RT_DEVDESC descriptor;
   UCHAR      configurationData[1];
} RT_DEVINFO;

/* USBPRM 6.1.1.1: I/O request block */
typedef struct {
   UCHAR  controllerId;
   UCHAR  deviceAddress;
   UCHAR  endPointId;
   UCHAR  status;
   USHORT flags;
   PUCHAR buffer1;
   USHORT buffer1Length;
   PUCHAR buffer2;
   USHORT buffer2Length;
   USHORT serviceTime;
   USHORT maxPacketSize;
   ULONG  usbIDC;                  /* far entry point called on completion */
   USHORT usbDS;                   /* its data segment                     */
   UCHAR  category;                /* category value used in the IRQ call  */
   ULONG  requestData1;
   ULONG  requestData2;
   ULONG  requestData3;
   UCHAR  maxErrorCount;
   ULONG  nextRb;                  /* not used, NULL                       */
   UCHAR  altInterface;
   UCHAR  isoFlags;
   USHORT isoFrameLength;
   USHORT isoBuffers;
   UCHAR  Mult;
   UCHAR  HubAddr;
   UCHAR  PortNumb;
} RT_USBRB;

/* USBPRM 6.1.1.2: cancel I/O requests (endPointId 0xFF = all) */
typedef struct {
   UCHAR  controllerId;
   UCHAR  deviceAddress;
   UCHAR  endPointId;
} RT_USBCANCEL;

/* USBPRM 6.1.3.4: detach device */
typedef struct {
   UCHAR  controllerId;
   UCHAR  deviceAddress;
} RT_USBDETACH;

/* USBPRM 6.1.2.2: set device configuration */
typedef struct {
   UCHAR  controllerId;
   UCHAR  deviceAddress;
   USHORT classDriverDS;
   UCHAR  configurationValue;
   ULONG  irqSwitchValue;
   UCHAR  category;
   ULONG  setConfiguration;        /* far pointer to a setup packet buffer: not used */
} RT_USBSETCONF;

/* USBPRM 6.1.2.6: register a device class/client driver */
typedef struct {
   ULONG  usbIDC;                  /* far IDC entry point of this driver */
   USHORT usbDS;                   /* its data segment                   */
} RT_USBCLASS;

/* USBPRM 6.1.3.3: accept/reject device */
typedef struct {
   RT_DEVINFO FAR *pDeviceInfo;
   ULONG           pFilter;        /* set on exit when this driver is a filter: not used */
} RT_USBSERVE;

#pragma pack(pop)

/* IDC categories and functions (USBPRM 6.1) */
#define USB_IDC_CATEGORY_HOST      0x90
#define USB_IDC_CATEGORY_USBD      0x91
#define USB_IDC_CATEGORY_CLASS     0x92
#define USB_IDC_CATEGORY_CLIENT    0x93
#define USB_IDC_FUNCTION_ACCIO     0x41
#define USB_IDC_FUNCTION_CANCEL    0x42
#define USB_IDC_FUNCTION_REGISTER  0x43
#define USB_IDC_FUNCTION_PRCIRQ    0x44
#define USB_IDC_FUNCTION_CHKSERV   0x45
#define USB_IDC_FUNCTION_DETDEV    0x46
#define USB_IDC_FUNCTION_SETCONF   0x48
#define USB_IDC_FUNCTION_CLRSTALL  0x4A
#define USB_IDC_FUNCTION_UNREGISTER 0x4F

/* Return codes in the request header status (USBPRM 6.1) */
#define USB_IDC_RC_OK              0x0100
#define USB_IDC_ALLOCERR           0x14
#define USB_IDC_RC_SERVREJCTD      0x0119   /* "device can't be served" - value checked by behaviour, see PROVENANCE */

/* Request flags (USBPRM 6.1.1.1) */
#define USRB_FLAGS_TTYPE_IN        0x0001
#define USRB_FLAGS_TTYPE_OUT       0x0002
#define USRB_FLAGS_TTYPE_SETUP     0x0004
#define USRB_FLAGS_DET_BULK        0x2000
#define USRB_FLAGS_DET_DTGGLEON    0x0800   /* data toggle of the first packet (set/cleared on completion) */

/* --- our target device ------------------------------------------------- */
#define RTL8188EU_VID   0x2357   /* TP-Link Archer T2U Nano (RTL8811AU) */
#define RTL8188EU_PID   0x011E

/* --- USB defaults: endpoint 0, no service interval, 64-byte control packets (USB 2.0 9.6.6), at most 3 retries (USBPRM 6.1.1.1) --- */
#ifndef USB_DEFAULT_CTRL_ENDPT
# define USB_DEFAULT_CTRL_ENDPT   0
#endif
#ifndef USB_DEFAULT_SRV_INTV
# define USB_DEFAULT_SRV_INTV     0
#endif
#ifndef USB_DEFAULT_PKT_SIZE
# define USB_DEFAULT_PKT_SIZE     64
#endif
#ifndef USB_MAX_ERROR_COUNT
# define USB_MAX_ERROR_COUNT      3
#endif

/* Direction bit inside a USB bmRequestType / our helper calls. */
#define USB_DIR_OUT   0            /* host -> device */
#define USB_DIR_IN    1            /* device -> host */

/*
 * Completion record. Because ACCIO is asynchronous (USBD calls our IDC entry on
 * completion), a submit() posts the request and the completion handler flags it
 * done. For Phase 1 register reads we block on a simple flag/yield until done
 * (task-time context); real TX/RX (Phase 3) stays fully async.
 */
typedef struct _usb_xfer {
   volatile int  done;             /* set by the completion handler          */
   UCHAR         status;           /* USRB status byte on completion          */
   USHORT        actual;           /* bytes actually transferred (if avail)   */
   RT_USBRB      rb;               /* the request block (kept alive until done)*/
} usb_xfer_t;

/* Per-adapter USB context (one RTL8188EU dongle). */
typedef struct _usb_dev {
   RT_DEVINFO FAR *pDevInfo;       /* from USBD on attach (in USBD's segment)  */
   UCHAR         ctrlId;           /* cached pDevInfo->ctrlID                  */
   UCHAR         devAddr;          /* cached pDevInfo->deviceAddress           */
   UCHAR         configValue;      /* chosen bConfigurationValue               */
   UCHAR         epBulkIn;         /* bulk IN endpoint id  (from cfg descr)    */
   UCHAR         epBulkOut;        /* bulk OUT endpoint id (from cfg descr)    */
   int           attached;         /* 1 once matched + configured              */
} usb_dev_t;

extern usb_dev_t g_usbdev;         /* single instance for Phase 1             */

/* --- API ---------------------------------------------------------------- */

/* Register our client with USBD. Returns 0 on success. After this, USBD calls
 * rt_idc() on device attach/detach and on transfer completion. */
int  usb_register_client(void);
void usb_unregister_client(void);

/* USBD's IDC entry + DS, resolved at driver init (AttachDD to USBD$). */
extern ULONG        gpUSBDIDC;            /* far entry point */
extern USHORT       gdsUSBDIDC;

/* The one IDC entry point USBD calls back into (attach, detach, completion).
 * Must be FAR: USBD invokes it across segments (matches EXTRN ..:FAR in the
 * device-header asm and the far PUSBIDCEntry type). */
void __far __cdecl rt_idc(RP_GENIOCTL FAR *pRP);

/* Set the active configuration (must precede data transfers). */
int  usb_set_configuration(usb_dev_t *dev, UCHAR configValue);

/* Per-transfer completion wait in ms (default 5000); tests shorten it for scans. */
extern USHORT g_xfer_timeout_ms;
extern USHORT g_stale;           /* stale completions ignored (diagnostic) */

/* Task-time millisecond delay (ProcBlock timeout). Used by init-table settling. */
void usb_mdelay(USHORT ms);

/* ---- asynchronous bulk traffic (data path) ------------------------------------
 * RX: one bulk-IN request is always outstanding; its completion (task time, may
 * not block) feeds rtl_rx_feed() and immediately resubmits. TX: a small pool of
 * slots; usb_tx_submit() posts one frame without waiting, completion frees it. */
#define USB_RXBUF     2048
#define USB_TXSLOTS   8
#define USB_TXBUF     1620
int    usb_rx_start(usb_dev_t *dev, UCHAR ep);
void   usb_rx_stop(void);
extern USHORT g_rx_run, g_rx_err, g_rx_errtot;
USHORT usb_tx_alloc(void);                         /* slot 0..n-1, or 0xFFFF when all busy */
UCHAR *usb_tx_buf(USHORT slot);                    /* the slot's frame buffer (USB_TXBUF)  */
int    usb_tx_submit(usb_dev_t *dev, UCHAR ep, USHORT slot, USHORT len);   /* takes the TX queue lock; -2 = re-entered */
int    usb_tx_submit_locked(usb_dev_t *dev, UCHAR ep, USHORT slot, USHORT len);   /* caller holds usb_txq_lock */
/* TX queue lock: the order of bulk-OUT submissions (data toggle) and of CCMP packet numbers. Interrupts are off on
 * this CPU while it is held. Returns the saved FLAGS, or 0xFFFF when this CPU already holds it (re-entered). */
USHORT usb_txq_lock(void);
void   usb_txq_unlock(USHORT flags);
/* Synchronous transfer lock: g_xfer, the setup packet and the bounce buffer are single instances. Task time only.
 * Held by every rtw_read / rtw_write (rtw_reg.c) around usb_ctrl_msg, and taken by usb_bulk_msg itself. */
void   usb_xfer_lock(void);
void   usb_xfer_unlock(void);
/* An Ethernet frame gathered into a TX slot at this offset needs no copying: the TX descriptor + 802.11 + CCMP +
 * LLC/SNAP headers (40 + 24 + 8 + 8) end exactly where its payload starts (TX_ETH_OFF + 14). */
#define TX_ETH_OFF    66
void   usb_tx_release(USHORT slot);
extern USHORT g_tx_busy, g_tx_err;
void   rtl_rx_feed(UCHAR *buf, USHORT n);          /* implemented in rtl_rx.c (completion context) */

/*
 * One control transfer. Builds the 8-byte setup packet from the args, posts a
 * TTYPE_SETUP USBRB via ACCIO, waits for completion. Returns 0 on success and
 * writes up to len bytes into/out of pData.
 *   bmRequestType: e.g. 0xC0 (vendor IN) / 0x40 (vendor OUT)
 */
int  usb_ctrl_msg(usb_dev_t *dev, UCHAR bmRequestType, UCHAR bRequest,
                  USHORT wValue, USHORT wIndex, void *pData, USHORT len);

/* One bulk transfer on the given endpoint. dir = USB_DIR_IN/OUT. */
int  usb_bulk_msg(usb_dev_t *dev, UCHAR endpoint, int dir,
                  void *pData, USHORT len, USHORT *pActual);

/* --- RTL8188EU register access + Phase 1 probe (rtw_reg.c) ------------- */
UCHAR  rtw_read8 (usb_dev_t *dev, USHORT addr);
USHORT rtw_read16(usb_dev_t *dev, USHORT addr);
ULONG  rtw_read32(usb_dev_t *dev, USHORT addr);
void   rtw_write8 (usb_dev_t *dev, USHORT addr, UCHAR  val);
void   rtw_write16(usb_dev_t *dev, USHORT addr, USHORT val);
void   rtw_write32(usb_dev_t *dev, USHORT addr, ULONG  val);
int    rtw_writeN (usb_dev_t *dev, USHORT addr, USHORT len, UCHAR *pdata);
ULONG  rtl8188eu_read_chip_version(usb_dev_t *dev);
int    rtl8188eu_probe(usb_dev_t *dev);          /* Phase 1 milestone gate  */
int    rtl8188eu_download_fw(usb_dev_t *dev);    /* milestone 4: fwdl.c     */
int    rtl8188eu_init_power_on(usb_dev_t *dev);  /* Phase 2 m2.1: rtl_init.c */
int    rtl8188eu_early_mac(void);                  /* bind-time: power chip + read eFuse MAC into g_mymac (0 = ok) */
int    ndis_set_mac(UCHAR *m);                     /* adopt the chip address into the NDIS tables unless already bound (0 = ok) */

/* DHCP client inside the driver (rtl_dhcp.c): a read of the device runs the exchange */
extern USHORT g_lannum;                  /* rt_ndis.c: TCP/IP interface number (lanN) of this adapter */
void   ndis_find_lan(void);              /* rt_ndis.c: derive g_lannum from PROTOCOL.INI */
void   dhcp_init(void);
void   dbg_init(void);
void   rtl8188eu_dbg_write(ULONG phys, USHORT n);   /* run a command line written to the device */
int    efuse_byte(usb_dev_t *dev, USHORT addr, UCHAR *out);
void   efuse_power(usb_dev_t *dev, int on);
int    dhcp_rx(UCHAR *eth, USHORT len);              /* 1 = frame was a reply to our exchange (consumed) */
USHORT rtl8188eu_dhcp_read(ULONG phys, USHORT max);  /* run DHCP, write the lease text to the caller buffer */
void   ndis_rx_free(USHORT handle);                  /* give back a receive buffer without indicating it */
int    rtl8188eu_init_queue(usb_dev_t *dev);     /* Phase 2 m2.2: rtl_init.c */
int    rtl8188eu_config_mac(usb_dev_t *dev);     /* Phase 2 m2.3: rtl_mac.c  */
int    rtl8188eu_config_bb(usb_dev_t *dev);      /* Phase 2 m2.4: rtl_bb.c   */
int    rtl8188eu_config_rf(usb_dev_t *dev);      /* Phase 2 m2.5: rtl_rf.c   */
/* ---- scan table shared between the receive code (rtl_rx.c) and the GenMAC facade (rtl_nic.c) ---- */
#define MAXNET 24
#define IEMAX  240
typedef struct {
   UCHAR  bssid[6];
   UCHAR  ch;            /* from the DS Parameter IE (else the tuned channel)  */
   UCHAR  sec;           /* bit0 WPA, bit1 WPA2/RSN, bit2 privacy-only (WEP)   */
   UCHAR  len;
   signed char rssi;     /* best dBm seen (stock-driver formulas)               */
   char   ssid[33];
   USHORT cnt;           /* beacons / probe responses seen                     */
   USHORT ielen;         /* bytes valid in ie[]: 12 fixed beacon bytes + IEs   */
   UCHAR  ie[IEMAX];     /* timestamp(8) interval(2) capability(2) + IEs       */
} net_t;
extern net_t  g_net[MAXNET];
extern USHORT g_nnet;
extern UCHAR  g_mymac[6];
extern int    g_hw_ready;      /* chip fully initialised (rtl8188eu_hw_init ok)  */
extern int    g_hw_failed;     /* init failed; needs a re-plug before retrying   */

/* Diagnostics: compiled in, run once from the open handler when enabled. */
#define RUN_EFUSE_MAC 1   /* read the factory MAC from the eFuse at init (stock byte sequence, cold chip state) */
#define TRACE_DATA 0      /* 1 = per-frame serial trace (T k f d x r + counter dump): costs ~5 ms per frame, debugging only */
#define RUN_TXPOWER 0     /* 1 = per-rate TX power from the eFuse/Windows offsets (measured WORSE at close range: off) */
#define RUN_LED    1      /* drive the dongle LED: slow blink while searching, steady on when joined, flicker on traffic */
#define DBG_ADMIN  0      /* 1 = debug channel may write chip registers and force a re-init (development only; 0 for releases) */
#define SHOW_RSSI  1      /* print RSSI in the COM1 network list */
void   rtl8188eu_iqk_set(usb_dev_t *dev, int on);
int    rtl8188eu_llt_init(usb_dev_t *dev);       /* LLT only (rtl_rx.c)      */
int    rtl8188eu_rx_enable(usb_dev_t *dev);      /* Phase 2 m2.6a: rtl_rx.c  */
int    rtl8188eu_iqk(usb_dev_t *dev);            /* Phase 2 m2.7: rtl_iqk.c  */
void   rtl8188eu_tx_prepare(usb_dev_t *dev);     /* TX queue map, MAC address, un-pause    */
void   rtl8188eu_scan_once(usb_dev_t *dev);      /* one pass over channels 1..13 -> g_net  */

/* join state + EAPOL path (rtl_rx.c); joins the SSID requested through the OID interface */
extern UCHAR  g_joined, g_jch, g_jslen;
extern UCHAR  g_jbssid[6], g_jssid[33];
extern USHORT g_jrescap, g_jstatus, g_jaidv;
extern UCHAR  g_jreq[96], g_jres[128];
extern USHORT g_jreqlen, g_jreslen;
void   rtl8188eu_set_want_ssid(UCHAR FAR *s, UCHAR len);   /* SSID to join, as requested by the supplicant/monitor */
int    rtl8188eu_join(usb_dev_t *dev, UCHAR *want_bssid);   /* NULL = best BSS of the scan table */
void   rtl8188eu_rx_engine_start(usb_dev_t *dev);        /* always-on async bulk-IN receive loop */
void   rtl8188eu_dbg(void);                              /* counter dump on COM1 */
void   rtl8188eu_print_nets(void);                      /* debug: dump the scan table (with RSSI) on COM1 */
UCHAR  rtl8188eu_cur_rate(void);                       /* debug: current data rate code */
void   rtl8188eu_set_txpower(usb_dev_t *dev, UCHAR channel);   /* per-rate TX power from the eFuse (rtw_reg.c) */
signed char rtl8188eu_rssi_of(UCHAR *bssid);          /* smoothed dBm of a BSS from the scan table, 0 = unknown */
void   rtl8188eu_leave(usb_dev_t *dev);
USHORT rtl8188eu_poll(usb_dev_t *dev, USHORT tmo_ms);       /* returns EAPOL frames waiting      */
USHORT rtl8188eu_eapol_get(UCHAR FAR *out, USHORT room);
int    rtl8188eu_eapol_tx(usb_dev_t *dev, UCHAR FAR *eth, USHORT len);
int    rtl8188eu_eth_tx(usb_dev_t *dev, UCHAR FAR *eth, USHORT len);
int    rtl8188eu_ndis_tx(UCHAR *eth, USHORT len);   /* async encrypted data TX (copies eth into a slot) */
int    rtl8188eu_ndis_tx_slot(USHORT slot, USHORT len);   /* same, Ethernet frame already at TX_ETH_OFF in the slot */
USHORT rtl8188eu_ndis_slot(void);                     /* a free TX slot for it (waits briefly), 0xFFFF = none */
int    rtl8188eu_tx_ready(void);                     /* joined with a pairwise key: data frames can be sent */
extern UCHAR g_ctlrate;                  /* rtl_rx.c */
void   usb_tx_reset(void);                      /* rt_usb.c: free all TX slots (device detached) */
ULONG  rtl8188eu_ms(void);                      /* rtl_rx.c: milliseconds since boot */
USHORT rtl8188eu_diag(USHORT i);                /* rtl_rx.c: diagnostic value i (0xFFFF past the end) */
void   rtl8188eu_diag_set(USHORT reason, USHORT a, USHORT b);
USHORT rtl8188eu_diag_ctx(USHORT k, USHORT i);
USHORT usb_ep_info(USHORT what);                  /* rt_usb.c: controller/port/max packet sizes for the diagnostics */
USHORT rtl8188eu_diag_reg(USHORT n, USHORT half);   /* rtl_rx.c: chip register n (see rtl_dhcp.c) */
USHORT rtl8188eu_iqk_info(USHORT i);               /* rtl_iqk.c: IQ calibration outcome */
UCHAR  rtl8188eu_diag_tm(USHORT n, USHORT i);
USHORT usb_phys(USHORT i, USHORT half);               /* rt_usb.c: physical address of a DMA buffer end */
extern USHORT g_dump_base;                       /* rtl_dhcp.c: register window to list on the next read (0xFFFF = none) */
USHORT rtl8188eu_diag_ev(USHORT n, USHORT which);
USHORT rtl8188eu_diag_ul(USHORT n, USHORT which);   /* log of unicast frames addressed to us */
USHORT rtl8188eu_diag_ul_n(void);
void   rtl8188eu_dbg_probe(USHORT count, USHORT pad);   /* rtl_rx.c, debug command "pr" */
#define DIAG_COUNT 153
void   rtl8188eu_on_detach(void);               /* rtl_rx.c: forget link, keys and queues */
void   rtl8188eu_nic_lock(void);                 /* rtl_nic.c: the request lock of the OID interface */
void   rtl8188eu_nic_unlock(void);
void   rtl8188eu_nic_detach(void);              /* rtl_nic.c: DETDEV handler (non-blocking) */
int    rtl8188eu_card_disable(usb_dev_t *dev);  /* rtl_init.c: power the chip down (cold-state reset) */
void   rtl8188eu_led_off(usb_dev_t *dev);       /* shutdown: LED off, stays off */
void   rtl8188eu_shutdown(void);                /* rtl_nic.c: CMDShutdown handler */
void   rtl8188eu_led_tick(usb_dev_t *dev);      /* call from task time under the request lock */
void   rtl8188eu_dig_tick(usb_dev_t *dev);      /* receiver gain from the signal strength: same call rules */
extern UCHAR g_dig_auto;                        /* rtl_rx.c: 0 = automatic receiver gain off (debug "dig 0") */
int    rtl8188eu_has_ptk(void);                 /* pairwise key installed on the current join */
int    rtl8188eu_key_add(usb_dev_t *dev, UCHAR idx, UCHAR pairwise, UCHAR *key);   /* key = 16-byte CCMP TK */
void   rtl8188eu_key_remove(usb_dev_t *dev, UCHAR idx, UCHAR pairwise);
int    rtl8188eu_hw_init(usb_dev_t *dev);        /* rtw_reg.c: full chip bring-up, once     */
void   rtl8188eu_on_open(void);                  /* rtl_nic.c: CMDOpen                      */
void   rtl8188eu_genmac_ioctl(RP_GENIOCTL FAR *rp);  /* rtl_nic.c: CMDGenIOCTL (category 0x99)  */

/* RTL8821A chip layer (rtl8821_hal.c) */
int    rtl8821_fw_load(void);                     /* INIT time: allocate the firmware buffer */
int    rtl8821_fw_write(ULONG phys, USHORT n);    /* task time: 1 when this write was the firmware image */
int    rtl8821_hw_init(usb_dev_t *dev);           /* reset / power-on / LLT / firmware download */
extern USHORT g_r21_stage, g_r21_pwr_fail, g_r21_llt_fail, g_r21_fwload, g_r21_fwlen, g_r21_fwsig, g_r21_fwtries;
extern USHORT g_r21_chksum, g_r21_ready;
extern ULONG  g_r21_mcufwdl, g_r21_cfg;
extern USHORT g_r21_tried;
int    rtl8821_phy_init(usb_dev_t *dev);          /* rtl8821_phy.c: MAC / BB / RF init after the firmware runs */
void   rtl8821_set_channel(usb_dev_t *dev, UCHAR ch);
void   rtl8821_rx_enable(usb_dev_t *dev);
int    rtl8821_efuse_load(usb_dev_t *dev);        /* rtl8821_efuse.c */
int    rtl8821_early_power(usb_dev_t *dev);       /* rtl8821_hal.c: MAC power-on only */
int    rtl8821_tx_mgmt(usb_dev_t *dev, UCHAR *f, USHORT n, int bcast, UCHAR rate, int cck);   /* rtl8821_tx.c */
int    rtl8821_tx_beacon(usb_dev_t *dev, UCHAR ch);
void   rtl8821_fill_desc(UCHAR *b, USHORT n, int bcast, UCHAR qsel, int sec, UCHAR rate, int rpt);   /* 40 byte TX descriptor */
#define TXD_LEN       40       /* bytes of TX descriptor in front of every frame */
#define TXEP_MGT      9        /* bulk-OUT endpoint of the management queue (extra) */
#define TXEP_DATA     8        /* bulk-OUT endpoint of the best-effort queue (low) */
extern USHORT g_r21_txok, g_r21_txfail, g_r21_txlast, g_r21_channel;
void   rtl8821_lck(usb_dev_t *dev);
extern UCHAR  g_r21_ef[512], g_r21_ef_ok, g_r21_xtal, g_r21_lna2g, g_r21_pa2g, g_r21_rfe, g_r21_thermal;
extern USHORT g_r21_ef_used, g_r21_hold, g_r21_norx;
extern USHORT g_u_acc, g_u_done, g_u_stat, g_u_wait;
extern USHORT g_rc_n, g_rc_avg8, g_rc_max, g_rc_slow, g_if_on, g_if_off, g_ind_cli;
extern USHORT g_rel_n, g_rel_avg8, g_rel_max, g_rc_wait, g_rc_succ, g_rc_other;
extern USHORT g_use_hook, g_hk_run, g_hk_maxq, g_hk_arm_fail, g_rxgap[5];
extern USHORT g_txlat_avg8, g_txlat_max, g_txlat_n, g_ind_drop_busy, g_ind_drop_off;
int    efuse_byte(usb_dev_t *dev, USHORT addr, UCHAR *out);
void   efuse_power(usb_dev_t *dev, int on);
ULONG  rtl8821_rf_read(usb_dev_t *dev, UCHAR reg);
void   rtl8821_rf_write(usb_dev_t *dev, UCHAR reg, ULONG data);
void   rtl8188eu_rx_engine_start(usb_dev_t *dev);
extern USHORT g_r21_phy_stage, g_r21_channel;
extern ULONG  g_r21_chk[8];

/* The resident-end markers (rt_code_end / rt_data_end) are in rt_end.asm, linked last. */

/* Boot/attach trace: tcom_always() (rt_lib.asm) writes one raw byte to COM1 (0x3F8).
 * Used to localize hangs/traps via the VirtualBox raw-file COM capture. */
#define TRACE 0                              /* 1 = verbose COM1 boot/debug trace; 0 = quiet release build  */
#if TRACE
#define tcom(c) tcom_always(c)
#else
#define tcom(c) ((void)(c))                  /* trace compiled out (argument still evaluated)               */
#endif

#endif /* OS2_USB_H */
