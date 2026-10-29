# Testing record - preview 0.1.3 (status of 2026-10-28, evening)

Tests were run by the author with the driver on real PCs and in VirtualBox. The standard stress test ("soak") is a
script that repeats: download a 20 MB file from a web server on the development PC over the WiFi link, then send three
pings; every iteration is logged with its time and the ping loss. A second script (the watchdog) pings the router
every 3 s and logs a stall after six misses in a row. A run is clean if the machine neither traps nor freezes and no
stall is logged.

## Machines

| Name | USB controller | Kernel | Used for |
|---|---|---|---|
| ArcaMiix | xHCI (USB 3) | ArcaOS 5.1.1, 14.204 SMP, several logical CPUs | both drivers |
| PC1 | EHCI (USB 2), also UHCI | 14.204 SMP; `/CPUMASK=1` (one CPU) for some runs | 8821AU |
| VirtualBox VM | EHCI or xHCI | uniprocessor | development |

The access point is a WPA2-PSK mesh router on 2.4 GHz channel 1.

## RTL8821AU driver (`RTL8821AU.OS2`)

| Run | Result |
|---|---|
| VM, EHCI: join, DHCP, ping, TCP both ways, 5 forced re-associations | pass (about 170 KB/s, limited by the VM's USB emulation) |
| ArcaMiix, SMP: 199 downloads of 20 MB (4 GB, 55 min) with ping, development build | pass: no trap, no freeze, 0% ping loss |
| ArcaMiix, SMP: 450 downloads of 20 MB (1 h 56 min) with ping and watchdog, release build | pass: no trap, no freeze, no stall; 449 of 450 iterations 0% ping loss (one lost 1 of 3); counters: no AP de-authentication, 1 key installation, no lock timeouts |
| PC1, one CPU: join, 35 downloads | clean (run ended by a reboot to test SMP) |
| PC1, SMP, **first run after a boot** | **failed**: from the PC, pings and downloads to one WiFi neighbour (the development PC) got no answer, while pings to the router and the internet worked and the neighbour could still reach the PC. Not understood. Counters showed no lock timeouts, and ReceiveChain returned a status other than "success / wait" for about 19% of the frames |
| PC1, SMP, after a reboot (build that also records the ReceiveChain status codes; same code otherwise) | **383 downloads of 20 MB over 4 h 27 min on two CPUs: 382 iterations with 0% ping loss, one with 33%, no stall, no trap, no freeze.** The machine stayed up until the script ended 1 h 30 min later (the web server on the test PC had stopped; those last iterations only pinged and are not counted) |

Throughput measured on ArcaMiix: 1.1-1.9 MB/s download. Receiver gain control (see CHANGELOG) fixed a loss of
sensitivity close to the access point (45% retries at -30 dBm before it).

Problems found and fixed during these runs (all described in CHANGELOG.md): a trap in `SOCKETS` (frames were handed
to TCP/IP from the USB completion), a complete freeze (a transmit lock that disabled interrupts across a call into
the USB stack), the access point's de-authentication frames being ignored (a 26-byte frame was rejected as too short),
and a lost-gain problem near the access point.

## RTL8188EU driver (`RTL88EU.OS2`)

| Run | Result |
|---|---|
| 0.1.2 release on PC1, SMP, soak (20 MB downloads + ping) | traps in `AFINETK`/`SOCKETS` within 5-20 minutes, always on CPU 01 (also with 0.1.1) |
| 0.1.2 release on PC1, **one CPU** (`/CPUMASK=1`), 92 downloads, 50 min | no trap |
| 0.1.2 release on ArcaMiix, SMP, soak plus `yum`/`rpm` updates on the same link, 2026-10-28 | 43 iterations (about 65 min) clean, then **trap 000E in `AFINETK`**, CPU 01, `CS:EIP 0168:f0b6a9b6`, `CR2 00000014` |
| **0.1.3 build (this package)** on ArcaMiix, SMP, soak plus `yum`/`rpm` updates over the same link | **189 downloads of 20 MB over 3 h 53 min (with `yum`/`rpm` running for part of it): no trap, no freeze; 152 iterations with 0% ping loss, 34 with 33%, 3 with 66%.** One stall of 18 s at 16:15 that recovered by itself (at that moment every transmit had completed and no lock timeouts were counted). ArcaMiix stayed up and reachable until the script ended nine and a half hours after the start (the test web server had stopped at about 20:00, so the later iterations only pinged). The 0.1.2 driver trapped about 65 minutes into the same load |

## What the counters said (both drivers, new builds)

`TXQTMO` (transmit lock gave up), `INDTMO` (indication wait gave up), `INDDROP`, failed transmits: all 0 in the long
runs on ArcaMiix. `TXQRE` (a CPU re-entered the transmit lock): 1-3 per run. Both CPUs and more (APIC ids 0-3) were seen
running the transmit and receive paths.

## Not tested

5 GHz association; WPA3/WEP/open networks (the 8188EU driver connects to WPA2-PSK; others were not exercised);
more than one dongle at a time; sleep/resume; USB hubs; machines other than those listed.

## Test-rig caveat

The test web server ran on the author's Windows PC, which went to sleep (modern standby) several times in the evening and
stopped serving at about 20:00. Iterations from then on are not counted above: their downloads timed out after 60 s and
their pings to that PC often failed because it was asleep. Some of the 33% / 66% ping-loss iterations in the earlier
hours may also coincide with that PC dozing.
