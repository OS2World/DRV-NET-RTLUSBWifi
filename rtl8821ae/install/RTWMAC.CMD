/* RTWMAC.CMD - one-time helper: copy the adapter's real MAC into PROTOCOL.INI as NETADDRESS.
   Run it after the firmware file was copied and the adapter is READY (the status text shows HWMAC=...).
   Usage: RTWMAC [path-to-PROTOCOL.INI]   (default: \IBMCOM\PROTOCOL.INI on the boot drive) */
parse arg ini
if ini = '' then ini = value('BOOTDRIVE',,'OS2ENVIRONMENT')'\IBMCOM\PROTOCOL.INI'
if left(ini,1) = '\' then ini = 'C:'ini
mac = ''
'@copy \DEV\WRND32$ rtwmac.tmp > nul'
do while lines('rtwmac.tmp') > 0
   l = linein('rtwmac.tmp')
   p = pos('HWMAC=', l)
   if p > 0 then mac = space(translate(substr(l, p + 6, 17),,':'), 0)
end
call lineout 'rtwmac.tmp'
'@del rtwmac.tmp > nul'
if length(mac) <> 12 | mac = '000000000000' then do
   say 'Adapter not ready or MAC unknown. Copy the firmware file first (see STARTUP.CMD.example).'
   exit 1
end
n = 0
do while lines(ini) > 0
   n = n + 1; line.n = linein(ini)
end
call lineout ini
if n = 0 then do; say 'Cannot read' ini; exit 1; end
sect = 0; at = 0; done = 0
do i = 1 to n
   t = strip(line.i)
   if left(t,1) = '[' then sect = (translate(t) = '[RTL8821AE_NIF]')
   if sect & translate(word(translate(t,' ','='),1)) = 'NETADDRESS' then do
      line.i = '   NETADDRESS = "'mac'"'; done = 1
   end
   if sect then at = i
end
if \done then do
   if at = 0 then do; say '[RTL8821AE_NIF] section not found in' ini; exit 1; end
   do j = n to at + 1 by -1; k = j + 1; line.k = line.j; end
   k = at + 1; line.k = '   NETADDRESS = "'mac'"'
   n = n + 1
end
'@copy' ini ini'.bak > nul'
'@del' ini '> nul'
do i = 1 to n; call lineout ini, line.i; end
call lineout ini
say 'NETADDRESS =' mac 'written to' ini'. Reboot to use it.'
