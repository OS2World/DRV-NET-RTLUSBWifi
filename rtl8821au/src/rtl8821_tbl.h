/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * rtl8821_tbl.h - types and sizes of the flattened RTL8821A MAC / BB / AGC / RF tables (rtl8821_tbl.c).
 */
#ifndef RTL8821_TBL_H
#define RTL8821_TBL_H

typedef struct { USHORT a; UCHAR v; }  r21_mac_t;      /* MAC register, byte write */
typedef struct { USHORT a; ULONG v; }  r21_bb_t;       /* baseband register (PHY_REG and AGC), dword write */
typedef struct { UCHAR  a; ULONG v; }  r21_rf_t;       /* RF path A register, 20 bit data */

extern const r21_mac_t g_r21_mac_tbl[];
extern const USHORT    g_r21_mac_n;
extern const r21_bb_t  g_r21_bb_tbl[];
extern const USHORT    g_r21_bb_n;
extern const r21_bb_t  g_r21_agc_tbl[];
extern const USHORT    g_r21_agc_n;
extern const r21_rf_t  g_r21_rf_tbl[];
extern const USHORT    g_r21_rf_n;

#endif
