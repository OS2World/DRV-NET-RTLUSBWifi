# RTL88EU - native Realtek RTL8188EU USB WiFi driver for OS/2 and ArcaOS

Version 0.1.2 (experimental). A 16-bit OS/2 device driver for the common Realtek **RTL8188EU(S)** 802.11n
USB dongles (USB id `0BDA:8179`). It talks to the dongle directly over the OS/2 USB stack and presents it to
the system in two ways:

* a **GenMAC-compatible device** (`WRND32$`), so the stock ArcaOS **Wireless LAN Monitor** (XWLAN) and
  `wpa_supplicant` manage it like any other WiFi adapter, and
* an **NDIS 2.0.1 MAC**, so the OS/2 TCP/IP stack binds to it as an ordinary network interface (`lan1`).

> **Status:** works on the author's test setups (VirtualBox, two real PCs); see [Limitations](#limitations).
> Since 0.1.2 the driver is built with **open-source tools only** (Open Watcom + NASM) and contains **no IBM code**.

## What works

* WPA2-PSK (CCMP/AES) association to an access point, including the 4-way handshake and group keys
  (keys are installed in the chip's hardware key table; encryption/decryption is done by the chip).
* Scanning (the Wireless LAN Monitor lists the networks), signal strength, signal-based rate selection.
* IP over WiFi: DHCP is run by the driver itself (helper script `rtl88eu_ip.cmd`), ping/web/ssh over `lan1`.
* Wireless LAN Monitor integration: device list, profile with WPA2-PSK, connect/reconnect, green status icon.
* MAC address is read from the dongle's eFuse.
* The dongle LED: slow blink while searching, steady on when connected, flicker on traffic.
* Boot messages follow the usual OS/2 driver conventions (`/V`, `/Q`, `/W`, `/AA`).

Tested on: ArcaOS 5.1.1 (kernel 14.204, uniprocessor and SMP; on SMP see the limitation below) with a WPA2-PSK access point on channel 1:

* a real PC with **EHCI** (USB 2.0) controllers and the **SMP** kernel, WiFi as the only network;
* a real PC with an **xHCI** (USB 3) controller, with and without a USB Ethernet adapter;
* VirtualBox with an xHCI controller and with an EHCI controller.

Other chips, other access points and OHCI/UHCI (USB 1.1) controllers are untested.
What was run with 0.1.2 itself is listed in `docs/TESTING.md`.

## Limitations

* **The network is chosen in the Wireless LAN Monitor** (or by `wpa_supplicant`): the driver joins the SSID
  it is asked to join. Only networks that broadcast their name (non-hidden SSIDs) can be joined.
* **A phone hotspot did not work:** the driver joined an Android phone hotspot (WPA2 key handshake completed)
  but the access point kept resending the handshake and answered no DHCP request, so no address was obtained.
  Cause not found yet (suspect: uplink frames at the 1 Mbit/s handshake/DHCP rate are not accepted by that
  access point). The debug command `ctl <hexrate>` (see `src/rtl_dbg.c`) selects another rate for these frames
  for experiments.
* **DNS:** only the first DNS server of the lease is used; it is added to `RESOLV2` and not removed when the link goes
  down (it is replaced by the next lease's server).
* **WPA2-PSK with CCMP only.** No open networks, WEP, TKIP/WPA1, WPA3, 802.1X, or access-point mode.
* 2.4 GHz only (the chip is 1T1R 802.11n); no power saving; throughput is modest
  (400 to 600 KB/s measured for one TCP stream on real hardware at close range; the access point talks to the
  dongle at legacy 802.11g rates because the driver does not advertise 802.11n).
* **SMP kernels: sustained heavy traffic can trap the TCP/IP stack.** With two or more CPUs, 5 to 20 minutes of
  continuous large downloads over the WiFi adapter trapped the kernel modules `AFINETK` / `SOCKETS` on the test PC
  (0.1.1 behaves the same). With one CPU the same test ran 50 minutes without a problem. **Workaround:** limit
  ArcaOS to one CPU by adding `/CPUMASK=1` to the `PSD=ACPI.PSD` line of `CONFIG.SYS`. Normal use (browsing, a
  few downloads) did not trigger it. This is being investigated.
* No automatic reconnect logic of its own: roaming and reconnecting are left to `wpa_supplicant`/the monitor.
  An intermittent failure to connect at boot was seen once; a manual *Reconnect* in the monitor fixes it.
* Only the 16-bit driver is provided; USB 1.1 (UHCI/OHCI) controllers are untested.

## Requirements

* ArcaOS 5.x (or OS/2 Warp 4.5 with a working USB stack): `USBD.SYS` plus your host-controller driver.
* TCP/IP (MPTN) with the NDIS protocol manager (`PROTMAN.OS2`) - the standard install.
* The **Wireless LAN Monitor** package (XWLAN, includes `wpa_supplicant.exe` and `genmac.dim`).
* A Realtek RTL8188EU dongle, `0BDA:8179`.

## Installation

Back up `CONFIG.SYS` and `\IBMCOM\PROTOCOL.INI` first. The paths below assume boot drive `C:`.

1. **Copy the driver:** `bin\RTL88EU.OS2` to `C:\IBMCOM\MACS\RTL88EU.OS2`.
2. **Copy the NIF file** next to it: `install\RTL88EU.NIF` to `C:\IBMCOM\MACS\RTL88EU.NIF`.
3. **CONFIG.SYS:** add one line **after** the USB drivers and after `DEVICE=C:\IBMCOM\PROTMAN.OS2 ...`:

   ```
   DEVICE=C:\IBMCOM\MACS\RTL88EU.OS2
   ```

   Optional switches (case-insensitive): `/V` verbose boot messages, `/Q` completely silent, `/W` pause 30 s
   after all drivers have loaded so the messages can be read, `/AA` accepted (the driver never beeps).
   By default the driver is silent and only prints a message if it cannot load.
4. **PROTOCOL.INI:** merge the entries from `install\PROTOCOL.INI.example`:
   * in `[IBMLXCFG]` add `RTL88EU_NIF = RTL88EU.NIF`,
   * add `RTL88EU_NIF` to the `Bindings =` list of `[TCPIP_NIF]` (e.g. `Bindings = MMLEM_NIF,RTL88EU_NIF`),
   * add the section `[RTL88EU_NIF]` with `DriverName = WRND32$`.

   The driver and NIF live in `\IBMCOM\MACS`, matching the `[File]` entry of the NIF, so *MPTS* will find
   them there. If you run *MPTS* later, re-check these entries and the `CONFIG.SYS` line afterwards.
5. **Address helper:** copy `install\rtl88eu_ip.cmd` to `C:\IBMCOM\RTL88EU\` and start it at boot from
   `STARTUP.CMD` (see `install\STARTUP.CMD.example`):

   ```
   detach C:\OS2\CMD.EXE /c C:\IBMCOM\RTL88EU\rtl88eu_ip.cmd
   ```

   It needs only REXX (no extra tools). It waits for the WiFi link, gets an address from the driver's built-in DHCP
   client and applies it to the WiFi interface (the interface name `lanN` is taken from the driver, which works it
   out from `PROTOCOL.INI`; pass `lanN` as an argument only to override it). It adds a default route only if none
   exists, adds the lease's DNS server to `RESOLV2` (the original is saved once as `RESOLV2.bak_rtl88eu`; the
   entry from the previous lease is replaced), and keeps the lease renewed. Log: `C:\IBMCOM\RTL88EU\ip.log`.
6. **Reboot**, with the dongle plugged in (or plug it in after boot).
7. **Check:** `bldlevel C:\IBMCOM\MACS\RTL88EU.OS2` shows the driver version; `ifconfig lan1` shows the interface.

### Connecting with the Wireless LAN Monitor (recommended)

1. Open the monitor's *Properties* -> *Device*: "Realtek RTL8188EU USB WLAN (OS/2 native)" should be listed.
2. *Add/Edit Profile...* -> create a profile: SSID = your network, *Security* = **WPA2-PSK**, enter the
   passphrase there.
3. **Every** profile's *TCP/IP* tab: **Don't configure** (the helper script supplies the address; the monitor's own DHCP
   client would conflict with the other network interface).
4. Monitor *Properties* -> *TCP/IP* -> *Resolve address range conflicts*: **Keep configuration of other
   interface**. Never choose the option that disables your wired interface.
5. Select the profile / *WPA Supplicant -> Reconnect*. The icon turns green and `lan1` gets its address.

### Without the monitor (plain wpa_supplicant)

`install\rtl88eu_wlan.cmd` (needs `sleep.exe` from the unixroot tools; set `IFACE` at its top to your `lanN`) keeps `wpa_supplicant.exe -i lan1 -c C:\MPTN\ETC\rtl88eu.conf` running. Create
that config file yourself (standard `wpa_supplicant.conf` with `network={ ssid="..." psk="..." }`). Do not run
it together with the monitor's own supplicant.

## Troubleshooting

* **Driver not listed / WiFi never comes up:** unplug the dongle, wait 10 s, plug it in again. The chip must
  start from a cold state; a "warm" chip (re-plugged while the system was still shutting down) can fail to
  initialize.
* **The WiFi interface is `lan0`, not `lan1` (or the helper configures the wrong one):** the stack numbers the
  adapters that are actually present. If an adapter listed before `RTL88EU_NIF` in `Bindings =` (for example a USB
  Ethernet adapter) is not plugged in at boot, the WiFi adapter becomes `lan0`. Start the helper with the right
  name (`rtl88eu_ip.cmd lan0`) or list `RTL88EU_NIF` first in `Bindings =` so that it is always `lan0`.
* **"The address assigned to the wireless LAN device conflicts with lan0":** both adapters are on the same
  network. Set the monitor's *Resolve address range conflicts* option to keep the other interface's configuration
  (or let the monitor's profile use DHCP and answer YES to run on WiFi alone).
* **The dongle's light stays off / "Association request failed" after a system trap:** a crash leaves the chip
  running; unplug the dongle for 10 seconds and plug it in again (a reboot does not power it down).
* **Wrong interface used by the monitor / supplicant:** boot once with `/V`; the driver prints
  `TCP/IP interface: lanN`. N is the position of `RTL88EU_NIF` in the `Bindings =` list of `[TCPIP_NIF]` in
  `PROTOCOL.INI`, counting from 0.
* **Connected but no address:** check `C:\IBMCOM\RTL88EU\ip.log`. The helper retries every few seconds.
* **Boot hangs or errors:** boot with `/V` (and `/W`) on the `DEVICE=` line to read the driver's messages.
* **Debugging:** set `#define TRACE 1` in `src/rt_usb.h` and rebuild to send a detailed trace to COM1.
  A small command channel is always available, e.g. `echo r 00F0 > \DEV\WRND32$` reads a chip register (the
  reply goes to COM1).

## Building from source

Open Watcom C/C++ 2.0 (16-bit `wcc` and `wlink`) and NASM are all that is needed; nothing from the IBM Device
Driver Kit. See `BUILD.md` for the details. In `src\`:

```
build.cmd
```

The result is `src\RTL88EU.OS2` (and `RTL88EU.map`). The build date is compiled into the boot banner, so a rebuild differs
from the shipped binary by that string only. Notes: there is no C runtime (`-zl`, no default libraries), so no 32-bit
variable shifts, multiplications or divisions; stack data is not addressable from the USB stack (SS != DS), so
buffers passed to it are static. The version/BLDLEVEL signature is the `option description` line in `src\rtl88eu.lnk`.

## Layout

```
bin\RTL88EU.OS2       built driver (quiet release build) and its SHA-256
src\                  driver sources, build.cmd, rtl88eu.lnk
install\              NIF, PROTOCOL.INI / STARTUP.CMD examples, address and supplicant helper scripts
docs\ARCHITECTURE.md  how the driver is built and how to debug it
docs\HARDWARE.md      supported and tested hardware, how to report results
SECURITY.md          what is protected, known limits
CHANGELOG.md         what changed
COPYING              GNU GPL version 2
LICENSE-AUDIT.md     where each source file comes from (provenance)
PROVENANCE.md        the documents and numbers the new OS/2 code was written from
BUILD.md             how to build with Open Watcom + NASM
```

## License

GPL version 2 only (see `COPYING`; each source file has an SPDX header). Third-party material:

* Portions are derived from the **Linux rtl8188eu driver**, Copyright(c) 2007 - 2011 Realtek Corporation,
  GPL version 2 (no later-version option) - the reason this project is GPL-2.0-only.
* `src\fwimg.c` embeds Realtek's **firmware** image (`rtl8188eufw.bin`) under Realtek's firmware
  redistribution terms; it is binary data, not GPL source.
* The OS/2 glue (`rt_*`) was written from the published specifications (IBM Physical Device Driver Reference, the
  OS/2 USB stack programmer's reference, USB 2.0, NDIS 2.0.1) and contains no IBM code; nothing from the IBM Device
  Driver Kit is linked. `PROVENANCE.md` lists where every external number comes from.
* `wpa_supplicant` and the Wireless LAN Monitor are separate programs and are not part of this package.

No warranty; use at your own risk. See `LICENSE-AUDIT.md` for the per-file details.
