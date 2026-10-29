@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem RTL8821AU 0.1.3 build with Open Watcom 16-bit C + NASM + wlink only. Log: build.log
rem Run this in the directory that holds the sources (see ..\BUILD.md).
rem usage: build                 (compile everything and link)
rem        build file1 file2 ... (recompile only these C files, e.g. "build rt_usb", then link)
rem Adjust the next two lines if Open Watcom / NASM are installed elsewhere.
set WATCOM=C:\WATCOM
set PATH=C:\WATCOM\binp;C:\usr\bin;%PATH%
set INCLUDE=
echo build start > build.log
for %%f in (rt_hdr rt_lib rt_ndisx rt_end) do nasm -f obj %%f.asm -o %%f.obj >> build.log 2>&1
if "%1"=="" goto all
for %%f in (%1 %2 %3 %4 %5 %6 %7 %8 %9) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j %%f.c >> build.log 2>&1
goto link
:all
for %%f in (rt_strat rt_usb rt_ndis rtl_nic rtl_dhcp rtl_dbg rtl_rx rtw_reg rtl8821_hal rtl8821_phy rtl8821_tbl rtl8821_efuse rtl8821_tx) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j %%f.c >> build.log 2>&1
:link
wlink @rtl88eu.lnk >> build.log 2>&1
echo build done >> build.log
type build.log
