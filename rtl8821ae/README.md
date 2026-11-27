# RTL8821AE - Realtek RTL8821AE PCIe WiFi for OS/2 and ArcaOS

A native driver for the **Realtek RTL8821AE** (802.11ac, one stream, PCI Express; mini PCIe and M.2 cards, PCI id
`10EC:8821`). Like the other drivers of this project it is one 16-bit physical device driver that shows the adapter to
the system in two ways:

* a **GenMAC-compatible device** (`\DEV\WRND32$`), so the ArcaOS **Wireless LAN Monitor** and `wpa_supplicant` manage it,
* an **NDIS 2.0.1 MAC** (`RTL8821AE_NIF`), so the OS/2 TCP/IP stack binds to it as `lanN`.

It is put together from two working drivers:

| Part | Comes from |
|---|---|
| OS/2 plumbing: device header, DevHelp glue, strategy routine, PCI configuration, BAR mapping, interrupt and context hook, DMA memory, NDIS MAC, GenMAC OIDs, status and debug channel (`src/os2/`) | the **iwlwifi** OS/2 driver ([psmedley/iwlwifi](https://github.com/psmedley/iwlwifi)), itself from the QCA6174 and RTL88EU drivers |
| The chip-independent station: scan table, authentication / association, WPA2 keys from the supplicant, EAPOL queue, Ethernet <-> 802.11, Block Ack reorder buffer (`src/wlan/`) | the same iwlwifi driver, unchanged apart from one debug hook (`hw_cmd`) |
| The RTL8821A chip layer: power-on, LLT, firmware download, MAC / BB / AGC / RF tables, band and channel switch, 5 GHz transmit power, eFuse, firmware IQ calibration, receiver gain, CAM keys, TX descriptor, RX status, rate ladder (`src/rtw/`) | the **RTL8821AU** USB driver of this project ([`../rtl8821au`](../rtl8821au)) - the same chip behind USB |
| The PCIe side: power sequence for the PCIe interface, DMA descriptor rings, interrupt registers, ring addresses, TX doorbell | Linux **rtlwifi** `rtl8821ae` (`pci.c`, `rtl8821ae/hw.c`, `trx.c`, `reg.h`, `pwrseq.h`) |

> **Status: written, builds, not yet tried on hardware (0.0.1, build "dev").** Every register sequence is taken from
> one of the sources above, but none of the PCIe-specific code has run on a real RTL8821AE yet. Expect the first runs
> to need fixes; the status text (`copy \DEV\WRND32$ con`) and the debug commands below are there to find them.
> Section "What to look at first" in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) lists where the open questions are.

## What it does (once it runs)

* Scan of 2.4 GHz (1-13) and 5 GHz (36-64, 100-140 passive, 149-165), the networks in the Wireless LAN Monitor.
* Join an open or WPA2-PSK (CCMP) network: open authentication, association (HT 20 MHz and WMM when the access point
  has them), the 4-way and group handshakes through `wpa_supplicant`, keys in the chip's CAM (hardware CCMP).
* IP over WiFi through the normal TCP/IP stack: configure `lanN` for DHCP or a static address as for any adapter
  (unlike the USB drivers, there is no DHCP client inside this driver).
* Receive aggregation (A-MPDU): the chip answers with block acks, `wlan/sta.c` puts frames back in order.
* Transmit rates from the signal strength (the 8821AU ladder: OFDM up to 36 Mbit/s, CCK at the edge of 2.4 GHz),
  handshake / ARP / DHCP frames at 1 Mbit/s (6 Mbit/s on 5 GHz); receiver gain follows the signal.
* Beacon loss (10 s) is reported to the station, which leaves; the supplicant or the monitor rejoins.

Not done: 40 / 80 MHz, transmit aggregation, power save, host IQ calibration (the firmware's is used when it offers
it), Bluetooth coexistence (the RTL8821AE is a WiFi + Bluetooth combo; the Bluetooth half is not touched), WEP / TKIP /
WPA3, the RTL8812AE (2x2) and the RTL8821CE (a different chip family).

## Install (for testing)

1. Copy `bin\RTL8821AE.OS2`, `bin\RTL8821AE.FW` and `install\RTL8821AE.NIF` to `C:\IBMCOM\MACS`.
2. `CONFIG.SYS`: `DEVICE=C:\IBMCOM\MACS\RTL8821AE.OS2 /V`. Only one `WRND32$` driver can be loaded: remove or `REM`
   GenMAC, the RTL USB drivers, the iwlwifi or the QCA6174 driver line.
3. `PROTOCOL.INI`: bind `RTL8821AE_NIF` as in `install\PROTOCOL.INI.example` (MPTS can do it too, with the NIF).
4. `STARTUP.CMD`: the `copy /b` line from `install\STARTUP.CMD.example` (the firmware, once per boot).
5. Reboot. `copy \DEV\WRND32$ con` shows the state; `STATE=0064 READY` means the chip is up. Then `RTWMAC.CMD`
   writes the adapter's MAC into `PROTOCOL.INI` (recommended: TCP/IP copies the address when it binds).
6. Use the Wireless LAN Monitor, or for a first test of an open network: `echo join MyNetwork > \DEV\WRND32$`.

The firmware is Realtek's `rtlwifi/rtl8821aefw_29.bin` from linux-firmware (the one Linux loads for this chip),
redistributed under `bin\LICENCE.rtlwifi_firmware.txt`. The 8821AU driver's `RTL8821AU.FW` (Realtek's version 42
"nic" image of the vendor driver) is accepted as well; with it the firmware IQ calibration runs after 5 GHz joins.

## Reading the state

`copy \DEV\WRND32$ con` (or `> file`) prints `KEY=VALUE` lines. The general ones are those of the iwlwifi driver
(`STATE`, `MAC`/`HWMAC`, `LINK`, `DATA`, `NDIS`, `BA`, `NET=` per network, the last GenMAC requests). The back end adds:

| Line | Meaning |
|---|---|
| `RTW step= warm= SYSCFG= cut= pwrfail= lltfail=` | bring-up step (table in docs/ARCHITECTURE.md), whether the chip was still on, `REG_SYS_CFG`, the failing power-sequence entry, the failing LLT entry |
| `FW ver= sub= tries= chksum= ready= MCUFWDL= H2CBUSY=` | firmware version from its header, download attempts, checksum report, `WINTINI_RDY`, the register, firmware commands that found no free box |
| `EFUSE ok= used= xtal= rfe= pa2g/lna2g/pa5g/lna5g= thermal=` | the eFuse: decoded, bytes used, crystal trim, RFE type, external amplifiers |
| `PHY ch= band= igi= txp5= ofdm= ht= grp= IQK ...` | channel, band, receiver gain, 5 GHz power indexes, IQ calibration counters |
| `CHK` | read-backs after bring-up: BB 800, 808, C1C, C50, RF 00, RF 18, CR, RCR |
| `REGS CR= RCR= HIMR= HISR= PCIE= TXPAUSE=` | live MAC registers |
| `TRX on= rxi= behead= mghead= masked= svc= lastisr=` | ring positions, interrupt masked, service runs, the last interrupt status |
| `RX frames= crc= c2h= rdu= big= fovw= rate=` / `TX be= mgmt= full= fovw= ...` | receive and transmit counters |

## Debug commands

`echo <command> > \DEV\WRND32$` (note the space before `>`). Values are hexadecimal; results go to COM1.

`up` (retry a failed start), `scan`, `join <ssid>` (open network), `leave`, `r <reg>` (32-bit read), `r1 <reg>`,
`w1|w2|w4 <reg> <value>`, `rf <reg> [value]`, `ch <channel>` (not while joined; `ch 24` = channel 36), `txp 0|1`
(5 GHz eFuse power), `iqk 0|1|2` (and run it), `dig <igi>` / `dig 0` (fixed / automatic receiver gain),
`rate <code>` / `rate ff` (fixed / automatic data rate: 0-3 CCK, 4-B OFDM 6-54), `ctl <code>`, `bsw 0|1` (external
antenna band switch of single-antenna boards), `lck`, `down`.

## Building

Linux (cross): Open Watcom 2.0 `binl64` and NASM on the `PATH`, `WATCOM` set, `INCLUDE` empty, then `make -C src`.
OS/2: `build.cmd` in `src`. The flags and rules are those of the other drivers ([../BUILD.md](../BUILD.md)). The
register tables in `src/rtw/rtw_tbl.c` are generated: `python3 tools/gen_tbl.py <8821au-20210708>/hal/phydm/rtl8821a
rtl8821ae/src/rtw/rtw_tbl.c pcie`. Host tests of the shared station code: `sh tests/llc/run.sh`, `sh tests/ba_reorder/run.sh`.

## Layout

```
src/os2/    OS/2 plumbing (from the iwlwifi driver; pci.c reads the BAR the back end names, lib.asm has 8/16-bit MMIO)
src/wlan/   the station (from the iwlwifi driver) and hw.h, the contract a chip back end implements
src/rtw/    the RTL8821AE back end
install/    NIF, PROTOCOL.INI and STARTUP.CMD examples, RTWMAC.CMD
bin/        RTL8821AE.OS2, RTL8821AE.FW and its licence
docs/       ARCHITECTURE.md
tests/      host tests of wlan/sta.c (from the iwlwifi driver)
```

## Licence

GPL-2.0-only, as the rest of the project (`../COPYING`). The chip code derives from Realtek's GPL-2.0 Linux drivers
(the vendor 8821au driver through `rtl8821au/`, and rtlwifi); the OS/2 and station code is the iwlwifi driver's
(GPL-2.0-only). The firmware is Realtek's binary, redistributable under its own licence. See [../PROVENANCE.md](../PROVENANCE.md).
