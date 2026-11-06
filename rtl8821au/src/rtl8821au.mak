# SPDX-License-Identifier: GPL-2.0-only
# RTL8821AU 0.1.3: Open Watcom WMAKE on OS/2 / ArcaOS.
#
# Build:          wmake -f rtl8821au.mak
# Full rebuild:   wmake -a -f rtl8821au.mak
# Compile only:   wmake -f rtl8821au.mak rt_usb.obj
# Force relink:   wmake -f rtl8821au.mak link
# Clean:          wmake -f rtl8821au.mak clean
#
# Run from the directory containing the sources and rtl88eu.lnk.
# rtl88eu.lnk controls segment attributes and actual link order:
# rt_hdr.obj must be FIRST and rt_end.obj must be LAST.
# Its output must be RTL8821AU.OS2 in this directory.

!ifdef %WATCOM
WATCOMDIR = $(%WATCOM)
!else
WATCOMDIR = C:\WATCOM
!endif

CC = wcc
AS = nasm
LD = wlink

CFLAGS = -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j
AFLAGS = -f obj

TARGET = RTL8821AU.OS2
LNKFILE = rtl88eu.lnk
LOG = build.log

HEADERS = rt_ndis.h rt_os2dd.h rt_usb.h rtl8821_tbl.h

COBJS = rt_strat.obj &
        rt_usb.obj &
        rt_ndis.obj &
        rtl_nic.obj &
        rtl_dhcp.obj &
        rtl_dbg.obj &
        rtl_rx.obj &
        rtw_reg.obj &
        rtl8821_hal.obj &
        rtl8821_phy.obj &
        rtl8821_tbl.obj &
        rtl8821_efuse.obj &
        rtl8821_tx.obj

AOBJS = rt_hdr.obj rt_lib.obj rt_ndisx.obj rt_end.obj

# Dependency order does not replace the order in rtl88eu.lnk.
OBJS = rt_hdr.obj $(COBJS) rt_lib.obj rt_ndisx.obj rt_end.obj

all: $(TARGET) .SYMBOLIC
	@%null

# Remove incomplete targets after failed commands.
.ERASE

.BEFORE
	@set WATCOM=$(WATCOMDIR)
	@set PATH=$(WATCOMDIR)\binp;C:\usr\bin;$(%PATH)
	@set INCLUDE=
	@echo make start > $(LOG)

.AFTER
	@echo make completed >> $(LOG)
	@type $(LOG)

.ERROR
	@echo make FAILED >> $(LOG)
	@type $(LOG)

$(TARGET): $(OBJS) $(LNKFILE) rtl8821au.mak
	@echo $(LD) @$(LNKFILE) >> $(LOG)
	$(LD) @$(LNKFILE) >> $(LOG) 2>&1

# Update objects, then link even if the driver is already current.
link: $(OBJS) $(LNKFILE) rtl8821au.mak .SYMBOLIC
	@echo $(LD) @$(LNKFILE) >> $(LOG)
	$(LD) @$(LNKFILE) >> $(LOG) 2>&1

# Conservative dependencies because source contents were not supplied.
# Add additional assembler include files to relevant prerequisites.
$(COBJS) $(AOBJS): $(HEADERS) rtl8821au.mak

.EXTENSIONS:
.EXTENSIONS: .obj .c .asm

.c.obj:
	@echo $(CC) $(CFLAGS) -fo=$^@ $^*.c >> $(LOG)
	$(CC) $(CFLAGS) -fo=$^@ $^*.c >> $(LOG) 2>&1

.asm.obj:
	@echo $(AS) $(AFLAGS) $^*.asm -o $^@ >> $(LOG)
	$(AS) $(AFLAGS) $^*.asm -o $^@ >> $(LOG) 2>&1

# Delete only named build outputs; retain build.log.
clean: .SYMBOLIC
	@if exist rt_strat.obj del rt_strat.obj
	@if exist rt_usb.obj del rt_usb.obj
	@if exist rt_ndis.obj del rt_ndis.obj
	@if exist rtl_nic.obj del rtl_nic.obj
	@if exist rtl_dhcp.obj del rtl_dhcp.obj
	@if exist rtl_dbg.obj del rtl_dbg.obj
	@if exist rtl_rx.obj del rtl_rx.obj
	@if exist rtw_reg.obj del rtw_reg.obj
	@if exist rtl8821_hal.obj del rtl8821_hal.obj
	@if exist rtl8821_phy.obj del rtl8821_phy.obj
	@if exist rtl8821_tbl.obj del rtl8821_tbl.obj
	@if exist rtl8821_efuse.obj del rtl8821_efuse.obj
	@if exist rtl8821_tx.obj del rtl8821_tx.obj
	@if exist rt_hdr.obj del rt_hdr.obj
	@if exist rt_lib.obj del rt_lib.obj
	@if exist rt_ndisx.obj del rt_ndisx.obj
	@if exist rt_end.obj del rt_end.obj
	@if exist $(TARGET) del $(TARGET)
