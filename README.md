# DRV-NET-RTLUSBWifi

Native **WiFi drivers for OS/2 and ArcaOS** for Realtek USB dongles. Two drivers live in this project:

| Dongle | USB id | Driver | Folder |
|---|---|---|---|
| Realtek **RTL8188EUS** (the common 150 Mbit nano dongles) | `0BDA:8179` | `RTL88EU.OS2` | [`rtl8188eu/`](rtl8188eu/) |
| TP-Link **Archer T2U Nano** (RTL8811AU / RTL8821AU) | `2357:011E` | `RTL8821AU.OS2` | [`rtl8821au/`](rtl8821au/) |

Both are 16-bit device drivers that talk to the dongle directly over the OS/2 USB stack and show it to the system in
two ways:

* a **GenMAC-compatible device** (`WRND32$`), so the stock ArcaOS **Wireless LAN Monitor** (XWLAN) and `wpa_supplicant`
  manage it like any other WiFi adapter, and
* an **NDIS 2.0.1 MAC**, so the OS/2 TCP/IP stack sees an ordinary network interface (`lan1`).

They are built with **open-source tools only** (Open Watcom + NASM) and link no IBM code or libraries.

> **Status: experimental preview (0.1.3).** They work on the author's test machines (real PCs with EHCI and xHCI
> controllers, the ArcaOS SMP kernel, VirtualBox), but have not been tried widely. See [docs/TESTING.md](docs/TESTING.md).

## What works

* WPA2-PSK (CCMP/AES) association, including the 4-way and group-key handshakes; keys live in the chip's hardware key
  table and the chip does the encryption and decryption.
* Scanning (the Wireless LAN Monitor lists the networks), signal strength, MAC address from the dongle's eFuse.
* IP over WiFi: DHCP is run by the driver (helper script `rtl88eu_ip.cmd`); ping, web and ssh work over `lan1`.
* Wireless LAN Monitor integration: device list, profile with WPA2-PSK, connect and reconnect, status icon.
* Automatic re-association when the access point drops the link.
* The dongle LED: slow blink while searching, steady on when connected, flicker on traffic.
* The **SMP kernel**: the 0.1.3 drivers serialise USB transfers, the order of transmitted frames and their packet numbers,
  and the indications into the protocol stack, which is what the 0.1.2 driver lacked
  (it could trap in `AFINETK` / `SOCKETS` under sustained traffic on two or more CPUs).
* RTL8821AU only: signal-based receiver gain, so a strong access point nearby does not overload the receiver.

## Install in a minute

*TP-Link Archer T2U Nano:* copy `RTL8821AU.OS2`, `RTL8821AU.FW` and `RTL88EU.NIF` (from `rtl8821au/install`) to
`C:\IBMCOM\MACS`; add `DEVICE=C:\IBMCOM\MACS\RTL8821AU.OS2` to `CONFIG.SYS`; bind the adapter in `PROTOCOL.INI` as in
`PROTOCOL.INI.example`; put the firmware load line from `STARTUP.CMD.example` into `C:\STARTUP.CMD`
(`copy /b C:\IBMCOM\MACS\RTL8821AU.FW \DEV\WRND32$`, once per boot); reboot, plug the dongle in and use the Wireless LAN
Monitor. *RTL8188EU:* the same with `RTL88EU.OS2` and no firmware step. **Only one of the two drivers can be loaded at a
time** (both use the device name `WRND32$`): keep one `DEVICE=` line active. Full steps are in the folder READMEs and in
[docs/](docs/).

## Building

Open Watcom 2.0 (16-bit `wcc`, `wlink`) and NASM on OS/2; each driver has its own `build.cmd`. See [BUILD.md](BUILD.md).

## Repository layout

```
README.md  CHANGELOG.md  BUILD.md  PROVENANCE.md  SECURITY.md  COPYING
docs/        ARCHITECTURE.md  HARDWARE.md  TESTING.md  (and the 0.1.2 audit documents)
rtl8188eu/   src/  install/  bin/        RTL88EU.OS2 (0.1.2 driver + SMP fixes)
rtl8821au/   src/  install/  bin/        RTL8821AU.OS2 + RTL8821AU.FW
tools/       gen_tbl.py                  generator of the RTL8821A register tables
```

## Known issues

* Experimental; expect rough edges. On an SMP kernel, long-term stability is limited to what
  [docs/TESTING.md](docs/TESTING.md) records. `PSD=ACPI.PSD /CPUMASK=1` (one CPU) is a safe fallback.
* RTL8821AU: 2.4 GHz only has been exercised; no HT/VHT rates (802.11g-class throughput, 1-2 MB/s), no per-rate TX power
  calibration, no power saving, no AP mode. The firmware must be loaded after every boot.
* No WEP/WPA1, no 802.11w, no WPA3.
* One unexplained episode on one test machine (8821AU, SMP, first boot): traffic to one WiFi neighbour failed while
  other destinations worked; it did not return after a reboot. Details in the testing record.
* Wired and WiFi on the same subnet confuse the routing of any OS/2 machine: test WiFi with the cable unplugged.

If you hit a trap, a photo of the trap screen (module, `CS:EIP`, `CR2`, CPU number) and the driver counters
(`copy \DEV\WRND32$ file`) make the best bug report.

## Licence

**GNU GPL version 2 only** (`COPYING`; SPDX `GPL-2.0-only` in the sources): the drivers derive from Realtek's Linux
drivers `rtl8188eu` and `rtl8821au`, which carry that licence. The firmware images are Realtek's binary firmware. Where
each file came from, and what is still to be confirmed, is in [PROVENANCE.md](PROVENANCE.md).

## Credits

Project owner and tester: Martin Iturbide (OS2World). Most of the code was written with Claude (Anthropic) as the
programming assistant, from the Realtek Linux drivers and public specifications.
