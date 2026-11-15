# Changelog

## Unreleased

### RTL8821AU
* The dongle now joins as an HT (802.11n, 20 MHz, one stream) and WMM client, so the access point can send to it at HT
  rates (MCS 0-7, up to 72 Mbit/s) instead of 54 Mbit/s at most. The association request carries an HT capabilities
  element (short guard interval at 20 MHz, no 40 MHz, no LDPC / STBC, maximum A-MSDU 3839) and the WMM information
  element; access points use HT rates only towards QoS clients. Our own transmissions are unchanged (legacy rates,
  non-QoS data frames).
  * Block Ack (ADDBA) requests from the access point are declined (status 37): receiving A-MPDUs needs a reorder
    buffer that is not written yet. The decline goes out within milliseconds from the NDIS context hook, through an
    asynchronous TX slot; the first test answered from the ~1 s request-path tick, and downloads fell to 1.5 MB/s on
    both bands (an access point may hold a traffic identifier's frames while it waits for the answer, and the first
    request comes with the association, ahead of the 4-way handshake). The tick remains as the fallback.
  * A-MSDU frames are received (802.11n requires it): each subframe goes to the protocol stack. Only in protected
    frames; a first subframe whose address reads as an LLC/SNAP header is dropped (FragAttacks, CVE-2020-24588).
  * QoS data frames with the Order bit set skip their 4-byte HT Control field.
  * `R21_HT 0` in `rt_usb.h` leaves both elements out (the old association request). New read-out line:
    `HT= GRANT= BA= FAST= BAMS= TID= AMSDU=` (GRANT: 1 = the association response has HT capabilities, 2 = HT
    operation, 4 = WMM parameters; BA: requests / declines sent / failed; FAST: declines sent from the context hook;
    BAMS: request to decline, the last one, ms; AMSDU: frames / subframes / dropped).
* Block Ack receive (A-MPDU), on (`R21_BA 1`; 0 declines as before). The access point's ADDBA request is accepted, so
  it can send A-MPDUs: several frames per channel access with one Block Ack, which the chip sends by itself. The
  answer keeps the request's policy and TID, turns A-MSDU-in-A-MPDU off, and limits the buffer to `R21_BA_WIN` (8)
  frames; the HT capabilities now advertise a maximum A-MPDU of 8 KB (`R21_AMPDU_PARAM`), about 5 full frames, so a
  burst fits the chip's receive buffer. There is no reorder buffer: frames go up as they arrive, and for a TID with an
  agreement the duplicate filter remembers the last 16 sequence numbers instead of the last one. A DELBA from the
  access point ends the agreement. New read-out line `BAACC= BAON= BARX= OOO= GAP= DUPW= DELBA=` (switch, TIDs with
  an agreement, their frames, frames out of order, sequence gaps, duplicates caught, DELBAs received).
  First test (5 GHz, `R21_BA 1`): three agreements were set up, but no frame arrived on an agreed TID, the access
  point sent plain (non-QoS) data at MCS 4-5 and the download was slower (1.91 MB/s), hence the default. A second
  read-out line `AGG ALL= ME= QOS= PLAIN= TID=` counts frames that came in an A-MPDU (receive descriptor PAGGR bit;
  any address / to us), data frames to us by kind, and QoS data frames to us per TID.
  Second test (`R21_BA 1`): nearly every download frame came in an A-MPDU (QoS, TID 0, MCS 7, no DELBA), so the
  agreement works, but 4% arrived out of order and the download was slower than with the agreement declined (2.08
  against 2.35 MB/s): TCP takes reordered segments for loss. Hence a reorder buffer for TID 0 (IEEE 802.11-2016
  10.24.7): frames ahead of a missing one are held, up to 8, and go up in sequence order once it arrives. With no
  timer in the driver, held frames are also released (skipping the gap) when the window has to slide, when the
  access point sends a Block Ack Request, and when a frame arrives while the oldest held one has waited more than
  `R21_RO_MS` (20 ms at first, now 100). The held frames live in 12.8 KB of physical memory allocated at INIT, outside the data
  segment, which is nearly full. A release pauses while no NDIS receive buffer is free and resumes with the next
  frame. New read-out line `RO HELD= REL= SKIP= STALE= BAR= LATE= DROP= MAX= WAIT=`.
  Third test (5 GHz, reorder buffer on): 2.99 MB/s, the best so far (Windows 3.4-3.8). Every held frame went up
  in order (`HELD=REL=3929`, no skips, no stale releases), but releases waited 48 times for a free NDIS receive
  buffer and the delivery queue reached all 6. So the driver now has 8 NDIS receive buffers (`RXP_N`, was 6) and
  6 USB transmit slots (`USB_TXSLOTS`, was 8; all transmit slots were never busy in any test), which leaves the
  data segment about 200 bytes smaller. With 8 the delivery queue index also no longer jumps when its 16-bit counter wraps.
  Fourth test (`ba-ro8`): 3.03 MB/s on 5 GHz (`WAIT=0`, all 8 receive buffers used), but 2.26 MB/s on 2.4 GHz,
  against about 2.5 without the agreement. On 2.4 GHz (a busier channel: `XRQAVG` 31 against 8) 17.6% of the frames
  came out of order, and the access point can't send more than the window (8) past a lost frame until it is
  resent. Hence a 16-frame window (`R21_BA_WIN`; the reorder buffer follows it, 25.6 KB of physical memory) and a
  16 KB maximum A-MPDU (`R21_AMPDU_PARAM` 0x01, about 10 full frames; the chip's receive buffer is 16 KB). The
  6 USB transmit slots ran out 3 times in that download (`NTXNOSLOT`), so there are 7 again.
  Fifth test (`ba-w16`): 3.07 MB/s on 5 GHz and 2.35 on 2.4 GHz. The window is used (`MAX=15`), but a release of
  up to 15 frames is more than the 8 receive buffers: releases waited again (`WAIT` 469 and 910) and frames that
  arrived in order with no buffer free were dropped (`DNDROP` 26 and 46). Now the context hook, once it has
  delivered the queued frames and so freed their buffers, resumes a paused release itself instead of leaving it to
  the next frame (`rtl8188eu_ro_kick`, under a receive path lock against the USB completion, with interrupts off),
  and an in-order frame that finds no buffer is held in the reorder buffer instead of dropped. New read-out fields
  `KICK=` (frames the hook sent up) and `FULL=` (in-order frames held for want of a buffer).
  Sixth test (`ba-kick`): 3.29 MB/s on 5 GHz and 2.89 on 2.4 GHz, with no frame dropped for want of a buffer. A
  slower 2.4 GHz run (about 2.5, dropping to 1.9) showed our own frames queuing in the chip for up to 97 ms (a busy
  channel), and every gap the reorder buffer gave up on arrived later after all (`LATE=14`, `SKIP=14`). So a missing
  frame is now waited for up to 100 ms (`R21_RO_MS`, was 20), the value Linux mac80211 uses.

## 0.1.4a4 (2026-11-09)

Alpha: 0.1.4a3 plus a change to the boot messages only. No change to the driver's behaviour.

### Both drivers
* The `/V` boot messages are shorter than the 80-column screen and every line starts with the driver name (`RTL8821AU:` /
  `RTL88EU:`): the long ones are split into two lines instead of wrapping in the middle of a word. The line telling you how to
  load the firmware (`copy /b ...`) is gone from the RTL8821AU messages; STARTUP.CMD / the install guide still carry it.

## 0.1.4a3 (2026-11-08)

Alpha: 0.1.4a2 plus the changes below. The DHCP read-out fix, the 5 GHz transmit power and the IQ calibration, and the build tag
come from Paul Smedley's pull requests (his repository psmedley/DRV-NET-RTLUSBWifi, merged 2026-11-08).

### RTL8188EU
* Unencrypted (open) networks, ported from the RTL8821AU driver (same changes in `rtl_rx.c` / `rtl_nic.c`): an open join sends
  no privacy bit or RSN element, installs no keys, sends data frames unprotected and accepts unprotected data. New read-out
  counter `OPEN`. **Build-tested only: not yet run on an RTL8188EUS dongle.**

### RTL8821AU
* Unencrypted (open) networks. The driver only knew WPA2-PSK, so a profile without encryption (the Wireless LAN Monitor
  sends AuthMode 2 / AutoSwitch with encryption Disabled) was refused. An open join now sends an association request
  without the privacy bit and RSN element, installs no keys (security off), sends data frames unprotected and accepts
  unprotected data from the access point. New read-out counter `OPEN`. Tested: joins and association on 2.4 and 5 GHz
  against a hotel network; the data path through a captive portal is not verified yet.
* The DHCP lines (`STATUS=`, `IP=`, `MASK=`, ...) come first in the `\DEV\WRND32$` read-out. With the larger scan table
  of 0.1.4 the diagnostics filled the 3900-byte text buffer, the lease lines were cut off, and `rtl88eu_ip.cmd`
  logged `no lease:` at every renewal (once even a truncated netmask `255.255`; seen in a forum report, where the
  read-out was exactly 3898 bytes long).
* The `\DEV\WRND32$` read-out has two new lines for the 5 GHz work: `EFPWR2G=` / `5G=`, the transmit power area of
  the eFuse (logical bytes 0x10-0x21 and 0x22-0x39: per channel group base indexes and the OFDM / BW20 differences),
  and `TXAGC`, the power index registers as the chip has them now (0xC20 CCK, 0xC24 / 0xC28 OFDM, 0xC2C HT, 0xC3C /
  0xC44 VHT; one index per rate byte, 0.5 dB steps).
* 5 GHz transmit power from the eFuse. The driver never set transmit power: every channel on both bands used the BB
  table's flat index (OFDM 0x30). On a 5 GHz channel the OFDM and 1SS HT/VHT indexes now come from the eFuse as in
  Realtek's drivers (base index of the channel group at logical 0x22-0x2F plus the OFDM or BW20 difference at 0x30; the
  Linux build has the by-rate table off, so one index per section). 2.4 GHz keeps the table values, and so does a
  dongle whose eFuse has no 5 GHz power data. `R21_TXP5` in `rt_usb.h` (and the debug command `txp 0` / `txp 1` in
  admin builds) switches it off. New read-out line: `TXP5= GRP= OFDM= HT=` (0 = table values). The debug command `txp`
  used to call the 8188EU power routine, which writes 8188EU register addresses.
* IQ calibration on 5 GHz joins, run by the firmware. The driver never calibrated the radio's IQ balance. As the Linux
  driver does for an 8821A without Bluetooth coexistence, the driver now sends the firmware command 0x45 (channel,
  band, 20 MHz, external 5 GHz PA / LNA flags from the eFuse) after tuning to the access point's channel and waits up
  to 500 ms for the firmware's "finished" report (C2H 0x11); the firmware writes the correction registers itself. This
  adds the driver's first firmware-command (H2C mailbox) code. `R21_IQK` in `rt_usb.h` (debug `iqk 0|1|2` in admin
  builds): 0 off, 1 on 5 GHz joins (default), 2 on every join. The read-out counters `IQOK` (finished), `IQTX` (sent),
  `IQTY` (no report in 500 ms), `IQRX` (last duration, ms), `IQRY` (last channel) now mean this, and a new line
  `IQK= C2H= H2CBUSY= IQC=` shows the correction registers 0xC10 / 0xCCC / 0xCD4 before and after the last run.
* Build tag. `DRV_BUILD` in `rt_usb.h` names the change set a test build comes from; the boot message shows it with the
  compile date and time, the read-out has a new line `BUILD=<version>-<tag> <date> <time>` after the DHCP lines, and the
  BLDLEVEL description in `rtl88eu.lnk` carries it too (kept in step by hand). `DRV_VERSION` moved from `rt_strat.c` to
  `rt_usb.h`.

## 0.1.4a2 (2026-11-06)

Alpha: 0.1.4a1 plus the changes below. Makefiles contributed by Andy Willis (pull request #2).

### RTL8821AU
* The scan table holds 24 access points. The scan visits 2.4 GHz first, so in a crowded neighbourhood the table was full before the
  5 GHz channels were reached and every 5 GHz access point was dropped silently (for example a router's 5 GHz radio on channel 149). When the table
  is full, a stronger access point now replaces the weakest one, never one that matches the SSID to join.
* The NIF has `CopyFile = MACS\RTL8821AU.FW`, so the Network Adapters and Support installer copies the firmware file with the driver.
* Band preference (`PREFER_5GHZ` in `rt_usb.h`, on): with a dual-band router that uses one SSID on both bands, the driver chooses the
  access point (the Wireless LAN Monitor joins by SSID only). 5 GHz reaches 15-20 dB less far than 2.4 GHz, so a plain "strongest
  wins" never picks it: a 5 GHz access point of the SSID with a signal of -65 dBm or better wins over any 2.4 GHz one, and one between
  -75 and -65 dBm gets a 10 dB bonus. A failed 5 GHz join falls back to 2.4 GHz at the next attempt. Joining 5 GHz works on the author's setup (WPA2-PSK,
  channel 149, 1.7 MB/s). Set `PREFER_5GHZ` to 0 and rebuild to always take the strongest access point.
* Debug build only: the read-out lists the 5 GHz entries of the scan table even beyond its first 14 lines; `p5 1` switches the
  band preference on.
* **Fix: "IP address assigned, nothing gets through" (reported on the forum, xHCI machine).** The protocol stack copies the adapter
  address when it binds, which can happen before the dongle answers. It then holds the driver's placeholder address
  (`02:88:E0:00:00:01`) while the radio used the real address from the dongle's eFuse: the driver's own DHCP worked, but every
  ARP reply from the router was addressed to a station the access point did not know. The radio now keeps the stack's address in that
  case, as the 8188EU driver always did. New counter `MACLATE` (1 = that happened). `DBG_LATE_MAC` in `rt_usb.h` (debug builds)
  forces the case for testing.

### Both drivers
* Open Watcom `wmake` makefiles: `wmake -f rtl88eu.mak` in `rtl8188eu/src`, `wmake -f rtl8821au.mak` in `rtl8821au/src`. They
  produce the same binaries as `build.cmd` (byte for byte, apart from the compile date in the boot message). Contributed by
  Andy Willis.
* The boot message and the BLDLEVEL string say 0.1.4a2.

## 0.1.4a1 (2026-11-02)

Alpha: built from the 0.1.3 sources plus the changes below. Contributions by Paul Smedley (the 5 GHz frequency report and the transmit
watchdogs, posted on the OS2World forum) and Lars (the firmware `CopyFile` line in the NIF). See PROVENANCE.md.

### RTL8821AU
* 5 GHz networks are now reported with their real channel frequency (e.g. 5180 MHz for channel 36) and network type
  `Ndis802_11OFDM5`, both in the scan list (`OID_802_11_BSSID_LIST`) and for the joined network
  (`OID_802_11_CONFIGURATION`, `OID_802_11_NETWORK_TYPE_IN_USE`). Before, every 5 GHz access point was reported as
  2.4 GHz channel 1 (2412 MHz), so `wpa_supplicant` could not tell the two bands of a dual-band router apart, its
  band preference never chose 5 GHz, and `freq_list=` could not select a band.
* Transmit watchdog. When a frame handed to the dongle has not completed for a second (the dongle has stopped taking
  frames), new frames are dropped at once instead of the driver waiting for a free slot and then answering "out of
  resource", which made the TCP/IP stack retry in a tight loop and could freeze the whole machine until the dongle was
  unplugged. After two seconds the stuck bulk-OUT requests are cancelled from the GenMAC request path (task time), so
  transmission resumes without unplugging. New counters in the device read-out: `NTXSTUCK` (frames dropped this way)
  and `TXUNSTICK` (cancels done); a `^` on the debug port marks each cancel.

### RTL8188EU
* The boot message and the BLDLEVEL string say 0.1.4a1 (the 0.1.3 package still said 0.1.2).
* The same protection. The 0.1.3 driver already had it, switched off (`RT_TX_RECLAIM 0` in `rt_usb.h`), because
  cancelling the stuck requests from inside TransmitChain did not free the slots; dropping frames while the transmit path
  is stuck was switched off with it. Dropping now has its own switch, `RT_TX_DROP_STUCK`, which is on, so a stuck
  transmit path can no longer make the protocol stack retry in a loop. The cancel runs at task time (the GenMAC request
  path) under the TX queue lock and rewinds the endpoint's data toggle. The existing counters show the effect: `NDDROP`
  (frames dropped) and `TXRECL` (cancels done).

## 0.1.3 preview (2026-10-28)

### New: RTL8821AU driver (TP-Link Archer T2U Nano, `2357:011E`)

* Ported from the Realtek Linux `rtl8821au` driver onto the 0.1.2 platform (device header, USB layer, NDIS MAC, GenMAC
  OID layer, DHCP client, join state machine). Chip layer: power on, firmware download (30,880 bytes, loaded at task
  time through `\DEV\WRND32$`), MAC/baseband/AGC/RF tables, channel switch, eFuse (MAC address), receive and transmit
  descriptors, hardware CCMP key table.
* Found while bringing it up: bit 4 of register `0xFE55` must be 0 on this dongle (it has an interrupt-IN endpoint);
  with 1 the chip never sends receive data on the bulk-IN endpoint.
* USB receive uses a single 2 KB bulk-IN request (more than a few outstanding requests stopped reception in the VM);
  the bulk toggle is computed from the packet count of each completion.
* Receiver gain (IGI, baseband `0xC50`) follows the signal strength of the access point (target = signal + 90 dBm,
  limited to `0x20..0x3A`, steps of 4, every 2 s; scans use the most sensitive setting). Without it a strong
  access point (-30 dBm) overloaded the receiver: 45% retries, 0.65 MB/s.
* De-authentication / disassociation frames from the access point are honoured (a 26-byte frame was rejected by a
  `>= 28` length test, because this chip delivers frames without the FCS); the Wireless LAN Monitor then
  re-associates by itself.
* The dongle LED: slow blink while searching, steady on when connected, flicker on traffic (GPIO8 through register
  `0x60`, as in the Linux driver).
* A pairwise key that the supplicant installs a second time (an access point repeating message 3) no longer restarts
  the packet numbers.

### SMP fixes (both drivers)

* `rt_lib.asm`: `rt_cpu_id` (CPUID APIC id), `rt_lock_try / rt_lock_acquire / rt_lock_release` (owner-tagged
  `cmpxchg` locks), `rt_wait_zero` (bounded `PAUSE` spin), `rt_atomic_dec_nz`.
* One synchronous USB transfer at a time (`usb_xfer_lock`): the control and bulk transfers share one request block,
  one setup packet and one bounce buffer, and wait for their completion while another CPU or task may start a transfer.
  Every register read/write holds it.
* **TX queue lock** (`usb_txq_lock`): the bulk-OUT data toggle and the CCMP packet number are decided in the same locked
  step as the submission, so frames reach the dongle in packet-number order (the access point drops a frame whose
  number is not above the last one it accepted) and the EHCI data toggle follows the real order. The lock keeps
  interrupts enabled (a version that disabled them across the call into the USB stack froze an xHCI machine
  completely) and waits at most a few milliseconds; a refused frame returns "out of resource" and the protocol retries.
* **Indications into the protocol stack** (NDIS `IndicationOff` / `IndicationOn`): the receiver and the
  protocol's critical sections exclude each other through an owner word (CPU id + 1) and a locked counter; a call
  from inside our own upcall is recognised by the owner and never waits for itself; one receive descriptor per
  buffer (the protocol may use it after `ReceiveChain` returns "wait for release").
* **Transmit**: a frame from the stack is gathered straight into its own TX slot (no shared gather buffer); the headers
  are written in front of it in place; the GDT selector used for physical data blocks is taken under an owner lock.
  Frames the driver sends itself (DHCP, protected EAPOL replies) use the same path.
* The receive accumulator is only touched by the USB completion; tasks request a reset through a flag. The EAPOL
  queue counter is atomic. The request lock of the OID layer is an atomic test-and-set.
* Received frames reach the stack from a context hook at task time (this was already in 0.1.2 for the 8188EU).
* New diagnostics in the read-back text: `TXQRE`, `TXQTMO`, `CPUTX`, `INDTMO`, `INDRE`, `CPURX`.

### Packaging

* The 8821AU driver file is `RTL8821AU.OS2`; its boot messages are prefixed `RTL8821AU:`.
* Release builds restrict the debug channel to read-only commands.

## 0.1.2 (2026-10-22)

See `docs/` and the 0.1.2 notes: first build without IBM code or libraries (Open Watcom + NASM).
