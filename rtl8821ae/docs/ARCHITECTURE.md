# Architecture - RTL8821AE for OS/2

## 1. Big picture

```
 Wireless LAN Monitor / wpa_supplicant              TCP/IP (NDIS 2.0.1 protocol)
        | DosDevIOCtl cat 99h (GenMAC OIDs)                 | TransmitChain / ReceiveChain
        v                                                    v
   os2/genmac.c (OID layer)                          os2/ndis.c (MAC tables, indications)
        \                                                   /
         +------------- wlan/sta.c: scan table, join, keys, EAPOL, Ethernet <-> 802.11 ---------+
                               |  wlan/hw.h (the back-end contract)
         +---------------------+------------------------------------------------------------+
         | rtw/rtw_sta.c  scan (channel sweep + probe), join steps, CAM keys, CCMP header, rates |
         | rtw/rtw_trx.c  DMA rings: BE + management TX, RX; interrupt; service path             |
         | rtw/rtw_hal.c  device table, PCIe power flow, LLT, MAC setup, firmware, hw_up/down    |
         | rtw/rtw_phy.c  eFuse, MAC/BB/AGC/RF tables, band, channel, TX power, IQK, gain        |
         | rtw/rtw_io.c   MMIO register access, BB/RF access, H2C                                |
         +---------------------+------------------------------------------------------------+
                               |
                os2/pci.c (PCI config, BAR2 MMIO, IRQ entry, context hook)   os2/mem.c (DMA memory, firmware blob)
                               |
                     Realtek RTL8821AE (PCIe)
```

`os2/` and `wlan/` are the iwlwifi OS/2 driver's (psmedley/iwlwifi) with three changes: names and texts
(`drv.h DRV_*`), `pci.c` maps the BAR the back end names (`g_bar_cfg`: the RTL8821AE has an I/O BAR0 and its memory
registers in BAR2, rtlwifi `bar_id = 2`), and `lib.asm` / `pci.c` have 8- and 16-bit MMIO reads and a 16-bit write
(Realtek MAC registers are byte registers, the TX doorbell is a word). `dbg.c` passes unknown debug commands to
`hw_cmd()`.

The difference to the Intel back end is where the 802.11 work happens. The Intel firmware scans, keeps station
tables and runs a time event per join; the RTL8821A is a "soft MAC" whose firmware only does housekeeping (and IQ
calibration on request). So `rtw_sta.c` tunes the radio for every scan channel, sends the probe requests, writes the
BSSID and the receive filter at the join, and keeps the keys in the chip's CAM itself, the way the RTL8821AU USB
driver of this project does (`rtl8821au/src/rtl_rx.c`).

## 2. Where each part comes from

| File | Origin |
|---|---|
| `rtw_io.c` | `rtl8821au/src/rtl8821_phy.c` (bb_set, rf_write, rf_read, r21_h2c): USB vendor requests replaced by MMIO |
| `rtw_phy.c` | `rtl8821au/src/rtl8821_phy.c` and `rtl8821_efuse.c`; FEN_PCIEA, the PCIe tables, eFuse MAC at 0xD0, no DPDT switch by default (rtlwifi `rtl8821ae_phy_bb_config`, `phy_switch_wirelessband`, `reg.h`) |
| `rtw_hal.c` | power flows: rtlwifi `rtl8821ae/pwrseq.h` resolved for PCIe and cut A; `init_mac`, `hw_configure`, `poweroff_adapter`, `hw_init` order: rtlwifi `rtl8821ae/hw.c`; LLT and firmware download: `rtl8821au/src/rtl8821_hal.c` and rtlwifi `rtl8821ae/fw.c` |
| `rtw_trx.c` | ring protocol: rtlwifi `pci.c` (`_rtl_pci_init_tx_ring`, `_rtl_pci_rx_interrupt`); descriptors: rtlwifi `rtl8821ae/trx.h` / `trx.c` and `rtl8821au/src/rtl8821_tx.c`; RX status and RSSI: `rtl8821au/src/rtl_rx.c` |
| `rtw_sta.c` | `rtl8821au/src/rtl_rx.c`: scan, join register steps, `cam_write`, key handling, `adapt_rate`, `choose_rate` |
| `rtw_tbl.c` | `tools/gen_tbl.py ... pcie` from the vendor tables `halhwimg8821a_*.c` (interface PCIe: only RF-A entries differ from the USB tables) |

## 3. Memory

* **Registers**: BAR2, 16 KB; one 64 KB GDT window. MMIO of 8, 16 and 32 bits.
* **DMA memory** (from `os2/mem.c` chunks, permanent selectors, below 4 GB):

  | What | Size | Alignment |
  |---|---|---|
  | RX descriptor ring (32 x 32 bytes) and 32 buffers of 4 KB | 1 KB + 128 KB | 256 / 64 |
  | BE TX ring (32 x 64 bytes) and 32 buffers of 1664 bytes | 2 KB + 52 KB | 256 / 64 |
  | management TX ring (8 x 64 bytes) and 8 buffers of 512 bytes | 512 bytes + 4 KB | 256 / 64 |
  | a 2-entry ring for the queues the driver does not use (VO, VI, BK, high, beacon) | 128 bytes | 256 |
* The firmware file (28-31 KB) is kept in `g_fwblob` (os2/dbg.c, os2/mem.c) and copied to the chip at bring-up.
* DGROUP is about 36 KB, the code segment about 37 KB.

## 4. Execution contexts

* **Request path** (task time, under `nic_lock`): GenMAC requests, device reads / writes, bring-up, scan, join, keys.
  It may block (`dd_msleep`).
* **Interrupt** (`hw_irq`): reads `HISR` / `HISRE`; a cause we enabled = ours: `HIMR = HIMRE = 0`, EOI, context hook.
* **Context hook** (`hw_hook`): `hw_service()` (acknowledge, drain the RX ring, C2H events, beacon supervision and
  receiver gain once a second, reorder timeouts), unmask, then NDIS delivers the received frames.
* `hw_service()` is guarded by an owner-tagged lock; a busy caller leaves a note and the owner runs once more. Every
  wait services, so the driver works without an interrupt (`[rtw: no interrupt: polled]`).
* **Transmit**: NDIS TransmitChain can arrive at interrupt time on any CPU. `wlan/sta.c` serialises data frames
  (interrupts off + owner lock), so the CCMP packet number and the ring entry are taken in one step. Each ring has
  its own lock as well (management frames come from the request path and from the service path).
* A TX entry is free when the chip has cleared its OWN bit. The chip works through a ring in order, so the entry at the
  head is the oldest one in use; an OWN bit still set there means the ring is full (`TX full=`). No reclaim pass.

## 5. Bring-up (`hw_up`; `STATE=` / `RTW step=`)

| Step | What |
|---|---|
| 1 | firmware file present |
| 2 | PCI: memory + bus master, INTx on, ASPM off |
| 3 | BAR2 mapped |
| 4 | rings allocated, interrupt hooked (or polled) |
| 5 | chip answers (`REG_SYS_CFG`); a chip still on (warm boot) is powered off first (`_rtl8821ae_poweroff_adapter`: LPS-enter flow, 8051 reset, disable flow) |
| 6 | rings reset; `_rtl8821ae_init_mac`: power-on flow (PCIe), MAC blocks on, LLT (TX boundary 0xF7, RQPN 0x80E60808), queue priorities, ring addresses, PCIe settings |
| 7 | eFuse: MAC address, crystal trim, amplifiers, TX power |
| 8 | firmware download (4 KB pages to 0x1000, checksum report, `MCUFWDL_RDY`, 8051 reset, `WINTINI_RDY`) |
| 9 | MAC table, BB + AGC tables, crystal capacitor |
| 10 | RF table, `hw_configure` (response rates, retry limits, A-MPDU, beacon control, receive filters, multicast) |
| 11 | 2.4 GHz band, channel 1, 20 MHz, LC calibration |
| 12 | CAM cleared, MAC address, link type station, TX unpaused, RX / PCIe DMA released, MAC TX / RX on |
| 64 | interrupts on: **READY** |

## 6. Join

`wlan/sta.c` does the 802.11 part; the back end:

1. **hw_join_prepare**: BSSID register cleared, receive filter "every network", keys cleared, channel, firmware IQ
   calibration (5 GHz, when the firmware offers it).
2. Authentication and association frames go out on the management ring at 1 Mbit/s (6 Mbit/s on 5 GHz).
3. **hw_join_complete**: BSSID register (the MAC now drops other networks' frames), receive filter "our BSS",
   beacon interval, security on for a WPA2 network.
4. **Keys**: pairwise in CAM entry 4 with the AP's address, group keys in entries 0-3 by key id, AES; `SECCFG 0xCC`.
   A repeated install of the same pairwise key is ignored (the packet numbers must not restart).
5. **Data**: the BE ring, SEC_TYPE AES with the 8-byte CCMP header from the driver (packet number), the chip adds the
   MIC. Received protected frames arrive decrypted with CCMP header and MIC in place (RX status SWDEC clear).
6. **Leave**: BSSID cleared, receive filter "every network", keys cleared, receiver gain back to its most sensitive.

## 7. What to look at first (open questions for the first hardware runs)

1. **Power-on flow.** `pwrfail=` names the entry of `g_pwr_on` (rtw_hal.c) that timed out. Entry 6 is "power ready";
   if it fails on a warm boot, try a cold boot first.
2. **Firmware.** `FW chksum=0` points at the page writes (dword writes to 0x1000); `ready=0` at the 8051 start. Both
   firmware images (Linux v29 and the vendor v42) have the 0x2101 header.
3. **Rings.** If nothing is received (`RX frames=0`, `TRX rxi=0`) after a scan, check that `RCR` and `CR` read back
   as expected (`REGS`), that `HISR` shows `ROK` (bit 0) and that `rdu=` (no free RX descriptor) stays 0.
4. **Transmit.** `TX mgmt=` counts probe requests put in the ring; `full=` growing means the chip never clears OWN
   (doorbell bits of `REG_PCIE_CTRL_REG`, ring addresses `REG_MGQ_DESA` / `REG_BEQ_DESA`).
5. **Interrupt.** `IRQS=` / `SPUR=` / `HOOK=` (general lines) and `lastisr=`. Without interrupts the driver still
   works, more slowly, from the GenMAC request path.
6. **Antenna / band switch.** Some boards may need the external DPDT switch the 8811AU dongles use (rtlwifi does not
   drive it for the 8821AE, so it is off): if 2.4 GHz works and 5 GHz hears nothing, try `echo bsw 1 > \DEV\WRND32$`
   and scan again.
7. **Firmware IQ calibration.** Linux calibrates on the host and never asks the v29 firmware; if it does not answer,
   the driver stops asking after the first 500 ms timeout (`IQK tmo=1`, `mode=0`). 5 GHz then runs uncalibrated.
