@echo off
rem SPDX-License-Identifier: GPL-2.0-only
rem RTL88EU build with Open Watcom 16-bit C + NASM + wlink only. Log: build.log
rem usage: build              (everything)     build file1 file2 ...   (only these C files, then link)
set WATCOM=C:\WATCOM
set PATH=C:\WATCOM\binp;C:\usr\bin;%PATH%
set INCLUDE=
z:
cd \WiFi-RTL8188EU-ArcaOS\v012\src
echo build start > build.log
for %%f in (rt_hdr rt_lib rt_ndisx rt_end) do nasm -f obj %%f.asm -o %%f.obj >> build.log 2>&1
for %%f in (%1 %2 %3 %4 %5 %6 %7 %8 %9) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j %%f.c >> build.log 2>&1
wlink @rtl88eu.lnk >> build.log 2>&1
echo build done >> build.log
type build.log
