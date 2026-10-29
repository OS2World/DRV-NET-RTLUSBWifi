@echo off
rem Build for the VM staging dir (sources fetched by rb88.sh). Log: build.log
set WATCOM=C:\WATCOM
set PATH=C:\WATCOM\binp;C:\usr\bin;%PATH%
set INCLUDE=
echo build start > build.log
for %%f in (rt_hdr rt_lib rt_ndisx rt_end) do nasm -f obj %%f.asm -o %%f.obj >> build.log 2>&1
for %%f in (rt_strat rt_usb rt_ndis fwdl fwimg rtl_init rtl_mac rtl_bb rtl_rf rtl_iqk rtl_nic rtl_dhcp rtl_dbg rtl_rx rtw_reg) do wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j %%f.c >> build.log 2>&1
wlink @rtl88eu.lnk >> build.log 2>&1
echo build done >> build.log
type build.log
