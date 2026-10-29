# Installing and using the drivers (preview 0.1.3)

Two native 16-bit OS/2 device drivers for Realtek USB WiFi dongles. Each one presents the dongle to the system in
two ways:

* a **GenMAC-compatible device** (`WRND32$`), so the stock ArcaOS **Wireless LAN Monitor** (XWLAN) and
  `wpa_supplicant` manage it like any other WiFi adapter, and
* an **NDIS 2.0.1 MAC**, so the OS/2 TCP/IP stack binds to it as an ordinary network interface (`lan1`).

Both are built with open-source tools only (Open Watcom + NASM). No IBM code or libraries are linked.

> **Status: experimental.** They work on the author's test machines (see [docs/TESTING.md](TESTING.md)),
> but they have only been tried on a few setups. Read [Known issues](#known-issues) before relying on them.

## Which driver for which dongle

| Dongle | USB id | Folder | Driver file | Needs |
|---|---|---|---|---|
| Realtek RTL8188EUS (the common "150 Mbit" nano dongles) | `0BDA:8179` | `rtl8188eu\` | `RTL88EU.OS2` | nothing else (firmware is built in) |
| TP-Link **Archer T2U Nano** (RTL8811AU / 8821AU) | `2357:011E` | `rtl8821au\` | `RTL8821AU.OS2` | `RTL8821AU.FW` copied into the driver after every boot |

**Only one of the two can be loaded at a time.** Both register the same device name (`WRND32$`) and use the same
`PROTOCOL.INI` label (`RTL88EU_NIF`). Keep both files in `C:\IBMCOM\MACS` if you like, and have exactly one
`DEVICE=` line active in `CONFIG.SYS` (put `REM` in front of the other), then reboot.

## What's new in 0.1.3

* **New driver for the TP-Link Archer T2U Nano (RTL8821AU).** 2.4 GHz, WPA2-PSK, scanning, DHCP, LED, signal-based
  receiver gain, automatic re-association after the access point drops the link.
* **SMP fixes in both drivers** (the 8188EU one is the 0.1.2 driver plus these). The 0.1.2 driver could trap in
  `AFINETK`/`SOCKETS` (the TCP/IP kernel modules) on the SMP kernel under sustained traffic. The new code serialises
  what two CPUs must not do at once: USB transfers, the order of transmitted frames and their CCMP packet numbers, the
  indication calls into the protocol stack, and the receive buffers. The transmit lock never disables interrupts and
  never waits for ever (a refused frame is simply retried by the stack). Details: [CHANGELOG.md](../CHANGELOG.md).
* Boot messages and the driver file for the 8821AU say `RTL8821AU`; the 8188EU driver is unchanged in name.

## Install (TP-Link Archer T2U Nano)

1. Copy `rtl8821au\bin\RTL8821AU.OS2`, `RTL8821AU.FW` and `rtl8821au\install\RTL88EU.NIF` to `C:\IBMCOM\MACS`.
2. Add `DEVICE=C:\IBMCOM\MACS\RTL8821AU.OS2` to `CONFIG.SYS` (after the USB drivers; add `/V` for boot messages).
3. Bind the adapter in `C:\IBMCOM\PROTOCOL.INI` as in `install\PROTOCOL.INI.example`
   (an `RTL88EU_NIF = RTL88EU.NIF` entry, a `[RTL88EU_NIF]` section with `DriverName = WRND32$`, and the label in the
   `Bindings` line of `[TCPIP_NIF]`). Keep a copy of the old file: if the driver is not loaded, a binding to it
   breaks all networking.
4. The firmware cannot be loaded at boot by the driver itself (that happens too early). Load it once per boot with
   `copy /b C:\IBMCOM\MACS\RTL8821AU.FW \DEV\WRND32$`. Put that line in `C:\STARTUP.CMD`
   (`install\STARTUP.CMD.example`). Until it has run, the dongle LED stays on steadily and the Wireless LAN Monitor
   reports "Association request to the driver failed".
5. Reboot, plug the dongle in, and use the Wireless LAN Monitor as usual: radio on, pick the network, WPA2-PSK passphrase.
   The LED blinks slowly while searching, stays on when connected, and flickers with traffic.

## Install (RTL8188EU)

Same, with `rtl8188eu\bin\RTL88EU.OS2`, `rtl8188eu\install\RTL88EU.NIF`, `DEVICE=C:\IBMCOM\MACS\RTL88EU.OS2`, and no
firmware step. (This build still prints version `0.1.2` at boot and in its BLDLEVEL string: it is the 0.1.2 driver
with the SMP fixes; the SHA-256 of the file is in `SHA256SUMS`.)

## Known issues

* **Experimental.** Tested on a handful of setups only; expect rough edges.
* **SMP kernel:** the old 0.1.2 8188EU driver trapped on SMP under heavy traffic (documented, reproduced again on
  2026-10-28 in `AFINETK`). This package's SMP-fixed build has run for hours under load without a trap, but long-term
  confidence is limited to what [docs/TESTING.md](TESTING.md) lists. If you hit a trap, a photo of the trap screen
  (module, `CS:EIP`, `CR2`, CPU number) is the most useful report. `PSD=ACPI.PSD /CPUMASK=1` (one CPU) is a safe fallback.
* **One unexplained episode (8821AU, SMP, first run after a boot):** traffic from the PC to one particular WiFi
  neighbour failed while other destinations worked. It did not come back after a reboot (hundreds of 20 MB downloads
  since). The cause is not understood; see TESTING.md.
* **8821AU limits:** 2.4 GHz only has been exercised (5 GHz scanning exists, joining a 5 GHz network is untested); no
  HT/VHT rates (throughput is 802.11g class, 1-2 MB/s measured); no per-rate TX power calibration from the eFuse; no
  power saving; no AP mode; no WEP/WPA1; no 802.11w.
* **Wired and WiFi on the same subnet** confuses the routing of any OS/2 machine (replies leave through the wrong
  adapter). Test WiFi with the cable unplugged, or put the interfaces on different subnets.
* The release builds have the debug channel (`\DEV\WRND32$` writes) restricted to read-only commands.

## Building

[BUILD.md](../BUILD.md). Open Watcom 2.0 (16-bit `wcc`, `wlink`) and NASM on OS/2.

## Licence and provenance

GNU GPL version 2 only (`COPYING`, SPDX `GPL-2.0-only` in every source file): large parts derive from Realtek's Linux
drivers (`rtl8188eu`, `rtl8821au`), which carry that licence. The firmware images are Realtek's binary firmware (see
`../PROVENANCE.md`). **Read `../PROVENANCE.md` before redistributing:** it lists where every file came from and what is still
to be confirmed (the copyright holder placeholder, and the origin and licence of an outside SMP patch whose ideas and
helper routines were adopted).

## Credits

Project owner and tester: Martin Iturbide (OS2World). Most of the code was written with Claude (Anthropic) as the
programming assistant, working from the Realtek Linux drivers and public specifications.
