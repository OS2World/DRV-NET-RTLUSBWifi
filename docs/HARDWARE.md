# Hardware and software tested

## Supported dongles

The driver recognises one USB id:

| USB id | Chip | Notes |
|---|---|---|
| `0BDA:8179` | Realtek RTL8188EU(S), 802.11b/g/n 1T1R | "Realtek 802.11n NIC" - the id used by many cheap 150 Mbit/s dongles |

Dongles of other brands that contain the same chip often use different USB ids (the Linux driver lists many).
They should work with this driver once their id is added in `src/rt_usb.h` (`RTL8188EU_VID` / `RTL8188EU_PID`
are compared in `rt_usb.c`, function `rt_idc`, case `CHKSERV`); this has **not** been tested. Chips other than the RTL8188EU
(RTL8188CUS, RTL8192CU, RTL8188FU, RTL8812/8821 ...) are **not** supported: they need different firmware and tables.

## Tested configurations

| Setup | USB controller driver | Kernel | Result (0.1.1; see TESTING.md for 0.1.2) |
|---|---|---|---|
| Real PC | EHCI (`USBEHCD`), dongle at high speed | ArcaOS 5.1.1, SMP (2 CPUs) | joins WPA2-PSK, holds the link, 20/20 forced re-associations, boots with WiFi as the only network |
| Real PC | xHCI (`USBXHCD`) | ArcaOS 5.1.1 | joins WPA2-PSK, survives plugging/unplugging a USB Ethernet adapter |
| VirtualBox | xHCI (emulated) | ArcaOS 5.1.1, 1 CPU | joins WPA2-PSK |
| VirtualBox | EHCI + OHCI (emulated) | ArcaOS 5.1.1, 1 CPU | joins WPA2-PSK |

Common to all: dongle `0BDA:8179`, one 2.4 GHz WPA2-PSK (CCMP) mesh router, channel 1, DHCP, the Wireless LAN
Monitor and `wpa_supplicant` shipped with ArcaOS. Version 0.1 had only been tried in VirtualBox with xHCI and did
not work on the EHCI PC (see CHANGELOG, 0.1.1).

Not tested: OHCI/UHCI (USB 1.1) controllers, other routers (WPA/WPA2 mixed mode with TKIP group cipher is a
known risk), other dongles, OS/2 Warp 4.5 / eComStation, hidden SSIDs, open networks.
One Android phone hotspot was tried with 0.1: the driver joined it, but the first attempts got no DHCP answer
(the handshake-reply fix in the 0.1 changelog and the EHCI fix in 0.1.1 may resolve this; not re-tested).

## Please report

If you try the driver, a short report helps a lot: ArcaOS/OS/2 version, USB controller (and whether the kernel is
SMP), dongle brand and USB id, router model and security mode, what worked, and the driver's `/V` boot messages.
For problems, also a COM1 trace from a `TRACE 1` build (see ARCHITECTURE.md, section 7) is very valuable.

## Added in 0.1.3: TP-Link Archer T2U Nano (RTL8811AU / RTL8821AU)

* USB id `2357:011E`, 802.11ac 1T1R (the driver uses it as 2.4 GHz 802.11g/n, 20 MHz).
* Endpoints: bulk IN `0x84`, bulk OUT `0x05` / `0x06` / `0x08` / `0x09`, interrupt IN `0x87`. The driver uses one
  bulk-IN request, bulk OUT `0x08` for data and `0x09` for management frames, and sets bit 4 of register
  `0xFE55` to 0 (the interrupt-IN endpoint exists; with the bit set the chip sends no receive data).
* Needs the firmware file `RTL8821AU.FW` (30,880 bytes) copied into the driver after every boot.
