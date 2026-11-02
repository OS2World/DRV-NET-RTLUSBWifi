# SPDX-License-Identifier: GPL-2.0-only
# RTL88EU.OS2: Open Watcom WMAKE, 16-bit WCC, NASM, WLINK.
#
# Run from the src directory:
#   wmake -f rtl88eu.mak
#   wmake -a -f rtl88eu.mak
#   wmake -f rtl88eu.mak rt_usb.obj
#   wmake -f rtl88eu.mak link
#   wmake -f rtl88eu.mak clean
#
# rtl88eu.lnk remains authoritative for linker settings and order:
# rt_hdr.obj must be first, rt_end.obj must be last.
# Its output name must be RTL88EU.OS2 in this directory.

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

TARGET = RTL88EU.OS2
LNKFILE = rtl88eu.lnk
LOG = build.log

HEADERS = rt_ndis.h rt_os2dd.h rt_usb.h

COBJS = fwdl.obj &
        fwimg.obj &
        rt_ndis.obj &
        rt_strat.obj &
        rt_usb.obj &
        rtl_bb.obj &
        rtl_dbg.obj &
        rtl_dhcp.obj &
        rtl_init.obj &
        rtl_iqk.obj &
        rtl_mac.obj &
        rtl_nic.obj &
        rtl_rf.obj &
        rtl_rx.obj &
        rtw_reg.obj

AOBJS = rt_hdr.obj rt_lib.obj rt_ndisx.obj rt_end.obj

# Dependency order here does not replace the order in rtl88eu.lnk.
OBJS = rt_hdr.obj $(COBJS) rt_lib.obj rt_ndisx.obj rt_end.obj

# Default target.
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

$(TARGET): $(OBJS) $(LNKFILE) rtl88eu.mak
	@echo $(LD) @$(LNKFILE) >> $(LOG)
	$(LD) @$(LNKFILE) >> $(LOG) 2>&1

# Force a link, first bringing all objects up to date.
link: $(OBJS) $(LNKFILE) rtl88eu.mak .SYMBOLIC
	@echo $(LD) @$(LNKFILE) >> $(LOG)
	$(LD) @$(LNKFILE) >> $(LOG) 2>&1

# Conservative header dependencies because source contents were not supplied.
# Add any additional NASM include files to the relevant prerequisites.
$(COBJS) $(AOBJS): $(HEADERS) rtl88eu.mak

.EXTENSIONS:
.EXTENSIONS: .obj .c .asm

.c.obj:
	@echo $(CC) $(CFLAGS) -fo=$^@ $^*.c >> $(LOG)
	$(CC) $(CFLAGS) -fo=$^@ $^*.c >> $(LOG) 2>&1

.asm.obj:
	@echo $(AS) $(AFLAGS) $^*.asm -o $^@ >> $(LOG)
	$(AS) $(AFLAGS) $^*.asm -o $^@ >> $(LOG) 2>&1

# Remove only named build outputs, not unrelated object files.
clean: .SYMBOLIC
	@if exist fwdl.obj del fwdl.obj
	@if exist fwimg.obj del fwimg.obj
	@if exist rt_ndis.obj del rt_ndis.obj
	@if exist rt_strat.obj del rt_strat.obj
	@if exist rt_usb.obj del rt_usb.obj
	@if exist rtl_bb.obj del rtl_bb.obj
	@if exist rtl_dbg.obj del rtl_dbg.obj
	@if exist rtl_dhcp.obj del rtl_dhcp.obj
	@if exist rtl_init.obj del rtl_init.obj
	@if exist rtl_iqk.obj del rtl_iqk.obj
	@if exist rtl_mac.obj del rtl_mac.obj
	@if exist rtl_nic.obj del rtl_nic.obj
	@if exist rtl_rf.obj del rtl_rf.obj
	@if exist rtl_rx.obj del rtl_rx.obj
	@if exist rtw_reg.obj del rtw_reg.obj
	@if exist rt_hdr.obj del rt_hdr.obj
	@if exist rt_lib.obj del rt_lib.obj
	@if exist rt_ndisx.obj del rt_ndisx.obj
	@if exist rt_end.obj del rt_end.obj
	@if exist $(TARGET) del $(TARGET)
