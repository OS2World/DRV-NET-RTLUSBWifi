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
 * rt_ndis.h - NDIS 2.0.1 MAC definitions for the RTL8188EU driver.
 *
 * The driver is BOTH the GenMAC-style OID device (rtl_nic.c) used by wpa_supplicant AND an NDIS 2 MAC
 * (rt_ndis.c) that the protocol stack (TCP/IP) binds to through the Protocol Manager.
 *
 * Written from the NDIS 2.0.1 specification (3Com/Microsoft, 1990), which may be implemented freely:
 * characteristics tables, the MAC and protocol dispatch tables, the system request and bind calls,
 * return codes and the Protocol Manager interface. See v012/PROVENANCE.md, section 6.
 */
#ifndef RT_NDIS_H
#define RT_NDIS_H

#include "rt_os2dd.h"

typedef unsigned short WORD;
typedef unsigned long  DWORD;
typedef unsigned char  BYTE;
typedef BYTE FAR      *PBYTE;

#define NDIS_MODULE_NAME      "RTL88EU_NIF"   /* = PROTOCOL.INI section [RTL88EU_NIF] */
#define MAC_TYPE_NAME         "DIX+802.3"
#define STATION_ADDR_LEN      6
#define STATION_ADDR_MC       1
#define MAX_FRAME_SIZE        1514
#define MAX_LINK_SPEED        11000000UL      /* nominal */
#define MAX_MC_ADDRS          16

#define MAJOR_NDIS_VERSION    2
#define MINOR_NDIS_VERSION    0
#define MAJOR_DRV_VERSION     0
#define MINOR_DRV_VERSION     1

/* Common characteristics: module function flags */
#define CCTFLAG_BINDING_UPPER 1

/* MAC service-specific characteristics: service flags */
#define SCTFLAG_BROADCAST            0x0001UL
#define SCTFLAG_MULTICAST            0x0002UL
#define SCTFLAG_SET_STN_ADDR         0x0010UL
#define SCTFLAG_STAT_ALWAYS_CURR     0x0020UL
#define SCTFLAG_RECEIVE_CHAIN        0x0100UL

/* MAC status (service-specific status table) */
#define SSTATUS_HW_OK         7
#define SSTATUS_BOUND         8
#define SSTATUS_OPEN          0x10

/* packet filter bits */
#define PACK_FILTER_DIRECT_MC   1
#define PACK_FILTER_BROADCAST   2
#define PACK_FILTER_PROMISCS    4
#define PACK_FILTER_ROUTING     8
#define PACK_FILTER_RESERVED    0xFFF0

#define UPPER_PROTOCOL_MAC    1
#define UPPER_INTERFACE_MAC   1
#define LOWER_PROTOCOL_USB    0               /* physical */
#define LOWER_INTERFACE_USB   0

/* system request opcodes and MAC request opcodes */
#define SYSTEM_REQUEST_BIND        2
#define INITIATE_DIAGNOSTICS       1
#define READ_ERROR_LOG             2
#define SET_STATION_ADDRESS        3
#define OPEN_ADAPTER               4
#define CLOSE_ADAPTER              5
#define RESET_MAC                  6
#define SET_PACKET_FILTER          7
#define ADD_MULTICAST_ADDRESS      8
#define DELETE_MULTICAST_ADDRESS   9
#define UPDATE_STATISTICS         10
#define CLEAR_STATISTICS          11
#define INTERRUPT_REQUEST         12
#define SET_FUNCTIONAL_ADDRESS    13
#define SET_LOOK_AHEAD            14

/* NDIS return codes */
#define NDIS_SUCCESS             0
#define NDIS_WAIT_FOR_RELEASE    1
#define NDIS_REQUEST_QUEUED      2
#define NDIS_OUT_OF_RESOURCE     6
#define NDIS_INVALID_PARAMETER   7
#define NDIS_INVALID_FUNCTION    8
#define NDIS_NOT_SUPPORTED       9
#define NDIS_GENERAL_FAILURE     0xFF
#define NDIS_ALREADY_STARTED     0x20
#define NDIS_DRIVER_NOT_INIT     0x22

/* Protocol Manager: device PROTMAN$, IOCtl category 81h, function 58h; opcode 2 = RegisterModule */
#define IOCTL_CAT_LAN_MANAGER    0x81
#define IOCTL_FUN_PM             0x58
#define PM_OP_GET_PM_INFO        1
#define PM_OP_REGISTER_MODULE    2

typedef void (__far __cdecl *PFN)(void);

typedef struct CommonChars   FAR *PCCT;
typedef struct MACSpecChars  FAR *PSCT;
typedef struct MCAddrList    FAR *PMCAL;
typedef struct MACSpecStatus FAR *PSST;
typedef struct MACUpperDisp  FAR *PUDT;
typedef struct ProtLowerDisp FAR *PLDT;
typedef struct TCBufferD     FAR *PTCBD;
typedef struct TDBufferD     FAR *PTDBD;
typedef struct RCBufferD     FAR *PRCBD;

#pragma pack(push, 1)

/* Common characteristics table */
struct CommonChars {
   WORD   size;
   BYTE   majorNDISv, minorNDISv;
   WORD   reserved;
   BYTE   majorV, minorV;
   DWORD  flags;
   BYTE   name[16];
   BYTE   protocolUpp, interfaceUpp, protocolLow, interfaceLow;
   WORD   moduleID;
   WORD   moduleDS;
   PFN    pSysReq;
   PSCT   pSpecChars;
   PSST   pSpecStatus;
   PUDT   pUppDisp;
   PLDT   pLowDisp;
   DWORD  pReserved1;
   DWORD  pReserved2;
};

/* Multicast address list */
struct MCAddrList {
   WORD   maxMCAddrs;
   WORD   currMCAddrs;
   BYTE   mcAddr[MAX_MC_ADDRS][16];
};

/* MAC service-specific characteristics */
struct MACSpecChars {
   WORD   length;
   BYTE   typeName[16];
   WORD   lenStnAddrs;
   BYTE   permStnAddr[16];
   BYTE   currStnAddr[16];
   DWORD  currFncAddr;
   PMCAL  pMCAddrList;
   DWORD  linkSpeed;
   DWORD  flags;
   WORD   maxFrameSz;
   DWORD  trBuffCap;
   WORD   trBlockSz;
   DWORD  recBuffCap;
   WORD   recBlockSz;
   BYTE   vendorCode[3];
   BYTE   vendAdCode;
   PSZ    pVendAdDescr;
   WORD   irq;
   WORD   trQDepth;
   WORD   maxDataBlocks;
};

/* MAC service-specific status table, followed by driver-private state outside the NDIS table */
struct MACSpecStatus {
   WORD   length;
   DWORD  diagDT;
   DWORD  status;
   WORD   filter;
   DWORD  pSpecStats;
   DWORD  clearDT;
   DWORD  recFrames;
   DWORD  recFrCRC;
   DWORD  recBytes;
   DWORD  recFrNoBuff;
   DWORD  recMCFrames;
   DWORD  recBCFrames;
   DWORD  reserved1, reserved2, reserved3, reserved4, reserved5;
   DWORD  recFrHW;
   DWORD  trFrames;
   DWORD  trBytes;
   DWORD  trMCFrames;
   DWORD  trBCFrames;
   DWORD  reserved6, reserved7;
   DWORD  trFrTO;
   DWORD  trFrHW;
   /* driver private (outside the NDIS table length) */
   WORD   flags;
   BYTE   indiCount;
};

/* MAC upper dispatch table */
struct MACUpperDisp {
   PCCT   pCommChars;
   PFN    pGenReq;
   PFN    pTransmitChain;
   PFN    pTransferData;
   PFN    pReceiveRelease;
   PFN    pIndicationOn;
   PFN    pIndicationOff;
};

/* Protocol lower dispatch table (the bound protocol's; its entries are Pascal far functions) */
struct ProtLowerDisp {
   PCCT   pCommChars;
   DWORD  interfaceFlags;
   WORD (__far __pascal *pGenReqConfirm)(WORD protID, WORD macID, WORD reqHandle,
                                         WORD status, WORD opcode, WORD protDS);
   WORD (__far __pascal *pTransmitConfirm)(WORD protID, WORD macID, WORD reqHandle,
                                           WORD status, WORD protDS);
   WORD (__far __pascal *pReceiveLookahead)(WORD macID, WORD frameSize, WORD bytesAvail,
                                            PBYTE pBuffer, PBYTE pIndicate, WORD protDS);
   WORD (__far __pascal *pIndicationComplete)(WORD macID, WORD protDS);
   WORD (__far __pascal *pReceiveChain)(WORD macID, WORD frameSize, WORD reqHandle,
                                        PRCBD pRCBufferD, PBYTE pIndicate, WORD protDS);
   WORD (__far __pascal *pStatusIndication)(WORD macID, WORD param1, PBYTE pIndicate,
                                            WORD opcode, WORD protDS);
};

/* Protocol Manager request block, 14 bytes */
struct PMReqBlock {
   USHORT opcode;
   USHORT status;
   PVOID  pointer1;
   PVOID  pointer2;
   USHORT word1;
};

/* Transmit buffer descriptor (data block pointer type 0 = physical, 2 = GDT) and receive descriptor */
#define MAX_DATA_BLOCK_COUNT  8
#define MAX_IMMED_LEN         64

struct TDataBlock {
   BYTE   ptrType;
   BYTE   resByte;
   WORD   dataLen;
   PBYTE  pData;
};
struct TCBufferD {
   WORD   immedLen;
   PBYTE  pImmed;
   WORD   dbCount;
   struct TDataBlock db[MAX_DATA_BLOCK_COUNT];
};
struct RCDataBlock {
   WORD   dataLen;
   PBYTE  pData;
};
struct RCBufferD {
   WORD   dbCount;
   struct RCDataBlock db[1];
};

#pragma pack(pop)

/* ---- entry points exported to the rest of the driver ---- */
USHORT ndis_init(void);                    /* CMDInit: register with the Protocol Manager */
WORD   ndis_is_open(void);                 /* adapter opened by the protocol?             */
UCHAR *ndis_rx_get(USHORT *handle);        /* a free receive buffer (or NULL)             */
USHORT ndis_diag(USHORT i);                /* indication exclusion counters (INDTMO, INDRE, INDDROP)  */
void   ndis_rx_put(USHORT handle, USHORT len);   /* indicate the filled buffer to the protocol */

/* ---- called by the assembler entry stubs (rt_ndisx.asm), DS already loaded ---- */
extern WORD __cdecl ndis_c_sysreq(DWORD param1, DWORD param2, WORD param3, WORD opcode);
extern WORD __cdecl ndis_c_genreq(WORD protID, WORD reqHandle, WORD param1, DWORD param2, WORD opcode);
extern WORD __cdecl ndis_c_transmit(WORD protID, WORD reqHandle, PTCBD pTCBufferD);
extern WORD __cdecl ndis_c_transfer(PUSHORT pBytesCopied, WORD frameOffset, PTDBD pTDBufferD);
extern WORD __cdecl ndis_c_release(WORD reqHandle);
extern WORD __cdecl ndis_c_indon(void);
extern WORD __cdecl ndis_c_indoff(void);

/* ---- the Pascal far entry points that go into the tables (rt_ndisx.asm) ---- */
extern void __far __cdecl ndis_sysreq_entry(void);
extern void __far __cdecl ndis_genreq_entry(void);
extern void __far __cdecl ndis_transmit_entry(void);
extern void __far __cdecl ndis_transfer_entry(void);
extern void __far __cdecl ndis_release_entry(void);
extern void __far __cdecl ndis_indon_entry(void);
extern void __far __cdecl ndis_indoff_entry(void);

#endif
