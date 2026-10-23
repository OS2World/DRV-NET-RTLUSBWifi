# Changelog

## 0.1.2 - 2026-10-21 (open toolchain, no IBM code)

The driver is now built with **open-source tools only** and contains **no IBM DDK code**.

**Changed**
* Build: Open Watcom C 2.0 (16-bit `wcc`/`wlink`) and NASM replace the IBM DDK's MS C 6.0, MASM and LINK. No IBM
  library or header is used (`usbmisc.lib`, `dhcalls.lib`, `rmcalls.lib`, `os2386.lib` are gone). See `BUILD.md`.
* The files adapted from the IBM Device Driver Source Kit (`strat.c`, `rtl_hdr.asm`, `os2_usb.*`, `rtl_ndis.*`)
  were replaced by new files written from the published specifications (PDD Reference, USB stack programmer's
  reference, USB 2.0, NDIS 2.0.1): `rt_hdr.asm`, `rt_lib.asm`, `rt_end.asm`, `rt_os2dd.h`, `rt_strat.c`, `rt_usb.*`,
  `rt_ndis.*`, `rt_ndisx.asm`. Every number taken from a document is listed in `PROVENANCE.md`.
* Every source file is now GPL-2.0-only (the IBM notices are gone with the IBM files). `LICENSE-AUDIT.md` was rewritten.
* The 802.11 duplicate-frame filter (retransmissions with the Retry bit and an unchanged sequence number are dropped)
  was added: duplicate segments made TCP's receiver send duplicate acknowledgements and slowed the sender down.

**Changed (NDIS)**
* `IndicationOff` / `IndicationOn` now follow the NDIS 2.0.1 specification: they return with interrupts disabled and
  `IndicationOff` waits (bounded) for an indication that is running on another CPU. This did not cure the SMP
  trap described under *Known limitation*, but it is what the specification requires.

**Tested**: see the test matrix in `docs/TESTING.md`.

**Known limitation (not new in 0.1.2, found by a long soak test)**: on an SMP kernel with two or more CPUs,
sustained heavy traffic over the WiFi adapter (about 5 to 20 minutes of continuous large downloads) can trap the
TCP/IP kernel modules (`AFINETK`, `SOCKETS`, always on the second CPU). 0.1.1 behaves the same. With one CPU the same
test ran for 50 minutes (92 downloads of 20 MB, about 550 KB/s, no trap). **Workaround:** limit ArcaOS to one CPU with
`PSD=ACPI.PSD /CPUMASK=1` in `CONFIG.SYS`. Short use (browsing, a few downloads) did not trigger it. The cause is being
investigated (suspected: our receive/transmit upcalls into the protocol stack are not serialized against the stack on a
second CPU).

## 0.1.1 - 2026-10-19 (fixes found on real hardware)

The first real-hardware tests (a PC with an EHCI USB 2.0 controller and an SMP kernel) found three bugs that
the VirtualBox xHCI setup of 0.1 could not show. With them fixed the driver now joins and holds a WPA2-PSK
connection on EHCI and xHCI controllers, on real PCs and in VirtualBox (xHCI and EHCI).

**Fixed**
* **Bulk transfers on EHCI controllers.** USBD does not track the USB data toggle (DATA0/DATA1) for a class
  driver: every bulk request has to carry the toggle of its first packet (`USRB_FLAGS_DET_DTGGLEON`) and the
  EHCI driver takes it from the request. 0.1 always started on DATA0, so the dongle discarded every second
  frame it was sent as a retransmission and the host discarded received packets with the wrong toggle: the
  access point almost never got an authentication request and the receive stream lost the start of frames.
  xHCI drivers keep their own toggle state and ignore the flag, which is why 0.1 worked there. The driver now
  keeps a toggle per endpoint and direction, starting at DATA0 for a newly attached device.
* **Unplugging any other USB device reset the driver.** USBD tells every registered driver about every detach.
  0.1 reacted to all of them, dropping the link, the keys and the receive request when, for example, a USB
  Ethernet adapter or a flash drive was removed. The driver now ignores detach messages for other devices.
* **Traps in `USBD` (TRAP 000E, NULL pointer) on SMP kernels.** All calls to USBD shared one static request
  packet; the task and the completion callback (which re-submits the receive request) could use it at the same
  time, so USBD received a call whose parameter pointer had been cleared. Each call now claims its own packet
  from a pool with an atomic test-and-set; transmit slot allocation and the busy counter are atomic as well.
* Handshake polling is faster (20 ms steps), so the association request follows the authentication reply
  quickly.

**Added**
* Diagnostics: `copy \DEV\WRND32$ file` now also returns counters (join result and reason, authentication /
  association status, USB controller and endpoint data, receive and transmit counters, a timeline of the last
  join). Useful for bug reports; see docs/ARCHITECTURE.md, section 7.
* Debug command `dump <addr>` lists 64 chip registers (`F000`..`F0C0` = the RF registers).
* Commands that transmit or re-initialise (`join`, `pr`, `txa`, `mrate`, `warm`, `cdis`, `in`) are only
  available in `DBG_ADMIN 1` builds; they are not in the released binary.

**Tested**: ArcaOS 5.1.1 on two real PCs (EHCI + SMP kernel; xHCI) and in VirtualBox (xHCI and EHCI controller),
WPA2-PSK, 20 forced re-associations in a row, 10 MB download, boot with WiFi as the only network.

**Known limitations**: unchanged from 0.1 (see README), and throughput on real hardware is modest
(about 115 KB/s measured).

## 0.1 - 2026-10-15 (first release, experimental)

First public version.

**Features**
* WPA2-PSK (CCMP) connection with hardware encryption, scanning, signal strength, signal-based rate selection.
* GenMAC-compatible interface: the ArcaOS Wireless LAN Monitor lists the dongle, scans, creates WPA2 profiles and
  connects; `wpa_supplicant` works as for any other adapter.
* NDIS 2.0.1 MAC: the TCP/IP stack binds the dongle as a normal adapter; the interface number (`lanN`) is derived
  from `PROTOCOL.INI`.
* In-driver DHCP client with a REXX helper script that applies address, default route and DNS.
* MAC address read from the dongle's eFuse; activity LED (slow blink searching, steady on joined, flicker on traffic,
  off at shutdown).
* Boot messages in the usual style (`/V`, `/Q`, `/W`, `/AA`), BLDLEVEL signature.

**Robustness and security found and fixed during testing**
* Unplugging the dongle clears the link, keys, queues and transmit slots; requests no longer wait on a missing device.
* A chip that fails to start (already running from an earlier session) is powered down and started once more.
* Data frames that are not encrypted are rejected after the key handshake (previously accepted).
* 4-way handshake replies are sent unencrypted as the standard requires (previously a retry after a lost reply was
  encrypted, which made some access points hang up the data path).
* A re-association to the same access point is made visible to the supplicant (previously it stayed in
  `ASSOCIATING` forever).
* The temporary copy of a key is wiped after use; register-writing debug commands are a build option.

**Known limitations**: see README (WPA2-PSK only, no hidden SSIDs, one tested configuration, a chip that is
re-attached without a power cycle may need a physical re-plug) and SECURITY.md.
