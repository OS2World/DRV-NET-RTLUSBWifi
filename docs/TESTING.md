# RTL88EU 0.1.2 - test record

Build under test: `bin\RTL88EU.OS2` as tested on the real PCs below: SHA-256
`bc602af8e431c53e6d0a0feff0766046b12a5ecaa59b7652a18fb245576a1134` (built 2026-10-21 with Open Watcom 2.0.1 and NASM 2.11;
flags in `BUILD.md`). The shipped binary is a rebuild of the same sources (SHA-256 `090f3cb37ddefd7058ac6320655246013accc811e7945358f327dfb2a8fe80f8`) that differs
in **two bytes only**: the file-version digit of the BLDLEVEL signature (so `bldlevel` shows 0.1.2) and the build date in the
banner.

## What was tested

| Setup | Test | Result |
|---|---|---|
| VirtualBox, ArcaOS 5.1.1, one CPU, EHCI controller, no dongle attached | boot with the driver | loads without error; `\DEV\WRND32$` answers; NDIS MAC registers with the Protocol Manager |
| Real PC 1: two EHCI controllers + UHCI, ArcaOS 5.1.1 kernel 14.204 **SMP**, WPA2-PSK access point | boot with WiFi as the only network | associates, 4-way handshake, DHCP lease |
| | ping | 6 of 6 |
| | 20 forced re-associations in a row (`wpa_cli reassociate`), each checked for state COMPLETED and the address kept | 20 of 20 (twice) |
| | sustained download, two CPUs (repeated 20 MB transfers from a local web server + continuous ping) | trapped the TCP/IP modules (`AFINETK` / `SOCKETS`, CPU 1) after 5 to 20 minutes - **also with the 0.1.1 driver** |
| | the same, one CPU (`PSD=ACPI.PSD /CPUMASK=1`) | 92 complete 20 MB downloads in 50 minutes at 540 to 600 KB/s, no trap, `RXERR=0 TXFAIL=0` |
| Real PC 2 ("ArcaMiix"): xHCI controller (`USBXHCD`), WiFi only (no USB Ethernet adapter) | boot, join, ping | associates at boot, DHCP lease, ping 5 of 5 |
| | 20 forced re-associations, address kept each time | 20 of 20 |
| | 15 minutes of repeated 20 MB downloads + continuous ping | 11 of 11 downloads complete (224 to 686 KB/s), ping loss about 4 % under load, `RXERR=0 TXFAIL=0`, no trap |

## Not yet tested with 0.1.2

* VirtualBox with the dongle attached (xHCI and EHCI controllers; tested with 0.1.1).
* The `/V` boot messages (message id 1178) and the `/W` pause.
* Other USB devices (mouse, keyboard, USB Ethernet adapter, hubs) binding to their own drivers with this build loaded
  (PC 1 boots with its USB devices and the dongle attached).

## How the soak test was run

A Python web server on the development PC serves a 20 MB random file; a loop on the test PC runs
`curl.exe -o NUL http://<server>:8000/test20mb.bin` repeatedly over the WiFi interface (wired cable unplugged, so
the traffic cannot use another interface) while a continuous `ping` runs from the development PC. After each run the
size and speed are logged. Driver counters are read with `copy \DEV\WRND32$ file`.
