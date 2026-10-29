/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTL88EU - Realtek RTL8188EU USB WiFi driver for OS/2 and ArcaOS
 * Copyright (C) 2026 the RTL88EU-OS2 project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
/*
 * rt_usb.c - USB transport of the RTL8188EU driver, over the USBD IDC interface.
 *
 * Written from the "Basic USB Device Driver Stack for OS/2 Warp - Programmer's Reference" section 6
 * (request block, cancel, set configuration, register class driver, process IRQ, accept/reject,
 * detach) and the USB 2.0 specification. Facts used are listed in v012/PROVENANCE.md.
 *
 * Driver model: SS is not DS. Anything handed to USBD (which dereferences it as a far pointer) must live in
 * the driver's data segment, not on the stack: request packets, request blocks, setup packets and the
 * data buffers are statics.
 *
 * Two kinds of transfer:
 *   - synchronous (control, bulk firmware download): one request at a time from task time; the caller
 *     blocks (ProcBlock) until the completion callback signals it (ProcRun);
 *   - asynchronous data path: one bulk-IN request always outstanding (resubmitted from its own completion),
 *     and a small pool of transmit slots.
 */
#include "rt_usb.h"

usb_dev_t     g_usbdev;
ULONG         gpUSBDIDC;                  /* USBD's IDC entry point (far) */
USHORT        gdsUSBDIDC;                 /* and its data segment         */

/* Event ids for ProcBlock/ProcRun: any 32-bit value, chosen to be unlikely to collide. */
#define EVT_XFER   0x52544C01UL
#define EVT_DELAY  0x52544C02UL

static USHORT g_awake;                    /* ProcRun awakened-count (out parameter) */

/* Task-time millisecond delay: ProcBlock on a private event with a timeout. Nothing ever runs that event, so it
 * always times out after ms, yielding the CPU (a busy loop would not). Used for the settling delays of the
 * init tables. */
void usb_mdelay(USHORT ms)
{
   if (ms == 0)
      return;
   rt_dh_procblock(EVT_DELAY, (ULONG)ms, WAIT_UNINTERRUPTIBLE);
}

/* ---- request storage in the data segment (see the model note above) --------------- */
static usb_xfer_t   g_xfer;               /* the in-flight synchronous transfer        */
static RT_SETUP     g_setup;              /* control setup packet                      */
/* Bounce buffer for control AND bulk data. Must hold a whole firmware page / received frame:
 * usb_bulk_msg refuses len > sizeof(g_databuf), so a too-small buffer silently posts NO transfer. */
static UCHAR        g_databuf[1600];
USHORT              g_xfer_timeout_ms = 5000;   /* per-transfer wait; tests shorten it */
static RT_USBCANCEL g_cancel;                   /* cancel request block                */
static ULONG        g_seq;                      /* request sequence number (completion matching) */
USHORT              g_stale;                    /* completions ignored because they were not ours */

#define TAG_SYNC  0x100UL                 /* requestData1 of a synchronous request: above the async tags */

/* ---- synchronous transfer lock ----------------------------------------------------------
 * g_xfer, g_setup and g_databuf are single instances, and a synchronous transfer lets go of the CPU (ProcBlock)
 * while its transfer runs - so even one CPU interleaves them, and two CPUs do it at any moment. One synchronous
 * transfer at a time, task time only. */
#define EVT_XLOCK  0x52544C04UL
static UCHAR   g_xlock;
static USHORT  g_xwait;                          /* tasks waiting for the lock (so the unlock only calls ProcRun when needed) */

void usb_xfer_lock(void)
{
   while (rt_tas(&g_xlock)) {
      rt_atomic_inc(&g_xwait);
      rt_dh_procblock(EVT_XLOCK, 20UL, WAIT_UNINTERRUPTIBLE);   /* short: a ProcRun just before the block is lost */
      rt_atomic_dec(&g_xwait);
   }
}

void usb_xfer_unlock(void)
{
   g_xlock = 0;
   if (g_xwait)
      rt_dh_procrun(EVT_XLOCK, &g_awake);
}

/* TX queue lock. Two orders must match what reaches the dongle: the data toggle of consecutive bulk-OUT requests on
 * one endpoint (EHCI takes it from each request), and the CCMP packet numbers (the access point drops a frame whose
 * number is not above the last one it accepted). Both are decided and the request submitted under this lock.
 * Interrupts are NOT masked while it is held (an earlier version did, across the call into USBD, and a machine with an
 * xHCI controller froze completely under load: a CPU that waits inside USBD for an interrupt it has itself masked, the
 * other CPUs spinning on this lock). TransmitChain reaches the lock from any CPU; a CPU that already holds it (an
 * interrupt on top of a sender) gets 0xFFFF at once, and a wait that runs longer than a few milliseconds gives up with
 * 0xFFFF too: the caller reports "out of resource" and the protocol retries, so nothing can wait forever. */
static USHORT g_txq_owner;
USHORT        g_txq_reenter;                     /* diagnostics: attempts by a CPU that already held the lock */
USHORT        g_txq_tmo;                         /* diagnostics: waits that gave up */
USHORT        g_cpu_tx;                          /* diagnostics: bit n set = a transmit ran on the CPU with APIC id n (mod 16) */

USHORT usb_txq_lock(void)
{
   USHORT id = (USHORT)(rt_cpu_id() + 1), i;

   g_cpu_tx |= (USHORT)(1U << ((id - 1) & 15));
   for (i = 0; i < 8; i++) {
      if (!rt_lock_try(&g_txq_owner, id))
         return 0;                               /* taken */
      if (g_txq_owner == id) {
         g_txq_reenter++;
         return 0xFFFF;
      }
      rt_wait_zero(&g_txq_owner, 1);             /* up to 65536 PAUSE passes, then try again */
   }
   g_txq_tmo++;
   return 0xFFFF;
}

void usb_txq_unlock(USHORT fl)
{
   rt_lock_release(&g_txq_owner);
}

/* ---- one IDC call to USBD ----------------------------------------------------------
 * Every call needs its own request packet. One shared packet is not enough: the task and the completion callback
 * (which re-submits the receive request) call in at the same time, and on an SMP kernel truly in parallel; one
 * zeroing the packet between the other's fill and its call hands USBD a NULL parameter (trap in USBD). So the
 * packets come from a small pool, claimed atomically. */
#define NRP 8
static RP_GENIOCTL g_rp[NRP];
static UCHAR       g_rpu[NRP];
USHORT             g_rp_none;                      /* diagnostics: calls that found no free packet */

/* Every request submission into USBD is serialized: on an SMP kernel the task-time transmit path and the receive
 * resubmission made from the completion callback would otherwise run inside USBD at the same moment, and a request can
 * be lost there (it never completes). The lock is held with local interrupts disabled, so a completion cannot interrupt the
 * holder on the same CPU; the wait is bounded so that nothing can hang on it. */
static UCHAR  g_acc_lock;
static USHORT g_acc_lkf, g_cancel_rc;                 /* diagnostics: lock timeouts, status of the last cancel */
#define ACC_SPIN_MAX 30000U

static USHORT usbd_call(UCHAR function, PVOID pParm)
{
   USHORT        i, status, fl = 0, spin = 0;
   int           locked = 0;
   RP_GENIOCTL  *rp;

   for (i = 0; i < NRP; i++)
      if (!rt_tas(&g_rpu[i]))
         break;
   if (i == NRP) {
      g_rp_none++;
      return (USHORT)(STDON | STERR | USB_IDC_ALLOCERR);
   }
   rp = &g_rp[i];
   rt_setmem((PVOID)rp, 0, sizeof(RP_GENIOCTL));
   rp->rph.Cmd    = CMD_GENIOCTL;
   rp->Category   = USB_IDC_CATEGORY_USBD;
   rp->Function   = function;
   rp->ParmPacket = pParm;

#if RT_ACC_LOCK
   if (function == USB_IDC_FUNCTION_ACCIO) {
      fl = rt_irq_save();
      while (rt_tas(&g_acc_lock))
         if (++spin >= ACC_SPIN_MAX)
            break;
      locked = (spin < ACC_SPIN_MAX);
      if (!locked)
         g_acc_lkf++;
   }
#endif
   rt_idc_call(gpUSBDIDC, gdsUSBDIDC, (PVOID)rp);
   status = rp->rph.Status;
#if RT_ACC_LOCK
   if (function == USB_IDC_FUNCTION_ACCIO) {
      if (locked)
         g_acc_lock = 0;
      rt_irq_restore(fl);
   }
#endif
   if (function == USB_IDC_FUNCTION_CANCEL)
      g_cancel_rc = status;
   g_rpu[i] = 0;
   return status;
}

/* Far address of our IDC routine, for registration and for the completion address of each request. */
static ULONG idc_addr(void)
{
   union { void (__far *f)(void); ULONG l; } u;

   u.f = (void (__far *)(void))rt_idc;
   return u.l;
}

/* ---- client registration ----------------------------------------------------------- */
int usb_register_client(void)
{
   static RT_USBCLASS rd;
   USHORT st;

   rd.usbIDC = idc_addr();
   rd.usbDS  = rt_getds();

   st = usbd_call(USB_IDC_FUNCTION_REGISTER, (PVOID)&rd);
   return (st & STERR) ? -1 : 0;
}

void usb_unregister_client(void)
{
   static RT_USBCLASS rd;

   rd.usbIDC = idc_addr();
   rd.usbDS  = rt_getds();
   usbd_call(USB_IDC_FUNCTION_UNREGISTER, (PVOID)&rd);
}

/* ---- completion plumbing (submit g_xfer, wait) -------------------------------------- */
/* Caller holds usb_xfer_lock. tgl != 0 (bulk OUT): *tgl is the endpoint's toggle; it is applied to the request and
 * advanced by flip (the parity of the packet count) in the same locked step as the submission, like the async TX
 * path, so the two paths can share an endpoint. */
static int usb_submit_wait(UCHAR *tgl, UCHAR flip)
{
   USHORT st, fl = 0xFFFF;

   g_xfer.done   = 0;
   g_xfer.status = 0;
   g_xfer.actual = 0;
   g_xfer.rb.requestData1 = TAG_SYNC;
   g_seq++;                                    /* tag this request; USBD hands the cookie back */
   g_xfer.rb.requestData2 = g_seq;
   g_xfer.rb.usbIDC       = idc_addr();
   g_xfer.rb.usbDS        = rt_getds();
   g_xfer.rb.category     = USB_IDC_CATEGORY_CLIENT;

   if (tgl) {
      fl = usb_txq_lock();                     /* task time: never held by this CPU already */
      if (*tgl)
         g_xfer.rb.flags |= USRB_FLAGS_DET_DTGGLEON;
   }
   st = usbd_call(USB_IDC_FUNCTION_ACCIO, (PVOID)&g_xfer.rb);
   if (tgl) {
      if (!(st & STERR))
         *tgl ^= flip;
      if (fl != 0xFFFF)
         usb_txq_unlock(fl);
   }
   if (st & STERR)
      return -1;

   /* Block until the completion handler runs us. Finite timeout so a lost wakeup or a completion that never
    * arrives cannot hang the caller. Re-check 'done' after: if the wakeup raced ahead of the block, ProcBlock
    * just waits out the timeout and we still see done=1. Only called at task time. */
   if (!g_xfer.done)
      rt_dh_procblock(EVT_XFER, (ULONG)g_xfer_timeout_ms, WAIT_INTERRUPTIBLE);

   if (!g_xfer.done) {
      /* Timed out. A bulk request stays queued inside USBD pointing at g_xfer.rb, which the NEXT request would
       * overwrite - so cancel it and wait for its (canceled) completion before returning. (Control: endpoint 0,
       * short-lived, left as it is.) */
      if (g_xfer.rb.endPointId != 0) {
         g_cancel.controllerId  = g_xfer.rb.controllerId;
         g_cancel.deviceAddress = g_xfer.rb.deviceAddress;
         g_cancel.endPointId    = g_xfer.rb.endPointId;
         usbd_call(USB_IDC_FUNCTION_CANCEL, (PVOID)&g_cancel);
         if (!g_xfer.done)
            rt_dh_procblock(EVT_XFER, 1000UL, WAIT_INTERRUPTIBLE);
      }
      return -1;                       /* timed out / interrupted - no data */
   }

   return (g_xfer.status == 0) ? 0 : -1;
}

/* ---- control transfer (caller holds usb_xfer_lock: see rtw_reg.c) ---------------------- */
int usb_ctrl_msg(usb_dev_t *dev, UCHAR bmRequestType, UCHAR bRequest,
                 USHORT wValue, USHORT wIndex, void *pData, USHORT len)
{
   int rc;

   if (len > sizeof(g_databuf))
      return -1;

   g_setup.bmRequestType = bmRequestType;
   g_setup.bRequest      = bRequest;
   g_setup.wValue        = wValue;
   g_setup.wIndex        = wIndex;
   g_setup.wLength       = len;

   /* OUT (host to device): copy the caller's data into the bounce buffer. */
   if (!(bmRequestType & 0x80) && pData && len)
      rt_movmem((PVOID)g_databuf, (PVOID)pData, len);

   rt_setmem((PVOID)&g_xfer.rb, 0, sizeof(g_xfer.rb));
   g_xfer.rb.controllerId  = dev->ctrlId;
   g_xfer.rb.deviceAddress = dev->devAddr;
   g_xfer.rb.endPointId    = USB_DEFAULT_CTRL_ENDPT;
   g_xfer.rb.flags         = USRB_FLAGS_TTYPE_SETUP;
   g_xfer.rb.buffer1       = (PUCHAR)&g_setup;
   g_xfer.rb.buffer1Length = sizeof(RT_SETUP);
   g_xfer.rb.buffer2       = (PUCHAR)g_databuf;
   g_xfer.rb.buffer2Length = len;
   g_xfer.rb.serviceTime   = USB_DEFAULT_SRV_INTV;
   g_xfer.rb.maxPacketSize = USB_DEFAULT_PKT_SIZE;
   g_xfer.rb.maxErrorCount = USB_MAX_ERROR_COUNT;

   rc = usb_submit_wait((UCHAR *)0, 0);

   /* IN (device to host): copy the result back only on success, so a failed or timed-out read leaves the
    * caller's buffer untouched (the caller pre-zeros it). */
   if (rc == 0 && (bmRequestType & 0x80) && pData && len)
      rt_movmem((PVOID)pData, (PVOID)g_databuf, len);

   return rc;
}

/* Bulk data toggle (DATA0/DATA1). USBD does not track it for us: every bulk request carries the toggle of its
 * first packet in USRB_FLAGS_DET_DTGGLEON and the host controller flips it per packet. The xHCI driver keeps its
 * own state and ignores the flag; the EHCI driver takes it from the request, so a request that always starts on
 * DATA0 makes the device drop every second frame as a retransmission. g_tgl_* = toggle for the next request's
 * first packet (0 = DATA0). */
static UCHAR  g_tgl_out[16], g_tgl_in[16];
static USHORT g_tgl_bad;                       /* diagnostics: IN completions where the returned flag disagreed with our count */
static USHORT g_tgl_ret, g_tgl_pred;           /* last returned / predicted toggle of an IN completion */

static USHORT tgl_packets(USHORT len, USHORT mps)
{
   if (mps == 0 || len == 0)
      return 1;                                /* a zero-length transfer is one (empty) packet */
   return (USHORT)((len + mps - 1) / mps);
}

/* Max packet size of a bulk endpoint, from the cached configuration descriptor: 512 when the dongle runs at high
 * speed (EHCI port), 64 when it sits on a full-speed (UHCI/OHCI) port. 512 if it cannot be found. */
static USHORT ep_mps(usb_dev_t *dev, UCHAR num, UCHAR in)
{
   UCHAR FAR *p;
   USHORT i, tot;

   if (dev->pDevInfo == 0)
      return 512;
   p   = dev->pDevInfo->configurationData;
   tot = (USHORT)((USHORT)p[2] | ((USHORT)p[3] << 8));
   if (tot == 0 || tot > 512)
      tot = 512;
   i = 0;
   while ((USHORT)(i + 7) <= tot) {
      UCHAR len = p[i];
      if (len < 2)
         break;
      if (p[i + 1] == 0x05 && len >= 7 && (UCHAR)(p[i + 3] & 0x03) == 0x02 &&
          (UCHAR)(p[i + 2] & 0x0F) == num && (UCHAR)((p[i + 2] & 0x80) ? 1 : 0) == in)
         return (USHORT)((USHORT)p[i + 4] | ((USHORT)p[i + 5] << 8));
      i = (USHORT)(i + len);
   }
   return 512;
}

/* ---- bulk transfer ------------------------------------------------------------------- */
int usb_bulk_msg(usb_dev_t *dev, UCHAR endpoint, int dir,
                 void *pData, USHORT len, USHORT *pActual)
{
   int rc;

   if (len > sizeof(g_databuf))
      return -1;

   usb_xfer_lock();
   if (dir == USB_DIR_OUT && pData && len)
      rt_movmem((PVOID)g_databuf, (PVOID)pData, len);

   rt_setmem((PVOID)&g_xfer.rb, 0, sizeof(g_xfer.rb));
   g_xfer.rb.controllerId  = dev->ctrlId;
   g_xfer.rb.deviceAddress = dev->devAddr;
   g_xfer.rb.endPointId    = endpoint;
   g_xfer.rb.flags         = (USHORT)(USRB_FLAGS_DET_BULK |
                                (dir == USB_DIR_IN ? USRB_FLAGS_TTYPE_IN
                                                   : USRB_FLAGS_TTYPE_OUT));
   if (dir == USB_DIR_IN && g_tgl_in[endpoint & 0x0F])   /* OUT: set in usb_submit_wait, under the TX queue lock */
      g_xfer.rb.flags |= USRB_FLAGS_DET_DTGGLEON;
   g_xfer.rb.buffer1       = (PUCHAR)g_databuf;
   g_xfer.rb.buffer1Length = len;
   g_xfer.rb.serviceTime   = USB_DEFAULT_SRV_INTV;
   /* Bulk endpoints of a USB 2.0 high-speed device use a 512-byte max packet; the 64-byte control default would
    * mis-size the transfer descriptors and the IN could then never complete. */
   g_xfer.rb.maxPacketSize = ep_mps(dev, (UCHAR)(endpoint & 0x0F), (UCHAR)(dir == USB_DIR_IN ? 1 : 0));
   g_xfer.rb.maxErrorCount = USB_MAX_ERROR_COUNT;

   if (dir == USB_DIR_OUT)                      /* every packet of the transfer flips the toggle */
      rc = usb_submit_wait(&g_tgl_out[endpoint & 0x0F], (UCHAR)(tgl_packets(len, g_xfer.rb.maxPacketSize) & 1));
   else
      rc = usb_submit_wait((UCHAR *)0, 0);

   if (dir == USB_DIR_IN && pData && len)
      rt_movmem((PVOID)pData, (PVOID)g_databuf, len);
   if (pActual)
      *pActual = g_xfer.actual;
   usb_xfer_unlock();
   return rc;
}

/* ---- set configuration ---------------------------------------------------------------- */
int usb_set_configuration(usb_dev_t *dev, UCHAR configValue)
{
   static RT_USBSETCONF rd;
   USHORT st;

   rt_setmem((PVOID)&rd, 0, sizeof(rd));
   rd.controllerId       = dev->ctrlId;
   rd.deviceAddress      = dev->devAddr;
   rd.classDriverDS      = rt_getds();
   rd.configurationValue = configValue;
   rd.category           = USB_IDC_CATEGORY_CLIENT;

   st = usbd_call(USB_IDC_FUNCTION_SETCONF, (PVOID)&rd);
   if (st & STERR)
      return -1;
   dev->configValue = configValue;
   return 0;
}

static void async_complete(RT_USBRB FAR *rb);   /* async RX/TX completions (end of file) */

/* ---- the IDC routine USBD calls ----------------------------------------------------- */
static void idc_body(RP_GENIOCTL FAR *pRP);

/* Diagnostics of the completion context: how deeply our IDC routine is nested (USBD might complete a request inside
 * the call that submits the next one) and how low the stack pointer gets. */
static USHORT g_cb_depth, g_cb_max, g_cb_rec, g_cb_tot, g_sp_min = 0xFFFF, g_sp_max, g_last_sp, g_last_ss;

void __far __cdecl rt_idc(RP_GENIOCTL FAR *pRP)
{
   USHORT sp = rt_getsp(), ss = rt_getss();

   rt_atomic_inc(&g_cb_depth);
   g_cb_tot++;
   if (g_cb_depth > g_cb_max)
      g_cb_max = g_cb_depth;
   if (ss == g_last_ss && sp < g_last_sp && (USHORT)(g_last_sp - sp) < 8192U && g_cb_depth > 1)
      g_cb_rec++;                              /* entered again below an invocation still in progress: recursion */
   g_last_sp = sp;
   g_last_ss = ss;
   if (sp < g_sp_min)
      g_sp_min = sp;
   if (sp > g_sp_max)
      g_sp_max = sp;
   idc_body(pRP);
   rt_atomic_dec(&g_cb_depth);
}

static void idc_body(RP_GENIOCTL FAR *pRP)
{
   switch (pRP->Function) {

   case USB_IDC_FUNCTION_PRCIRQ: {            /* a transfer completed */
      /* rb is USBD's copy of our request block, in USBD's segment: far. */
      RT_USBRB FAR *rb = (RT_USBRB FAR *)pRP->ParmPacket;
      if (rb->requestData1 < 16UL) {          /* async RX / TX slot completion */
         async_complete(rb);
         break;
      }
      if (rb->requestData2 != g_seq) {        /* completion of an EARLIER (cancelled / abandoned) */
         g_stale++;                           /* request: must not satisfy the one we wait on now */
         break;
      }
      g_xfer.status = rb->status;
      g_xfer.actual = rb->buffer1Length;
      g_xfer.done   = 1;
      rt_dh_procrun(EVT_XFER, &g_awake);
      break;
   }

   case USB_IDC_FUNCTION_CHKSERV: {           /* "do you serve this device?" */
      /* The structures live in USBD's segment: far. */
      RT_USBSERVE FAR *cd = (RT_USBSERVE FAR *)pRP->ParmPacket;
      RT_DEVINFO  FAR *di = cd->pDeviceInfo;
      if (di &&
          di->descriptor.idVendor  == RTL8188EU_VID &&
          di->descriptor.idProduct == RTL8188EU_PID) {
         g_usbdev.pDevInfo  = di;
         g_usbdev.ctrlId    = di->ctrlID;
         g_usbdev.devAddr   = di->deviceAddress;
         g_usbdev.attached  = 1;
         rt_setmem((PVOID)g_tgl_out, 0, sizeof(g_tgl_out));      /* fresh device: every bulk endpoint starts on DATA0 */
         rt_setmem((PVOID)g_tgl_in, 0, sizeof(g_tgl_in));
         g_hw_ready  = 0;                    /* a (re)attached dongle is in power-on state: init again */
         g_hw_failed = 0;
         /* Accept the device. No USB transfers here: CHKSERV runs in USBD's callback context, so calling into USBD
          * (set configuration, probe) would re-enter it and fault it. The chip probe runs later at task time. */
         pRP->rph.Status = USB_IDC_RC_OK;
      } else {
         pRP->rph.Status = USB_IDC_RC_SERVREJCTD;
      }
      break;
   }

   case USB_IDC_FUNCTION_DETDEV: {            /* a device was detached - ours or any other one */
      RT_USBDETACH FAR *dd = (RT_USBDETACH FAR *)pRP->ParmPacket;

      if (!g_usbdev.attached || dd == 0 ||
          dd->controllerId != g_usbdev.ctrlId || dd->deviceAddress != g_usbdev.devAddr)
         break;                               /* somebody else's device (USB Ethernet, flash drive...): not our business */
      g_rx_run = 0;                           /* the outstanding RX request dies with the device */
      g_usbdev.attached = 0;
      g_usbdev.pDevInfo = NULL;
      usb_tx_reset();                         /* in-flight sends will never complete: free the slots */
      rtl8188eu_nic_detach();                 /* drop link/keys/queues; the next attach re-initialises the chip */
      break;
   }

   default:
      break;
   }
}

/* ===== asynchronous bulk RX / TX (data path) ==========================================
 * USBD copies the request block at submission, so the statics can be reused as soon as the call returns.
 * Completions arrive through rt_idc (process IRQ) in USBD's context: they must not block, but may submit new
 * requests. Tags in requestData1 route them. */
#define TAG_RX    1
#define TAG_TX0   2

static RT_USBRB g_rxrb;
static UCHAR    g_rxdata[USB_RXBUF];
static UCHAR    g_rx_ep;
USHORT          g_rx_run, g_rx_err, g_rx_errtot;
static USHORT   g_rx_fail, g_rx_lastrc, g_rx_laststat, g_rx_lastn, g_rx_done;   /* receive engine diagnostics */
static USHORT   g_tx_sub, g_tx_cmp, g_tx_stat;                       /* transmit diagnostics */
USHORT          g_tx_reclaims;                              /* diagnostics: reclaim rounds (endpoint cancels) */

/* Diagnostics for the read-back text: 0 controller id, 1 port, 2 IN max packet, 3 / 4 OUT 2 / 3 max packet,
 * 5 number of endpoint descriptors, 6.. endpoint address << 8 | attributes, 10..19 engine counters. */
USHORT usb_ep_info(USHORT what)
{
   UCHAR FAR *p;
   USHORT i, tot, n = 0;

   if (what == 69) return g_tx_reclaims;
   if (what == 70) return g_cancel_rc;
   if (what == 71) return g_stale;
   if (what == 72) return g_acc_lkf;
   if (what == 65) return g_tx_busy;
   if (what == 66) return g_tx_sub;
   if (what == 67) return g_tx_cmp;
   if (what == 68) return g_tx_stat;
   if (what == 60) return g_cb_max;
   if (what == 61) return g_cb_rec;
   if (what == 62) return g_sp_min;
   if (what == 63) return g_sp_max;
   if (what == 64) return g_cb_tot;
   if (what == 10) return g_rx_run;
   if (what == 11) return g_rx_fail;
   if (what == 12) return g_rx_lastrc;
   if (what == 13) return g_rx_laststat;
   if (what == 14) return g_rx_lastn;
   if (what == 15) return g_rx_done;
   if (what == 16) return g_tgl_bad;
   if (what == 19) return g_rp_none;
   if (what == 20) return g_txq_reenter;
   if (what == 21) return g_cpu_tx;
   if (what == 22) return g_txq_tmo;
   if (what == 17) return (USHORT)((g_tgl_ret << 4) | g_tgl_pred);
   if (what == 18) return (USHORT)((g_tgl_out[2] << 8) | g_tgl_out[3]);
   if (g_usbdev.pDevInfo == 0)
      return 0xFFFF;
   if (what == 0) return g_usbdev.pDevInfo->ctrlID;
   if (what == 1) return g_usbdev.pDevInfo->portNum;
   if (what == 2) return ep_mps(&g_usbdev, g_rx_ep, 1);
   if (what == 3) return ep_mps(&g_usbdev, 2, 0);
   if (what == 4) return ep_mps(&g_usbdev, 3, 0);
   p   = g_usbdev.pDevInfo->configurationData;
   tot = (USHORT)((USHORT)p[2] | ((USHORT)p[3] << 8));
   if (tot == 0 || tot > 512)
      tot = 512;
   for (i = 0; (USHORT)(i + 7) <= tot && p[i] >= 2; i = (USHORT)(i + p[i]))
      if (p[i + 1] == 0x05) {
         if (what >= 6 && n == (USHORT)(what - 6))
            return (USHORT)(((USHORT)p[i + 2] << 8) | p[i + 3]);   /* address << 8 | attributes */
         n++;
      }
   if (what >= 6)
      return 0xFFFF;
   return n;
}

typedef struct _txslot {
   RT_USBRB rb;
   UCHAR    busy;
   UCHAR    buf[USB_TXBUF];
} txslot_t;
static txslot_t g_txs[USB_TXSLOTS];
static ULONG    g_tx_t0[USB_TXSLOTS];                       /* ms clock when each slot was submitted */
static RT_USBCANCEL g_cancel_tx;
#define TX_STUCK_MS 2000UL
USHORT          g_tx_busy, g_tx_err;

/* Physical address of the first / last byte of each DMA buffer the USB host controller reads or writes
 * (i: 0/1 control+sync bulk buffer, 2/3 receive buffer, 4/5 first async TX slot; half 1 = high word). If first and
 * last do not differ by exactly size-1 the buffer is not physically contiguous. */
USHORT usb_phys(USHORT i, USHORT half)
{
   static union { ULONG l; USHORT w[2]; } u;
   PVOID a;

   if (half) {
      u.l = 0;
      if (i == 0)      a = (PVOID)g_databuf;
      else if (i == 1) a = (PVOID)(g_databuf + sizeof(g_databuf) - 1);
      else if (i == 2) a = (PVOID)g_rxdata;
      else if (i == 3) a = (PVOID)(g_rxdata + sizeof(g_rxdata) - 1);
      else if (i == 4) a = (PVOID)g_txs[0].buf;
      else             a = (PVOID)(g_txs[0].buf + USB_TXBUF - 1);
      rt_dh_virt_to_phys(a, &u.l);
   }
   return u.w[half ? 1 : 0];
}

int usb_rx_start(usb_dev_t *dev, UCHAR ep)
{
   USHORT st;

   rt_setmem((PVOID)&g_rxrb, 0, sizeof(g_rxrb));
   g_rxrb.controllerId  = dev->ctrlId;
   g_rxrb.deviceAddress = dev->devAddr;
   g_rxrb.endPointId    = ep;
   g_rxrb.flags         = (USHORT)(USRB_FLAGS_DET_BULK | USRB_FLAGS_TTYPE_IN);
   if (g_tgl_in[ep & 0x0F])
      g_rxrb.flags |= USRB_FLAGS_DET_DTGGLEON;
   g_rxrb.buffer1       = (PUCHAR)g_rxdata;
   g_rxrb.buffer1Length = sizeof(g_rxdata);
   g_rxrb.serviceTime   = USB_DEFAULT_SRV_INTV;
   g_rxrb.maxPacketSize = ep_mps(dev, ep, 1);
   g_rxrb.maxErrorCount = USB_MAX_ERROR_COUNT;
   g_rxrb.requestData1  = TAG_RX;
   g_rxrb.usbIDC        = idc_addr();
   g_rxrb.usbDS         = rt_getds();
   g_rxrb.category      = USB_IDC_CATEGORY_CLIENT;
   g_rx_ep  = ep;
   g_rx_run = 1;
   st = usbd_call(USB_IDC_FUNCTION_ACCIO, (PVOID)&g_rxrb);
   if (st & STERR) {
      g_rx_run = 0;
      g_rx_fail++;
      g_rx_lastrc = st;
      return -1;
   }
   return 0;
}

void usb_rx_stop(void)
{
   g_rx_run = 0;                       /* the outstanding request just won't be resubmitted */
}

/* A transmit request that the device never takes (its FIFO stays full, or a completion is lost) keeps its slot busy for
 * ever; with all slots gone the protocol stack is told "out of resources" on every send. usb_tx_stuck() reports a slot that
 * has been in flight for more than TX_STUCK_MS; usb_tx_reclaim() then cancels the transmit endpoints, which makes USBD
 * complete the requests with a "cancelled" status, and the completion frees the slots in the normal way (the slot is never
 * reused before its completion arrives, so the request block stays valid). Once per second at most. */
int usb_tx_stuck(void)
{
   ULONG  now = rtl8188eu_ms();
   USHORT i;

   if (now == 0)
      return 0;
   for (i = 0; i < USB_TXSLOTS; i++)
      if (g_txs[i].busy && g_tx_t0[i] != 0 && (long)(now - g_tx_t0[i]) > (long)TX_STUCK_MS)
         return 1;
   return 0;
}

static void usb_tx_reclaim(void)
{
   static ULONG last;
   ULONG        now = rtl8188eu_ms();
   UCHAR        ep;

   if (now == 0 || (long)(now - last) < 1000L || !usb_tx_stuck())
      return;
   last = now;
   g_tx_reclaims++;
   for (ep = 2; ep <= 3; ep++) {
      g_cancel_tx.controllerId  = g_usbdev.ctrlId;
      g_cancel_tx.deviceAddress = g_usbdev.devAddr;
      g_cancel_tx.endPointId    = ep;
      usbd_call(USB_IDC_FUNCTION_CANCEL, (PVOID)&g_cancel_tx);
   }
}

USHORT usb_tx_alloc(void)
{
   USHORT i;

   for (i = 0; i < USB_TXSLOTS; i++)
      if (!rt_tas(&g_txs[i].busy)) {               /* atomic claim: two senders must never get the same slot */
         rt_atomic_inc(&g_tx_busy);
         g_tx_t0[i] = 0;
         return i;
      }
#if RT_TX_RECLAIM
   usb_tx_reclaim();
#endif
   return 0xFFFF;
}

UCHAR *usb_tx_buf(USHORT slot)
{
   return g_txs[slot].buf;
}

void usb_tx_release(USHORT slot)
{
   if (g_txs[slot].busy) {
      g_txs[slot].busy = 0;
      rt_atomic_dec(&g_tx_busy);
   }
}

void usb_tx_reset(void)
{
   USHORT i;

   for (i = 0; i < USB_TXSLOTS; i++)
      g_txs[i].busy = 0;
   g_tx_busy = 0;
}

/* Queue a filled slot. Caller holds usb_txq_lock (the toggle below must follow the submission order). On failure
 * the slot is released. */
int usb_tx_submit_locked(usb_dev_t *dev, UCHAR ep, USHORT slot, USHORT len)
{
   USHORT    st;
   txslot_t *t = &g_txs[slot];

   rt_setmem((PVOID)&t->rb, 0, sizeof(t->rb));
   t->rb.controllerId  = dev->ctrlId;
   t->rb.deviceAddress = dev->devAddr;
   t->rb.endPointId    = ep;
   t->rb.flags         = (USHORT)(USRB_FLAGS_DET_BULK | USRB_FLAGS_TTYPE_OUT);
   if (g_tgl_out[ep & 0x0F])
      t->rb.flags |= USRB_FLAGS_DET_DTGGLEON;
   t->rb.buffer1       = (PUCHAR)t->buf;
   t->rb.buffer1Length = len;
   t->rb.serviceTime   = USB_DEFAULT_SRV_INTV;
   t->rb.maxPacketSize = ep_mps(dev, ep, 0);
   t->rb.maxErrorCount = USB_MAX_ERROR_COUNT;
   t->rb.requestData1  = (ULONG)(TAG_TX0 + slot);
   t->rb.usbIDC        = idc_addr();
   t->rb.usbDS         = rt_getds();
   t->rb.category      = USB_IDC_CATEGORY_CLIENT;
   st = usbd_call(USB_IDC_FUNCTION_ACCIO, (PVOID)&t->rb);
   if (st & STERR) {
      usb_tx_release(slot);
      g_tx_err++;
      return -1;
   }
   g_tgl_out[ep & 0x0F] ^= (UCHAR)(tgl_packets(len, t->rb.maxPacketSize) & 1);   /* queued: the toggle for the next request */
   g_tx_t0[slot] = rtl8188eu_ms();
   g_tx_sub++;
   return 0;
}

/* Queue a filled slot under the TX queue lock. -2: the lock was refused (re-entered or timed out): the slot is released
 * and the caller reports "out of resource". */
int usb_tx_submit(usb_dev_t *dev, UCHAR ep, USHORT slot, USHORT len)
{
   USHORT fl;
   int    rc;

   fl = usb_txq_lock();
   if (fl == 0xFFFF) {
      usb_tx_release(slot);
      return -2;
   }
   rc = usb_tx_submit_locked(dev, ep, slot, len);
   usb_txq_unlock(fl);
   return rc;
}

static void async_complete(RT_USBRB FAR *rb)
{
   ULONG tag = rb->requestData1;

   if (tag == TAG_RX) {
      USHORT n = (USHORT)rb->buffer1Length;
      g_rx_done++;
      g_rx_laststat = (USHORT)rb->status;
      g_rx_lastn = n;
      if (rb->status == 0) {
         UCHAR pred = (UCHAR)(g_tgl_in[g_rx_ep & 0x0F] ^ (tgl_packets(n, 512) & 1));
         UCHAR ret  = (UCHAR)((rb->flags & USRB_FLAGS_DET_DTGGLEON) ? 1 : 0);   /* toggle of the last packet, as the controller saw it */

         g_tgl_pred = pred;
         g_tgl_ret  = ret;
         if (ret == pred)                     /* the controller's "last packet" toggle must be the opposite of our next one */
            g_tgl_bad++;
         g_tgl_in[g_rx_ep & 0x0F] = pred;
      }
      if (rb->status == 0 && n > 0 && n <= USB_RXBUF) {
         g_rx_err = 0;
         rtl_rx_feed(g_rxdata, n);                  /* parse + hand frames up (no blocking!) */
      } else if (rb->status != 0) {
         g_rx_err++;
         g_rx_errtot++;
      }
      if (g_rx_run && g_rx_err < 8)
         usb_rx_start(&g_usbdev, g_rx_ep);          /* keep one IN request outstanding */
      else if (g_rx_run) {
         g_rx_run = 0;
         tcom('!'); tcom('R');                      /* too many errors: engine stopped */
      }
   } else {
      USHORT slot = (USHORT)(tag - TAG_TX0);
      g_tx_cmp++;
      g_tx_stat = (USHORT)rb->status;
      if (slot < USB_TXSLOTS) {
         if (rb->status != 0)
            g_tx_err++;
         usb_tx_release(slot);
      }
   }
}
