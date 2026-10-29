# v013-smp: the 8188EU 0.1.2 driver with the SMP fixes learned on the RTL8821AU driver

Source: copy of `..\v012\src` (the released 0.1.2 tree is untouched). Build: `src\build_vm.cmd` (all files, in the VM via the
scratchpad script rb88.sh), output RTL88EU.OS2 (52,308 bytes, md5 2f2da411a15d916bbaa9977ebba0ec36, first build 2026-10-28).

## What was ported (all of it platform code, nothing chip specific)
- rt_lib.asm / rt_os2dd.h: rt_cpu_id (CPUID APIC id), rt_lock_try / rt_lock_acquire / rt_lock_release (cmpxchg owner locks),
  rt_wait_zero (bounded PAUSE spin), rt_atomic_dec_nz.
- rt_usb.c: synchronous transfer lock (usb_xfer_lock/unlock) around g_xfer / g_setup / g_databuf; TX queue lock (usb_txq_lock,
  bounded 8 x 65536 PAUSE passes, interrupts stay enabled, 0xFFFF = refused); bulk-OUT data toggle applied and advanced under the TX lock
  (also for synchronous bulk OUT); usb_tx_submit = locked wrapper (returns -2 when refused) around usb_tx_submit_locked.
- rtw_reg.c: every rtw_read*/rtw_write*/rtw_writeN holds the transfer lock.
- rtl_nic.c: nic_lock with an atomic exchange (rt_tas) and a 50 ms ProcBlock.
- rt_ndis.c: indication exclusion owner-tagged by CPU (g_ind_owner, rt_lock_try, IndicationOff waits for another CPU, never for itself),
  rt_atomic_dec_nz for IndicationOn / the indicate flag; TransmitChain gathers the frame into its own TX slot at TX_ETH_OFF (74) instead of
  the shared g_txg buffer; the GDT selector for physical blocks is taken under an owner lock.
- rtl_rx.c: receive accumulator reset by a flag (only the completion touches g_acc/g_accn); EAPOL queue count atomic; TX packet number
  assigned under the TX queue lock in the same step as the submission (rtl8188eu_ndis_tx_slot: headers written in place); protected frames
  of the driver itself (DHCP, protected EAPOL) go through the same path; PN reset under the lock; duplicate pairwise-key install guard.
- Diagnostics added: TXQRE CPUTX TXQTMO INDTMO INDRE CPURX (DIAG_COUNT 85).

## Not ported (8821AU only or not applicable)
Receiver gain control (IGI), the 26-byte deauth length (this chip delivers frames with the FCS, so the existing >= 28 check is right),
LED, RX single-request tuning.

## Baseline result (before the port): the unmodified 8188EU 0.1.2 release traps on SMP
ArcaMiix (xHCI, ArcaOS 14.204_SMP), soak 20 MB downloads + ping to the dev PC over Wi-Fi, started 14:46 2026-10-28; 43 iterations clean
(about 220 KB/s, 0% loss, no watchdog event), then at about 15:55, while the user was also running yum / rpm updates over the same link:
TRAP 000E in AFINETK, CPU=01, CS:EIP 0168:f0b6a9b6, CR2=00000014, ERRCD=0000, EAX=ed4e9014 ECX=ESI=ed4d4514 EDI=f9494e84 EBP=00005030
SS:ESP=1530:00005010, FLG=00210206, internal revision 14.204_SMP. Same family as the PC1 traps in AFINETK/SOCKETS (always CPU 01) seen with
0.1.1 and 0.1.2, and the SOCKETS trap seen with the 8821AU driver before the hook delivery fix.
