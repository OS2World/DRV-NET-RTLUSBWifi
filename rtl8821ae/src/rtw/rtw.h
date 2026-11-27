/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtw.h - the Realtek RTL8821AE (PCIe) back end: registers, descriptors and the interfaces between its files.
 *
 * The chip is the RTL8821A of the RTL8821AU / RTL8811AU USB dongles (rtl8821au/ of this project) behind a PCIe
 * function instead of USB. The chip layer (power, LLT, firmware download, MAC / BB / RF tables, band and channel,
 * transmit power, eFuse, keys) is ported from rtl8821au/src; the PCIe parts (power sequence for the PCIe interface,
 * the DMA descriptor rings, the interrupt registers) follow the Linux rtlwifi driver rtl8821ae (drivers/net/wireless/
 * realtek/rtlwifi/: pci.c, rtl8821ae/hw.c, trx.c, trx.h, reg.h, pwrseq.h; Copyright(c) 2009-2010 Realtek
 * Corporation, GPL-2.0).
 *
 * Files:  rtw_io.c   register access, BB / RF access, firmware commands (H2C)
 *         rtw_hal.c  device table, power-on, LLT, PCIe MAC setup, firmware download, hw_up / hw_down, status
 *         rtw_phy.c  MAC / BB / RF tables, band, channel, transmit power, calibration, receiver gain, eFuse
 *         rtw_trx.c  DMA rings, transmit, receive, interrupt, service path
 *         rtw_sta.c  the station operations of hw.h: scan, join, leave, keys, rates
 *         rtw_tbl.c  the register tables (tools/gen_tbl.py, PCIe interface)
 */
#ifndef RTW_H
#define RTW_H

/* ---- MAC registers (rtlwifi rtl8821ae/reg.h) ---------------------------------------------------------- */
#define REG_SYS_FUNC_EN      0x0002
#define REG_APS_FSMCO        0x0004
#define REG_SYS_CLKR         0x0008
#define REG_RSV_CTRL         0x001C
#define REG_RF_CTRL          0x001F
#define REG_CRYSTAL          0x002C          /* REG_MAC_PHY_CTRL: crystal capacitor in bits 23:12                */
#define REG_EFUSE_CTRL       0x0030
#define REG_GPIO_MUXCFG      0x0040
#define REG_LEDCFG2          0x004E
#define REG_HSIMR            0x0058
#define REG_HSISR            0x005C
#define REG_OPT_CTRL         0x0074
#define REG_MCUFWDL          0x0080
#define REG_HIMR             0x00B0
#define REG_HISR             0x00B4
#define REG_HIMRE            0x00B8
#define REG_HISRE            0x00BC
#define REG_EFUSE_ACCESS     0x00CF
#define REG_SYS_CFG          0x00F0
#define REG_CR               0x0100
#define REG_MSR              0x0102
#define REG_PBP              0x0104
#define REG_TRXDMA_CTRL      0x010C
#define REG_TRXFF_BNDY       0x0114
#define REG_FWIMR            0x0130
#define REG_C2HEVT_MSG       0x01A0
#define REG_C2HEVT_CLEAR     0x01AF
#define REG_MCUTST_1         0x01C0
#define REG_HMETFR           0x01CC
#define REG_HMEBOX_0         0x01D0
#define REG_LLT_INIT         0x01E0
#define REG_HMEBOX_EXT0      0x01F0
#define REG_RQPN             0x0200
#define REG_TDECTRL          0x0208
#define REG_RQPN_NPQ         0x0214
#define REG_RXDMA_CONTROL    0x0286
#define REG_PCIE_CTRL_REG    0x0300          /* TX doorbell (word, bit = queue), +1: DMA stop, +3: 0x77          */
#define REG_INT_MIG          0x0304
#define REG_BCNQ_DESA        0x0308
#define REG_HQ_DESA          0x0310
#define REG_MGQ_DESA         0x0318
#define REG_VOQ_DESA         0x0320
#define REG_VIQ_DESA         0x0328
#define REG_BEQ_DESA         0x0330
#define REG_BKQ_DESA         0x0338
#define REG_RX_DESA          0x0340
#define REG_FWHW_TXQ_CTRL    0x0420
#define REG_HWSEQ_CTRL       0x0423
#define REG_BCNQ_BDNY        0x0424
#define REG_MGQ_BDNY         0x0425
#define REG_RL               0x042A
#define REG_DARFRC           0x0430
#define REG_RARFRC           0x0438
#define REG_RRSR             0x0440
#define REG_ARFR0            0x0444
#define REG_ARFR1            0x044C
#define REG_CCK_CHECK        0x0454
#define REG_AMPDU_MAX_TIME   0x0456
#define REG_AMPDU_MAX_LENGTH 0x0458
#define REG_FAST_EDCA_CTRL   0x0460
#define REG_DATA_SC          0x0483
#define REG_ARFR2            0x048C
#define REG_ARFR3            0x0494
#define REG_HT_SINGLE_AMPDU  0x04C7
#define REG_MAX_AGGR_NUM     0x04CA
#define REG_BAR_MODE_CTRL    0x04CC
#define REG_EDCA_VO_PARAM    0x0500
#define REG_EDCA_VI_PARAM    0x0504
#define REG_EDCA_BE_PARAM    0x0508
#define REG_EDCA_BK_PARAM    0x050C
#define REG_TXPAUSE          0x0522
#define REG_BCNTCFG          0x0510
#define REG_TBTT_PROHIBIT    0x0540
#define REG_NAV_PROT_LEN     0x0546
#define REG_BCN_CTRL         0x0550
#define REG_BCN_INTERVAL     0x0554
#define REG_SECONDARY_CCA    0x0577
#define REG_RCR              0x0608
#define REG_RX_PKT_LIMIT     0x060C
#define REG_RX_DRVINFO_SZ    0x060F
#define REG_MACID            0x0610
#define REG_BSSID            0x0618
#define REG_MAR              0x0620
#define REG_NAV_UPPER        0x0652
#define REG_WMAC_TRXPTCL_CTL 0x0668
#define REG_CAMCMD           0x0670
#define REG_CAMWRITE         0x0674
#define REG_SECCFG           0x0680
#define REG_RXFLTMAP0        0x06A0
#define REG_RXFLTMAP1        0x06A2
#define REG_RXFLTMAP2        0x06A4

/* receive configuration: the values the RTL8821AU driver runs with (rtl8821au/src/rtl_rx.c) */
#define RCR_SCAN             0x1000280FUL    /* AAP|APM|AM|AB + ADF + AMF + APP_PHYST_RXFF: every network          */
#define RCR_LINKED           0x100028CEUL    /* APM|AM|AB|CBSSID_DATA|CBSSID_BCN + ADF + AMF + APP_PHYST_RXFF      */

/* host interrupt bits (REG_HIMR / REG_HISR) */
#define IMR_ROK              0x00000001UL
#define IMR_RDU              0x00000002UL
#define IMR_VODOK            0x00000004UL
#define IMR_VIDOK            0x00000008UL
#define IMR_BEDOK            0x00000010UL
#define IMR_BKDOK            0x00000020UL
#define IMR_MGNTDOK          0x00000040UL
#define IMR_HIGHDOK          0x00000080UL
#define IMR_C2HCMD           0x00000400UL
/* REG_HIMRE / REG_HISRE */
#define IMRE_RXFOVW          0x00000100UL
#define IMRE_TXFOVW          0x00000200UL

/* ---- DMA descriptors (rtlwifi rtl8821ae/trx.h, pci.h) --------------------------------------------------
 * TX: a ring of 64-byte entries (struct rtl_tx_desc, 16 dwords); the first 40 bytes are the Jaguar TX descriptor the
 * USB driver puts in front of each frame, dw7 holds the buffer length, dw10 the buffer address, dw12 the address of
 * the next entry. The chip follows the chain from REG_xxQ_DESA, takes every entry whose OWN bit (dw0 bit 31) is set
 * and clears it when the frame is sent; a write of the queue's bit to REG_PCIE_CTRL_REG starts it.
 * RX: a ring of 32-byte entries (struct rtl_rx_desc, 8 dwords): dw0 = OWN | EOR (last entry) | buffer size, dw6 =
 * buffer address. When the chip gives an entry back, dw0..dw5 are the 24-byte RX status the USB driver parses
 * (rtl_rx.c rx_parse) and the buffer holds [PHY status (drvinfo)][shift][802.11 frame]. */
#define TXD_SIZE             64
#define TXD_HDR              40              /* the Jaguar TX descriptor proper (the OFFSET field)                */
#define RXD_SIZE             32
#define TXD_OWN              0x80000000UL

/* queue select values of the TX descriptor (def.h) and the doorbell bits of REG_PCIE_CTRL_REG (pci.h queue ids) */
#define QSLT_BE              0x00
#define QSLT_MGNT            0x12
#define TXQ_BIT_BE           0x0002
#define TXQ_BIT_MGNT         0x0040

/* hardware rate codes (DESC_RATE*) */
#define RATE_1M              0x00
#define RATE_6M              0x04
#define RATE_MCS0            0x0C

/* ---- rtw_io.c ------------------------------------------------------------------------------------------ */
#define rtw_r8(a)        mmio_rd8((ULONG)(USHORT)(a))
#define rtw_r16(a)       mmio_rd16((ULONG)(USHORT)(a))
#define rtw_r32(a)       mmio_rd((ULONG)(USHORT)(a))
#define rtw_w8(a, v)     mmio_wr8((ULONG)(USHORT)(a), (UCHAR)(v))
#define rtw_w16(a, v)    mmio_wr16((ULONG)(USHORT)(a), (USHORT)(v))
#define rtw_w32(a, v)    mmio_wr((ULONG)(USHORT)(a), (ULONG)(v))
void   rtw_mdelay(USHORT ms);                   /* task time: ProcBlock; never at interrupt time              */
ULONG  shl1(ULONG v);
ULONG  shr1(ULONG v);
void   bb_set(USHORT reg, ULONG mask, ULONG data);   /* PHY_SetBBReg: data into the bits of mask (read-modify-write) */
ULONG  bb_get(USHORT reg, ULONG mask);
void   rf_write(UCHAR reg, ULONG data);         /* RF path A, 20-bit data                                     */
ULONG  rf_read(UCHAR reg);
int    rtw_h2c(UCHAR id, UCHAR b1, UCHAR b2, UCHAR b3);   /* firmware command: 0 = written to a free box       */
extern UCHAR  g_h2c_box;
extern USHORT g_h2c_busy;

/* ---- rtw_hal.c ----------------------------------------------------------------------------------------- */
extern USHORT g_rtw_step;                       /* bring-up step reached (also g_hw_state)                    */
extern USHORT g_pwr_fail, g_llt_fail, g_fw_tries, g_fw_chksum, g_fw_ready, g_fw_ver, g_fw_sub;
extern ULONG  g_mcufwdl;
extern UCHAR  g_cut;                            /* chip cut from REG_SYS_CFG (0 = A)                          */

/* ---- rtw_phy.c ----------------------------------------------------------------------------------------- */
extern UCHAR  g_ef[512];                        /* logical eFuse map                                          */
extern UCHAR  g_ef_ok, g_xtal, g_lna2g, g_pa2g, g_lna5g, g_pa5g, g_rfe, g_thermal;
extern USHORT g_ef_used;
extern USHORT g_channel;                        /* channel currently set (0 = none)                           */
extern UCHAR  g_band;                           /* 0 = 2.4 GHz, 1 = 5 GHz, 2 = not set                        */
extern UCHAR  g_ext_bsw;                        /* 1 = drive the external antenna band switch (debug "bsw")   */
extern ULONG  g_chk[8];
int    rtw_efuse_load(void);
void   rtw_mac_tables(void);                    /* MAC register table                                         */
void   rtw_bb_init(void);                       /* function enable, PHY_REG + AGC tables, crystal             */
void   rtw_rf_init(void);
void   rtw_band_2g(void);
void   rtw_set_channel(UCHAR ch);
void   rtw_bw20(void);
void   rtw_lck(void);
void   rtw_iqk(UCHAR ch);                       /* firmware IQ calibration (H2C 0x45), waits for its report   */
extern UCHAR  g_iqk_mode;                       /* 0 off, 1 5 GHz joins, 2 every join                         */
extern volatile UCHAR g_iqk_done;
extern UCHAR  g_iqk_c2h;
extern USHORT g_iqk_sent, g_iqk_ok, g_iqk_tmo, g_iqk_ms;
void   rtw_txpower_set(UCHAR on);
extern UCHAR  g_txp5, g_txp_ofdm, g_txp_ht, g_txp_grp;
void   rtw_dig_set(UCHAR v);
void   rtw_dig_tick(void);                      /* receiver gain from the signal strength (request path)      */
extern UCHAR  g_dig_auto, g_igi;

/* ---- rtw_trx.c ----------------------------------------------------------------------------------------- */
int    rtw_trx_alloc(void);                     /* the rings and buffers (task time, once)                    */
void   rtw_trx_init(void);                      /* rings back to the start (every bring-up)                   */
void   rtw_trx_desa(void);                      /* ring addresses into the chip                               */
void   rtw_irq_enable(void);
void   rtw_irq_disable(void);
int    rtw_tx_frame(UCHAR qsel, UCHAR FAR *h, USHORT hl, UCHAR FAR *iv, UCHAR FAR *b, USHORT bl,
                    UCHAR rate, USHORT flags);   /* 0 queued, -2 ring full, -1 bad                             */
#define TXF_BCAST    0x0001
#define TXF_SEC      0x0002                     /* hardware CCMP with the pairwise key                        */
#define TXF_RPT      0x0004                     /* ask for a TX report (C2H)                                  */
extern USHORT g_trx_on;
extern USHORT g_rx_frames, g_rx_crc, g_rx_c2h, g_rx_rdu, g_rx_big, g_rx_fovw, g_tx_ok[2], g_tx_full[2], g_tx_fovw;
extern USHORT g_svc_runs, g_isr_last, g_c2h_reg;
extern ULONG  g_hisr_last;
extern UCHAR  g_rx_rate;                        /* rate code of the last data frame from the AP               */
void   rtw_c2h(UCHAR FAR *c, USHORT n);         /* a firmware report (rtw_phy.c, rtw_sta.c)                   */
USHORT rtw_trx_state(USHORT what);              /* ring positions for the status text                         */

/* ---- rtw_sta.c ----------------------------------------------------------------------------------------- */
extern UCHAR  g_curch;                          /* the channel the receiver is on (scan / join)               */
extern UCHAR  g_ctlrate, g_txrate, g_txrate_auto;
extern USHORT g_keys_p, g_keys_g, g_keys_dup, g_scan_n;
void   rtw_sta_reset(void);                     /* bring-up: no link, no keys                                 */
void   rtw_keys_reset(UCHAR secure);
UCHAR  rtw_rate_for(UCHAR FAR *body, USHORT blen, USHORT flags);
void   rtw_link_tick(void);                     /* service path, once a second: beacon loss, receiver gain    */

/* ---- rtw_tbl.c ----------------------------------------------------------------------------------------- */
typedef struct { USHORT a; UCHAR v; } rtw_mac_t;   /* MAC register, byte write                               */
typedef struct { USHORT a; ULONG v; } rtw_bb_t;    /* baseband register (PHY_REG and AGC), dword write       */
typedef struct { UCHAR  a; ULONG v; } rtw_rf_t;    /* RF path A register, 20 bit data                        */
extern const rtw_mac_t g_mac_tbl[];
extern const USHORT    g_mac_n;
extern const rtw_bb_t  g_bb_tbl[];
extern const USHORT    g_bb_n;
extern const rtw_bb_t  g_agc_tbl[];
extern const USHORT    g_agc_n;
extern const rtw_rf_t  g_rf_tbl[];
extern const USHORT    g_rf_n;

#endif
