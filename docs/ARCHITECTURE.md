# Architecture - RTL88EU for OS/2 and ArcaOS

This page is for people who want to read, debug or extend the driver. For installation see the README.

## 1. Big picture

```
 Wireless LAN Monitor (XWLAN)        wpa_supplicant            TCP/IP stack (MPTN)
   genmac.dim  \                      (driver_genmac)             |  NDIS 2.0.1
                \  DosDevIOCtl cat 0x99 (GenMAC OIDs)             |
                 +---------> \DEV\WRND32$ <----------+            |
                                |   rtl_nic.c        |            |
                                |   (OID layer)      |   rt_ndis.c (MAC, TransmitChain/ReceiveChain)
                                v                    v            v
                         rtl_rx.c  - 802.11 station: scan, auth/assoc, EAPOL queue, keys, data TX/RX,
                                     rate selection, LED          rtl_dhcp.c - in-driver DHCP client
                                |
                                v
              rtw_reg.c / rtl_init.c / fwdl.c / rtl_mac|bb|rf|iqk.c  - register access and chip bring-up
                                |
                                v
                          rt_usb.c   - USBD client (IDC), control + bulk transfers (async RX/TX)
                                |
                                v
                      USBD.SYS -> host controller -> RTL8188EU dongle
```

The driver is one 16-bit OS/2 physical device driver (PDD, character device `WRND32$`). It plays two roles:

* **GenMAC-compatible device.** `genmac.dim` (the Wireless LAN Monitor) and `wpa_supplicant` talk to it with
  `DosDevIOCtl` category `0x99`, function `0x40` (GET OID) / `0x41` (SET OID). The driver implements the NDIS OIDs
  they need, plus private OIDs (see section 6).
* **NDIS 2.0.1 MAC.** At boot it registers with the protocol manager (`PROTMAN$`) so the TCP/IP stack binds to it
  as an ordinary adapter (`lanN`).

## 2. Source map

| File | Role |
|---|---|
| `rt_hdr.asm`, `rt_end.asm` | Device-driver header, segment layout and strategy entry (name `WRND32$`, IDC entry, capability bits); the resident-end markers (linked last) |
| `rt_lib.asm` | Runtime and DevHelp services (ProcBlock/ProcRun, VerifyAccess, GDT selectors, AttachDD, Save_Message, IDC call, memory and atomic helpers, COM1 byte output) |
| `rt_os2dd.h` | OS/2 driver types: request packets, status codes, DevHelp wrapper prototypes |
| `rt_strat.c` | Strategy routine (Init, InitComplete, Open, Read, Write, GenIOCTL, Shutdown), boot messages and switches (`/V /Q /W /AA`) |
| `rt_usb.c/.h` | USB structures and USBD client: registration, attach/detach (IDC), synchronous control transfers, asynchronous bulk RX (one request always outstanding) and TX (4 slots) |
| `rtw_reg.c` | Register primitives (vendor control requests), eFuse reader (MAC address), `hw_init`, `probe` with warm-chip retry |
| `rtl_init.c` | Power-on sequence, queue/page setup, card-disable (power-down) |
| `fwdl.c`, `fwimg.c` | Firmware download to the on-chip 8051 and the firmware image |
| `rtl_mac.c`, `rtl_bb.c`, `rtl_rf.c`, `rtl_iqk.c` | MAC/baseband/RF register tables and IQ calibration |
| `rtl_rx.c` | The 802.11 station logic (largest file), see section 4 |
| `rtl_nic.c` | GenMAC OID layer and the NDIS-OID answers, shutdown/detach hooks |
| `rt_ndis.c/.h`, `rt_ndisx.asm` | NDIS MAC tables, bind, TransmitChain/ReceiveChain, indication on/off, interface number detection; the assembler stubs load DS and adapt the Pascal calling convention |
| `rtl_dhcp.c` | DHCP client; returns the lease as text when `\DEV\WRND32$` is read |
| `rtl_dbg.c` | Debug command interpreter (text written to `\DEV\WRND32$`) |

## 3. Constraints of the 16-bit driver model (worth knowing before editing)

* Built with Open Watcom `wcc -2 -ms -zu -zl -zp1 -j` (80286 code, no default libraries): **no C runtime**. Use `-2`, not
  a 386 target: 32-bit register halves must not be touched by 16-bit driver code called from 32-bit modules. No 32-bit shifts, multiplications or divisions (the
  compiler would call library helpers that do not exist). 32-bit values are split through unions of two
  `USHORT`/four `UCHAR`; 32-bit add, subtract and compare are fine.
* **SS != DS.** Stack data cannot be passed as a near pointer to USBD or DevHelp. Buffers handed to USBD, and
  variables whose address is passed on, are `static`.
* The compiler mis-dispatches some 32-bit `switch` statements (private OIDs are tested by halves) and can hit an
  internal error on `array[i].member` in places (a table of pointers is used for the TX slots).
* The whole driver stays resident; USBD keeps a pointer to the IDC entry. The resident-end markers live in the
  last-linked object (`rtw_reg.obj`).
* Completion handlers run at task time in USBD's context and **must not block**. Blocking work (register access)
  runs from the request path, under `nic_lock`.
* No timers or threads: periodic work (LED) is stepped from the GenMAC request path.

### USB stack rules this driver depends on (learned on real hardware)

* **Data toggle.** Every bulk request sets `USRB_FLAGS_DET_DTGGLEON` if its first packet must be DATA1. The
  driver tracks the toggle per endpoint and direction (`g_tgl_out[]`, `g_tgl_in[]` in `rt_usb.c`): it flips once
  per packet (`ceil(length / max packet)`), starts at DATA0 on attach, and the packet size comes from the
  endpoint descriptor. The xHCI driver ignores the flag; the EHCI driver needs it (the USB stack
  reference documents the flag; the 0.1 driver ignored it and failed on EHCI). The controller returns the toggle of the *last* packet in the completed request's flags; the
  receive path compares it with its own count (`TGLBAD` in the diagnostics stays small on real EHCI hardware; the xHCI driver and
  VirtualBox's emulated EHCI do not report toggles, so the counter is meaningless there).
* **Detach messages are broadcast.** `USB_IDC_FUNCTION_DETDEV` arrives for every device; check controller id and
  address (`USBDetach`) against the dongle before touching any state.
* **Request packets and SMP.** The IDC request packet (`RP_GENIOCTL`) passed to USBD must not be shared between
  concurrent callers. The task, the completion callback and the transmit path call in at the same time, truly in
  parallel on an SMP kernel: each call takes a packet from a pool (`usbd_call`), claimed with an atomic exchange
  (`tas`). Transmit slots are claimed the same way.
* **Max packet size** is read from the endpoint descriptor (512 at high speed, 64 at full speed).

## 4. The station logic (`rtl_rx.c`)

* **Receive.** One bulk-IN request is always outstanding. Each completion appends to an accumulation buffer;
  `rx_parse` splits it into frames using the 24-byte RX descriptor (length, driver-info size, shift), checks the
  length fields, and passes each frame to `note_frame`.
* **Scan.** Beacons and probe responses update the network table (`g_net`, SSIDs stored as raw bytes). A scan is a
  sweep over channels 1-13 (about 2.5 s); while it runs, plain state queries are answered without the request lock.
* **Join** (`rtl8188eu_join`): pick the strongest BSS with the wanted SSID, tune the channel, authentication
  (open system), association with a WPA2-PSK/CCMP RSN element, then hand EAPOL frames to the supplicant.
* **EAPOL.** Received EAPOL frames go to a small queue read by the supplicant through private OID `0xFFFFFF0D`;
  replies come back through `0xFFFFFF0E`. 4-way handshake replies are sent unprotected, group-key replies protected.
* **Keys.** The supplicant adds keys through `OID_802_11_ADD_KEY`; the driver writes them to the chip's CAM.
  The chip encrypts and decrypts CCMP (entry 4 = pairwise key, 0-3 = group keys; `SECCFG = 0xCC`).
* **Data.** Receive: the chip delivers decrypted frames; the driver accepts only protected non-EAPOL data frames
  once the pairwise key exists, converts 802.11 to Ethernet and hands the frame to NDIS (or to the DHCP client).
  Transmit: Ethernet to 802.11 with a CCMP header (packet number), queued in one of 4 asynchronous TX slots.
* **Rates.** Management and handshake/ARP/DHCP frames use 1 Mbit/s CCK; data frames use a signal-based ladder
  (36 down to 6 Mbit/s OFDM, then CCK) with hysteresis, re-evaluated every 32 frames.
* **LED.** Register `0x4E`: `0x20` on, `0x28` off. Slow blink while searching, steady on when joined, a short
  flicker on traffic.

## 5. Chip bring-up (order matters)

1. read the eFuse MAC (cold state), 2. power-on sequence, 3. queue/page setup, 4. firmware download and boot,
5. MAC table, 6. baseband and AGC tables, 7. RF table, 8. receive enable, 9. IQ calibration, 10. transmit setup.
If the sequence fails, `probe` powers the chip down (`rtl8188eu_card_disable`) and retries once. A chip that was
re-attached without losing power can still refuse to start its firmware; a physical re-plug fixes that.

## 6. GenMAC interface (what the monitor and supplicant expect)

* IOCTL parameter block: four ULONGs: OID, data length, bytes returned, **NDIS status**. The monitor's DIM looks
  at the fourth word for errors, so it must be filled in.
* Private OIDs: `0xFFFFFF00` interface number (position of the binding in `PROTOCOL.INI`), `01` is wireless
  (always 1), `02` link status (bit 1 = connected), `08` NIF file name (the monitor reads the title from it),
  `09` GenMAC version (**>= 2.00** makes the monitor offer WPA), `0B` start EAPOL passing, `0C` EAPOL frames
  waiting, `0D` next EAPOL frame, `0E` send an EAPOL frame.
* Encryption status reads back "enabled" (4/6) only once the pairwise key is installed; before that it reads the
  matching "key absent" value (5/7). Both the monitor and the supplicant depend on this.
* The network joined is the SSID the supplicant or monitor sets through `OID_802_11_SSID`. A random 32-byte SSID
  containing control bytes is the supplicant's "disconnect" trick and only makes the driver leave.

## 7. Debugging

* **COM1 trace.** Set `#define TRACE 1` in `rt_usb.h` and rebuild. One-letter markers describe progress (the
  boot trace `I A a R K S C ...`, init steps, `J`/`L` for join/leave, `[eapol ...]`, `[key ...]`). The VirtualBox
  setup captures COM1 to a file. `TRACE_DATA 1` adds a per-frame trace (slow).
* **Debug channel.** Write a text command to `\DEV\WRND32$`, the reply appears on COM1:
  `r <addr>` / `r2` / `r4` read registers, `e <addr> [n]` reads eFuse bytes, `rate <code>|ff`, `ra`, `ctl <rate>`
  (rate for handshake/ARP/DHCP frames), `n` (network list), `txp <ch>`. With `DBG_ADMIN 1` also `w`/`w2`/`w4`
  (register writes), `warm` (re-initialise over a running chip), `cdis` (power down), `in`.
  Use a space before `>` in `echo r 0100 > \DEV\WRND32$`.
* **DHCP read and diagnostics.** `copy \DEV\WRND32$ file` runs one DHCP exchange and returns `KEY=VALUE` lines:
  first `LAN` and the diagnostics, then `STATUS`, `IP`, `MASK`, `ROUTER`, `DNS`, `LEASE64`. Diagnostics: `JFAIL`
  (reason of the last failed join: 1 no network with that SSID in the scan table, 2 no authentication reply,
  3 authentication refused, 4 no association reply, 5 association refused, 6 wrong security mode), `JAUTH` /
  `JASSOC` (status codes, 65535 = no reply), `JCH`, `JRSSI`, `JNNET`, `JTRIES`; receive counters `RXFRM`,
  `RXCRC`, `RXDESYNC`, `RXERR`, `FEEDS`; transmit `TXOK`, `TXFAIL`; `TOME` frames addressed to us, `ACK` link-level
  acknowledgements seen during a join; USB `UCTRL` (controller), `UPORT`, `MPSIN`/`MPSO2`/`MPSO3` (max packet
  sizes), `NEP`, `EPA`..`EPD`; `RXRUN`, `RXFAIL`, `RXRC`, `RXSTAT`, `RXLEN`, `RXDONE` (receive engine); `TGL*` (data
  toggle); `EV=` a timeline of the last join (`01` authentication sent, `02` association request sent, `03` send
  returned, other values = frame-control byte of a frame addressed to us, `D4` = ACK, time in ms). `dump <addr>`
  before the read adds 64 chip registers (`F000`, `F040`, `F080`, `F0C0` = RF registers).
* **Simulating the dongle.** `VBoxManage controlvm <vm> usbdetach|usbattach <uuid>` (the UUID changes every time
  the device reappears) exercises detach handling, but it does not remove power from the chip, so it can leave the
  chip in a state that needs a real re-plug.
* **Forcing re-joins.** `wpa_cli.exe -i lanN reassociate` (the monitor's supplicant) while pinging the router.

## 8. Reference material used

The behaviour of the chip comes from the Linux `rtl8188eu` driver (register sequences and tables, GPL v2), from
a USB capture of the Windows driver on the author's dongle (cross-checks), and the GenMAC protocol from the
published source of the Wireless LAN Monitor (Netlabs). See `LICENSE-AUDIT.md` for what was derived from what.
