# Provenance and licence audit - preview 0.1.3

This is an engineering inventory, **not legal advice**. It says where each part of the code came from, so a reviewer
can check it. For the platform code shared with 0.1.2 (device header, USB layer, NDIS layer, GenMAC layer, DHCP client,
8188EU chip layer) the full per-file register and the list of specifications is in
[docs/PROVENANCE-0.1.2.md](docs/PROVENANCE-0.1.2.md) and [docs/LICENSE-AUDIT-0.1.2.md](docs/LICENSE-AUDIT-0.1.2.md);
they still apply. This file covers what 0.1.3 adds or changes.

## Licence

**GNU GPL version 2 only** (`COPYING`; SPDX `GPL-2.0-only` in every source file). The Realtek Linux drivers the chip
layers derive from carry "version 2 ... no later version", so the combined drivers can only be distributed under GPLv2.

The copyright line in the headers is a placeholder ("the RTL88EU-OS2 project"). **Replace it with the real
copyright holder(s) before publishing.**

## Still to be confirmed before a public announcement

1. **The outside SMP patch.** The SMP work in this release started from a patch file named
   `rtl88eu-0.1.2-smp.patch` that was supplied to the project from outside. It carries no author, no licence and no
   date other than the file times. Ideas and code from it were adopted: the assembler helpers in `rt_lib.asm`
   (`rt_cpu_id`, the `cmpxchg` lock helpers, `rt_wait_zero`, `rt_atomic_dec_nz`), the owner-tagged indication exclusion in
   `rt_ndis.c`, the synchronous transfer lock and the TX queue lock design in `rt_usb.c`, and the zero-copy transmit
   gather. The adopted code was reviewed and then changed substantially while testing (for example the TX lock no
   longer disables interrupts, and the waits are bounded). **The author of the patch and the terms under which it was
   given still have to be established and recorded here** (a GPL-2.0-only compatible grant is needed).
2. A reviewer other than the code's author should spot-check the files before the project is announced as free of
   IBM material (see the 0.1.2 audit, section "What changed since 0.1.1").
3. The Realtek firmware images (below) are binary blobs.

## What 0.1.3 adds

| File(s) | Class | Evidence |
|---|---|---|
| `rtl8821_hal.c`, `rtl8821_phy.c`, `rtl8821_efuse.c`, `rtl8821_tx.c` | **LIN** | ported from the Realtek Linux driver `morrownr/8821au-20210708` (`hal/rtl8812a/usb/usb_halinit.c`, `rtl8812a_phycfg.c`, `rtl8812a_hal_init.c`, `rtl8812a_xmit.c`, the eFuse code; Copyright (c) 2007 - 2017 Realtek Corporation, GPL-2.0). Headers name the Linux files |
| `rtl8821_tbl.c`, `rtl8821_tbl.h` | **LIN (generated)** | MAC / baseband / AGC / RF-A tables flattened by `tools/gen_tbl.py` from `halhwimg8821a_mac.c`, `halhwimg8821a_bb.c`, `halhwimg8821a_rf.c` (Realtek, GPL-2.0), evaluating the board conditions once for a USB interface and a default board |
| `RTL8821AU.FW` | **BIN** | Realtek firmware `array_mp_8821a_fw_nic` (30,880 bytes) taken from the same Linux driver source and written out as a file. Redistributed with the Linux driver under Realtek's notice; check it before redistributing |
| `rtl_rx.c` (8821AU copy) | **LIN + ORIG** | as 0.1.2; new: receiver gain control, LED sequence (`SwLedOn_8821AU` / `SwLedOff_8821AU` of the Linux driver), join/de-auth handling, duplicate-key guard (ours) |
| `rt_lib.asm`, `rt_usb.c`, `rt_ndis.c`, `rtw_reg.c`, `rtl_nic.c`, `rtl_rx.c`, `rt_usb.h`, `rt_os2dd.h` (both drivers) | **ORIG + outside patch** | the SMP changes listed above and in CHANGELOG.md |
| `tools/gen_tbl.py` | **ORIG** | the table generator |

Everything else in `rtl8821au\src` that is not listed (`rt_hdr.asm`, `rt_end.asm`, `rt_ndisx.asm`, `rt_strat.c`,
`rtl_dhcp.c`, `rtl_dbg.c`, `rt_ndis.h`) is the 0.1.2 platform code (**SPEC / ORIG**, see the 0.1.2 register).
`rtl8188eu\src` is the 0.1.2 source plus the SMP changes; its chip layer is **LIN** from the Linux `rtl8188eu` driver.

## Not linked

No IBM library or header; the only run-time imports are `DosOpen`, `DosClose`, `DosRead`, `DosDevIOCtl` from
`DOSCALLS`. No Open Watcom run-time code is in the binaries (`-zl`, `nodefaultlibs`).

## Specifications used

As in the 0.1.2 audit (IBM Physical Device Driver Reference, the USB stack programmer's reference, USB 2.0,
NDIS 2.0.1, the OS/2 control program reference, RFC 2131), plus the public datasheet-level facts visible in the
Realtek Linux driver for the 8821A. None of these documents is included here.
