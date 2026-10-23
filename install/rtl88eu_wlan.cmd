@echo off
rem ---------------------------------------------------------------------------
rem rtl88eu_wlan.cmd - keeps the native RTL8188EU WiFi link (lan1) up
rem
rem Runs wpa_supplicant on lan1 and restarts it whenever it ends (dongle not yet
rem plugged in, link lost, ...). Started in the background from STARTUP.CMD.
rem Output goes to C:\IBMCOM\RTL88EU\wlan.log (also used as the script log).
rem
rem Needs sleep.exe from the unixroot tools (C:\usr\bin).
rem Edit these values:
rem   CONF = your wpa_supplicant config (SSID + passphrase)
rem   IPADDR / NETMASK = static address for lan1 (empty IPADDR = do not touch)
rem ---------------------------------------------------------------------------
set LOG=C:\IBMCOM\RTL88EU\wlan.log
set SUPP=C:\sys\apps\xwps\plugins\xcenter\xwlan\wpa_supplicant.exe
rem IFACE = the adapter name of the WiFi dongle: lanN, where N is the position of RTL88EU_NIF in the
rem         Bindings list of [TCPIP_NIF] in PROTOCOL.INI, counting from 0 (the driver prints it with /V).
set IFACE=lan1
set IPADDR=
set NETMASK=255.255.255.0
if not exist C:\IBMCOM\RTL88EU\nul md C:\IBMCOM\RTL88EU
echo %DATE% %TIME% rtl88eu_wlan started >> %LOG%
rem DHCP for the WiFi interface comes from the driver (rtl88eu_ip.cmd); it waits until the link is up
detach C:\OS2\CMD.EXE /c C:\IBMCOM\RTL88EU\rtl88eu_ip.cmd %IFACE%
:loop
set CONF=C:\MPTN\ETC\rtl88eu.conf
if not exist %CONF% goto wait
echo %DATE% %TIME% starting wpa_supplicant with %CONF% >> %LOG%
if not "%IPADDR%"=="" C:\MPTN\BIN\ifconfig.exe %IFACE% %IPADDR% netmask %NETMASK% >> %LOG% 2>&1
%SUPP% -i %IFACE% -c %CONF% >> %LOG% 2>&1
echo %DATE% %TIME% wpa_supplicant ended, retry in 10 s >> %LOG%
:wait
C:\usr\bin\sleep.exe 10
goto loop
