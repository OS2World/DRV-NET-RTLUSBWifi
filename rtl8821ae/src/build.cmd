@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem Build RTL8821AE.OS2 on OS/2 / ArcaOS with Open Watcom 2.0 (binp on PATH, WATCOM set) + NASM.
rem usage: build    (run in rtl8821ae\src; objects go to obj\, the driver to RTL8821AE.OS2)
rem The compiler flags are written out on each wcc line: CMD.EXE does not expand %VAR% the same way
rem inside a FOR command, and wcc then sees the flags as extra file names (E1139).
if "%WATCOM%"=="" set WATCOM=C:\WATCOM
set PATH=%WATCOM%\binp;C:\usr\bin;%PATH%
set beginlibpath=%WATCOM%\binp\dll
set INCLUDE=
if not exist obj mkdir obj
for %%f in (hdr lib ndisx end) do nasm -f obj os2\%%f.asm -o obj\%%f.obj
for %%f in (strat pci mem ndis genmac dbg) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw -fo=obj\%%f.obj os2\%%f.c
for %%f in (sta) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw -fo=obj\%%f.obj wlan\%%f.c
for %%f in (rtw_io rtw_hal rtw_phy rtw_trx rtw_sta rtw_tbl) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=rtw -fo=obj\%%f.obj rtw\%%f.c
wlink @rtl8821ae.lnk
