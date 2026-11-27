# SPDX-License-Identifier: GPL-2.0-only
# Cross build on Linux with Open Watcom 2.0 (binl64 on PATH, WATCOM set) and NASM:  make
# On OS/2 use build.cmd (same flags). Objects go to obj/, the driver to RTL8821AE.OS2.
#
#   os2/   OS/2 plumbing: device header, DevHelp glue, strategy routine, NDIS MAC, GenMAC OIDs, DMA memory, PCI
#          (from the iwlwifi OS/2 driver, github.com/psmedley/iwlwifi)
#   wlan/  the chip-independent station (scan table, join, keys, EAPOL, Ethernet <-> 802.11) and hw.h
#   rtw/   the Realtek RTL8821AE back end (chip layer from rtl8821au/, PCIe DMA from Linux rtlwifi)
CFLAGS = -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw
ASM    = hdr lib ndisx end
OS2C   = strat pci mem ndis genmac dbg
WLANC  = sta
RTWC   = rtw_io rtw_hal rtw_phy rtw_trx rtw_sta rtw_tbl
HDRS   = os2/drv.h os2/os2dd.h os2/ndis.h wlan/hw.h wlan/wlan.h rtw/rtw.h

OBJS   = $(addprefix obj/,$(addsuffix .obj,$(ASM) $(OS2C) $(WLANC) $(RTWC)))

RTL8821AE.OS2: obj $(OBJS) rtl8821ae.lnk
	wlink @rtl8821ae.lnk

obj:
	mkdir -p obj

obj/%.obj: os2/%.asm
	nasm -f obj $< -o $@

obj/%.obj: os2/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

obj/%.obj: wlan/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

obj/%.obj: rtw/%.c $(HDRS)
	wcc $(CFLAGS) -fo=$@ $<

clean:
	rm -rf obj RTL8821AE.OS2 RTL8821AE.map

.PHONY: clean
