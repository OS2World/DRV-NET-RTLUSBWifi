# Changelog

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
