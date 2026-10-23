# RTL88EU OS/2 driver - source provenance audit, version 0.1.2

This is an engineering inventory, **not legal advice**. It lists, per file, where the code came from. A lawyer
(or at least the OS2World community) should confirm before the project is promoted.

## What changed since 0.1.1

0.1.1 contained files adapted from the IBM OS/2 Device Driver Source Kit and was linked with IBM's libraries and
built with IBM's tools. **0.1.2 contains no IBM code, headers or libraries and is built with open-source tools only**
(Open Watcom C 2.0 16-bit compiler and linker, NASM). The IBM-derived files were not edited: their replacements were
written from the published specifications listed below, with new names. `PROVENANCE.md` records, for every number or
layout taken from a document, the document and section it comes from, and which facts were confirmed by observing the
running driver instead (marked INTERFACE).

This is an independent re-implementation from specifications, **not a legal clean-room** procedure: the author of the
replacement files (the assistant working on this project) had earlier read IBM's sample sources while writing 0.1.x.
The behaviour the project itself had found by testing on real hardware (USB data toggle tracking, request packet pool,
buffer handling) was kept on purpose. `PROVENANCE.md`, section 8, states this. A reviewer other than the author should
read that register and spot-check the files before the project is announced as free of IBM material.

## License

The project owner chose **GNU GPL version 2 only** (SPDX `GPL-2.0-only`): the files derived from the Linux
`rtl8188eu` driver carry Realtek's "version 2 ... no later version" terms, so the combined driver can only be
distributed under GPLv2.

* `COPYING` holds the GPLv2 text; every source file carries an SPDX header.
* Files derived from the Linux driver also carry Realtek's attribution.
* `fwimg.c` is the Realtek firmware as a byte array, marked `LicenseRef-Realtek-Firmware` (binary firmware with its
  own redistribution terms; see the README).
* The copyright holder in the headers is the placeholder "the RTL88EU-OS2 project": replace it with the real holder(s)
  before publishing.

## Specifications used (documents, not code)

| Document | Used for |
|---|---|
| IBM *Physical Device Driver Reference* (DDK 2004) | device header, request packets, strategy commands, DevHelp register conventions |
| IBM *Basic USB Device Driver Stack for OS/2 Warp - Programmer's Reference*, v1.1 | USBD IDC interface: request block, cancel, configuration, registration, accept/reject, detach |
| *Universal Serial Bus Specification*, revision 2.0 | descriptors, control requests, data toggle |
| *NDIS 2.0.1 specification* (3Com/Microsoft, 1990; may be implemented freely) and the DDK LAN reference | characteristics tables, dispatch tables, Protocol Manager interface |
| OS/2 *Control Program Programming Reference* | signatures of `DosOpen`, `DosClose`, `DosRead`, `DosDevIOCtl` |
| EDM2 "Suggestion for OS/2 and eComStation Drivers Arguments" | the `/V /Q /W /AA` switches and the boot message id |
| RFC 2131 | DHCP client |

None of these documents is included in this repository.

## Per-file table

Legend: **ORIG** = written for this project; **LIN** = ported or derived from the Linux driver (GPL-2.0-only);
**WIN** = derived from observing the Windows driver's USB traffic; **BIN** = binary blob; **SPEC** = written from a
published specification (above).

| File | Class | Evidence |
|---|---|---|
| `fwimg.c` | **BIN** | Hex dump of Realtek's `rtl8188eufw.bin` |
| `rtl_mac.c`, `rtl_bb.c`, `rtl_rf.c`, `rtl_iqk.c`, `fwdl.c`, `rtl_init.c` | **LIN** | headers name the Linux files they port (`HalHWImg8188E_*`, `HalPhyRf_8188e.c`, `rtl8188e_hal_init.c`, `Hal8188EPwrSeq.c`) |
| `rtw_reg.c` | **LIN + ORIG** | register access and constants per the Linux `usb_ops_linux.c`; eFuse MAC decoder, TX-power helper, early-MAC logic are ours |
| `rtl_rx.c` | **LIN + ORIG (+ WIN)** | LLT, queues, RCR, TX descriptor and key register usage follow the Linux driver (cross-checked against the Windows capture); the 802.11 join state machine, EAPOL queue, duplicate filter, receive path to NDIS, LED and RSSI smoothing are ours |
| `rtl_nic.c` | **ORIG** | GenMAC OID layer; NDIS OID numbers are public (Microsoft NDIS), private OID numbers learned from the xwlan sources (not copied) |
| `rtl_dhcp.c` | **ORIG** | written from RFC 2131 |
| `rtl_dbg.c` | **ORIG** | debug command interpreter |
| `rt_os2dd.h` | **SPEC** | PDD Reference |
| `rt_hdr.asm`, `rt_end.asm` | **SPEC** | PDD Reference (device header, segment layout, resident-end markers) |
| `rt_lib.asm` | **SPEC** | PDD Reference (DevHelp register conventions), plain string/atomic routines |
| `rt_strat.c` | **SPEC + ORIG** | PDD Reference; switches and messages are ours |
| `rt_usb.h`, `rt_usb.c` | **SPEC + ORIG** | USBPRM and USB 2.0; toggle, packet pool and slot logic are ours (found on real hardware) |
| `rt_ndis.h`, `rt_ndis.c`, `rt_ndisx.asm` | **SPEC + ORIG** | NDIS 2.0.1; receive/transmit queueing and the SMP indication exclusion are ours |
| `build.cmd`, `rtl88eu.lnk` | **ORIG** | Open Watcom / NASM build scripts |

Not linked: IBM `usbmisc.lib`, `dhcalls.lib`, `rmcalls.lib`, `os2386.lib`, and no IBM header is included.
The only run-time dependencies are the OS/2 16-bit API imports `DosOpen`, `DosClose`, `DosRead`, `DosDevIOCtl`
(from `DOSCALLS`, part of the operating system). `wpa_supplicant` and xwlan are separate programs.

## The Open Watcom run-time

The driver is linked without any Open Watcom library (`-zl`, `nodefaultlibs`): no Open Watcom run-time code is part
of the binary. The few routines that C would normally take from a library are in `rt_lib.asm`.
