/* rtl88eu_ip.cmd - gets an address for the WiFi adapter from the driver's built-in DHCP
 * client and keeps it renewed. Reading \DEV\WRND32$ makes the driver run
 * DISCOVER/OFFER/REQUEST/ACK over the air and return the lease as KEY=VALUE lines.
 *
 * Usage: rtl88eu_ip.cmd [lanN]
 *   The interface name is taken from the driver (it works it out from PROTOCOL.INI);
 *   pass lanN only to override it.
 *
 * What it does with a lease: sets the address on the interface, adds a default route
 * (only if the system has none), adds the DHCP DNS server to RESOLV2 (the entry it added
 * for the previous lease is removed first; the original RESOLV2 is saved once as
 * RESOLV2.bak_rtl88eu).
 *
 * SPDX-License-Identifier: GPL-2.0-only */
parse arg ifname .
call RxFuncAdd 'SysSleep',      'RexxUtil', 'SysSleep'
call RxFuncAdd 'SysBootDrive',  'RexxUtil', 'SysBootDrive'
call RxFuncAdd 'SysMkDir',      'RexxUtil', 'SysMkDir'
call RxFuncAdd 'SysFileDelete', 'RexxUtil', 'SysFileDelete'

boot   = SysBootDrive()
etcdir = value('ETC', , 'OS2ENVIRONMENT')
if etcdir = '' then do
   etcdir = boot'\MPTN\ETC'
   call value 'ETC', etcdir, 'OS2ENVIRONMENT'
end
bin   = boot'\MPTN\BIN'
work  = boot'\IBMCOM\RTL88EU'
call SysMkDir work
log   = work'\ip.log'
lease = work'\lease.txt'
tmp   = work'\tmp.txt'
cur   = ''
lan   = 1

call say2 'rtl88eu_ip started' ifname
do forever
   call SysFileDelete lease
   address cmd '@copy \DEV\WRND32$' lease '>NUL 2>&1'
   status = ''; why = ''; ip = ''; mask = ''; router = ''; dns = ''; l64 = 0
   if stream(lease, 'C', 'QUERY EXISTS') \= '' then do
      do while lines(lease) > 0
         line = linein(lease)
         parse var line key '=' val
         key = translate(strip(key)); val = strip(val)
         select
            when key = 'LAN'     then lan    = val
            when key = 'STATUS'  then status = val
            when key = 'WHY'     then why    = val
            when key = 'IP'      then ip     = val
            when key = 'MASK'    then mask   = val
            when key = 'ROUTER'  then router = val
            when key = 'DNS'     then dns    = val
            when key = 'LEASE64' then l64    = val
            otherwise nop
         end
      end
      call stream lease, 'C', 'CLOSE'
   end
   if ifname = '' then ifc = 'lan'lan
   else ifc = ifname

   if status = 'OK' & ip \= '' then do
      if mask = '' then mask = '255.255.255.0'
      /* something else (e.g. the Wireless LAN Monitor) may have wiped the address: re-apply it */
      address cmd '@'bin'\ifconfig.exe' ifc '>' tmp '2>&1'
      txt = ''
      do while lines(tmp) > 0
         txt = txt linein(tmp)
      end
      call stream tmp, 'C', 'CLOSE'
      if pos('inet' ip' ', txt' ') = 0 then cur = ''
      if ip \= cur then do
         address cmd '@'bin'\ifconfig.exe' ifc ip 'netmask' mask
         cur = ip
         call say2 'lease' ip 'netmask' mask 'router' router 'dns' dns 'on' ifc
         if router \= '' then do
            if count_default() = 0 then do
               address cmd '@'bin'\route.exe add default' router '1'
               call say2 'default route via' router
            end
         end
         if dns \= '' then call apply_dns dns
      end
      /* renew well before expiry, at least every minute so a changed network is noticed */
      wait = 60
      if l64 > 0 & l64 * 32 < wait then wait = max(30, l64 * 32)
      call SysSleep wait
   end
   else do
      if why \= 'NOLINK' then call say2 'no lease:' status why
      call SysSleep 8
   end
end

/* number of 'default' routes in the routing table (no external tools needed) */
count_default: procedure expose bin tmp
   address cmd '@'bin'\netstat.exe -r >' tmp '2>&1'
   n = 0
   do while lines(tmp) > 0
      if word(linein(tmp), 1) = 'default' then n = n + 1
   end
   call stream tmp, 'C', 'CLOSE'
   return n

/* put the DHCP DNS server into RESOLV2, replacing the one added for the previous lease */
apply_dns: procedure expose etcdir work log
   parse arg dns
   f    = etcdir'\RESOLV2'
   last = work'\dns.last'
   old  = ''
   if stream(last, 'C', 'QUERY EXISTS') \= '' then do
      old = strip(linein(last))
      call stream last, 'C', 'CLOSE'
   end
   n = 0
   have = 0
   if stream(f, 'C', 'QUERY EXISTS') \= '' then do
      if stream(f'.bak_rtl88eu', 'C', 'QUERY EXISTS') = '' then
         address cmd '@copy' f f'.bak_rtl88eu >NUL 2>&1'
      do while lines(f) > 0
         l = linein(f)
         if old \= '' & strip(l) = 'nameserver' old then iterate   /* our previous entry */
         if strip(l) = 'nameserver' dns then have = 1
         n = n + 1
         line.n = l
      end
      call stream f, 'C', 'CLOSE'
   end
   if \have then do
      n = n + 1
      line.n = 'nameserver' dns
   end
   call SysFileDelete f
   do i = 1 to n
      call lineout f, line.i
   end
   call lineout f
   call SysFileDelete last
   call lineout last, dns
   call lineout last
   call say2 'DNS server' dns 'added to' f
   return

say2:
   call lineout log, date('S') time() arg(1)
   call lineout log
   return
