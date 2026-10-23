# Security notes - RTL88EU for OS/2 and ArcaOS (version 0.1)

This driver is experimental. These notes say what it protects, what it does not, and what was reviewed.
They are an engineering review by the authors, not an independent audit.

## What is protected

* **WPA2-PSK with CCMP (AES).** The key handshake is run by `wpa_supplicant`; the driver installs the resulting
  pairwise and group keys in the chip's hardware key table. Encryption, decryption and the CCMP integrity check
  (MIC) are done by the chip; frames with a bad MIC are dropped by the chip.
* **Unprotected data frames are rejected.** Once the connection exists, the driver accepts a data frame only if the
  pairwise key is installed **and** the frame is marked Protected. The only exception is EAPOL (key handshake)
  frames. Before the pairwise key is installed, nothing but EAPOL is accepted. (Found in review and fixed in
  0.1: earlier test builds accepted unprotected frames that claimed to come from the access point.)
* **Handshake replies are sent as the standard requires:** messages 2 and 4 of the 4-way handshake are sent
  unprotected (an access point that has not yet installed the key cannot read them otherwise); group-key replies
  are protected.
* **Key material:** the temporary copy of a key handed to the driver is wiped as soon as it is in the chip. Keys
  are removed from the chip when the link is left.
* **Input handling:** lengths coming from the radio (receive descriptors, information elements, EAPOL length,
  DHCP options) and from callers (ioctl parameter and data buffers, which are access-checked with
  `DevHelp_VerifyAccess`) are bounds-checked before data is copied. Transmit frames larger than the internal buffer
  are refused.

## Known limits (not protected)

* **No 802.11w (Protected Management Frames).** Deauthentication and disassociation frames are not authenticated,
  as with any plain WPA2 client: someone in range can force a disconnect (the supplicant then reconnects).
* **No replay protection of our own.** The driver does not track CCMP packet numbers on receive; it relies on the
  chip. A replay of an old, valid encrypted frame may be accepted. (Transmit packet numbers are incremented per
  frame.)
* **Group traffic can be forged by other stations** that know the network passphrase (inherent to WPA2-PSK: all
  stations share the group key).
* **DHCP replies are trusted** if they match our transaction id and hardware address, as in any DHCP client; a
  malicious station on the same network can act as a rogue DHCP server.
* **Only WPA2-PSK/CCMP.** No open networks, WEP, TKIP, WPA3, or 802.1X. Hidden SSIDs are not supported.
* **The device `\DEV\WRND32$` is open to every local process** (OS/2 has no per-user device permissions). Any
  program can send the driver the same requests the Wireless LAN Monitor sends: join a network, change keys,
  read scan results. Install the driver only on a machine where local users are trusted.
* **Debug command channel.** `\DEV\WRND32$` also accepts text commands (register reads, eFuse reads, rate
  settings). In release builds (`DBG_ADMIN` set to 0) the commands that write chip registers or force a
  re-initialisation are disabled; the read-only ones remain.
* **No suspend/resume (APM) handling.** After a suspend the chip may need a re-plug.
* **Not tested against active attackers** (fuzzing, injection tools) or on a real network with hostile
  clients. The review was done by reading the code and by functional tests.

## Reporting problems

Please report security issues privately to the project maintainer before publishing details.
