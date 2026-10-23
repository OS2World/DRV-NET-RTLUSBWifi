# Provenance register - external facts used by the new runtime

Each row: the fact, where it comes from, and the file that uses it. A fact marked **INTERFACE** is a number or
layout that is part of a binary interface and is checked by behaviour on a running system, not by copying text.
The documents are IBM/Arca Noae documentation used as specifications; they are not reproduced in this repository.

Sources
* **PDD** = IBM *Physical Device Driver Reference* (DDK 2004: `Docs-PDF/PDDREF.pdf`, `HTML/REF/PDD/*.HTM`).
* **USBPRM** = IBM *Basic USB Device Driver Stack for OS/2 Warp - Programmer's Reference*, v1.1, 2002/2004
  (DDK 2004: `.../dev/usb/USBprm.htm`).
* **USB2** = *Universal Serial Bus Specification* revision 2.0 (usb.org).
* **NDIS** = *NDIS 2.0.1 specification* (3Com/Microsoft, 1990) and the DDK LAN reference (`HTML/REF/LAN`).
* **OBS** = observed on a running system (this project's own tests).

## 1. DevHelp function codes (DL register) - PDD, "DevHlp Services and Function Codes" (page 00207)

| Service | Code | Used for |
|---|---|---|
| ProcBlock | 04h | wait for a USB completion, timed delays |
| ProcRun | 05h | wake the waiting thread from the completion callback |
| VirtToPhys | 16h | diagnostics (buffer physical addresses) |
| GetDOSVar | 24h | millisecond clock (global info segment) |
| VerifyAccess | 27h | validate caller buffers in the IOCtl path |
| AttachDD | 2Ah | find USBD's IDC entry point |
| AllocGDTSelector | 2Dh | selector for caller buffers |
| PhysToGDTSelector | 2Eh | map caller's physical buffer |
| Save_Message | 3Dh | boot messages |
| FreeGDTSelector | 53h | release the selector |

Register-level calling sequences: PDD pages 00227 (AllocGDTSelector), 00231 (AttachDD), 00239 (FreeGDTSelector),
00245 (GetDOSVar), 00258 (PhysToGDTSelector), 00262 (ProcBlock), 00263 (ProcRun), 00283 (Save_Message),
00299 (VerifyAccess), 00302 (VirtToPhys).

## 2. Device header - PDD, "Physical Device Driver Header" (page 00020)

First item of the first data segment: far pointer to next header (DWORD), device attribute (WORD), offset of
strategy routine (WORD), offset of IDC entry point (WORD), driver name (8 bytes), reserved (8 bytes),
capabilities bit strip (DWORD, level-3 drivers). Attribute bits: PDD page 00022.

## 3. USB class/client interface - USBPRM section 6

| Fact | Section |
|---|---|
| IDC call format: request packet with Cmd = 10h (generic IOCtl), Category and Function fields | 6.1 |
| Categories: HOST 90h, USBD 91h, CLASS 92h, CLIENT 93h | 6.1 |
| Functions: ACCIO 41h, CANCEL 42h, REGISTER 43h, PRCIRQ 44h, CHKSERV 45h, DETDEV 46h, SETCONF 48h, CLRSTALL 4Ah | 6.1 |
| Return codes: 0100h done/ok; 8113h parameter error; 8114h allocation failure; 8115h no bandwidth; 8118h device or endpoint absent | 6.1.x |
| "Rejected" return code: the document says 8019h for CHKSERV; the same document's table for the other codes shows the high bit (STERR) set only for errors. **INTERFACE**: decide by test which value USBD treats as "not mine" (see section 7) | 6.1.3.3 |
| Request block fields, order, sizes and flag bits (transfer type low byte, high byte incl. **data toggle bit 0800h**: "set/cleared on request completion") | 6.1.1.1 |
| Cancel: controllerId, deviceAddress, endPointId (FFh = all endpoints) | 6.1.1.2 |
| Detach: controllerId, deviceAddress | 6.1.3.4 |
| Accept/reject: pointer to DeviceInfo; DeviceInfo fields (ctrlID, deviceAddress, bConfigurationValue, bInterfaceNumber, lowSpeedDevice, portNum, parentHubIndex, rmDevHandle, clearStalled, descriptor, configurationData) | 6.1.3.3 |
| Detach messages are sent for devices, class/client drivers must compare controller and address | 6.1.3.4 + OBS (unplugging an unrelated USB Ethernet adapter reset the driver in 0.1) |
| Data toggle must be supplied by the driver per request | 6.1.1.1 + OBS (EHCI drops every second frame otherwise; xHCI ignores the flag) |

DeviceInfo field offsets (needed to reach the device descriptor and configuration data): derived from the field
sizes in the table above (1,1,1,1,1,1,2,4,8,18 then the configuration data) = descriptor at offset 20, configuration
data at offset 38. **INTERFACE**: verified by OBS (the vendor/product id read from `descriptor` matches 0BDA:8179 and
the endpoint descriptors parsed from `configurationData` match what the device reports).

Facts used by `rt_usb.h` / `rt_usb.c` in addition to the table above:

| Fact | Source |
|---|---|
| USBRB layout: the field list and sizes of USBPRM 6.1.1.1 in order, packed (pointers are far, ULONG request data, 55 bytes) | USBPRM 6.1.1.1; **INTERFACE**: packing and the far pointer sizes verified by OBS (transfers complete with the right data) |
| DeviceInfo field sizes (HDEVICE = 4 bytes) giving descriptor at offset 20 and configuration data at offset 38 | USBPRM 6.1.3.3; **INTERFACE**, verified by OBS |
| IDC call/entry convention: a far call to the target's IDC entry with DS = the target's data segment and the far request-packet pointer on the stack (C calling convention, caller removes it) | USBPRM 6.1 ("void IDCRoutine(RPH_GENIOCTL FAR *)") + PDD "AttachDD" (DS set by the caller); **INTERFACE**: the cdecl/stack form is confirmed by OBS (the working 0.1.1 driver's entry has exactly this form) |
| AttachDD result area: 3 reserved words, IDC entry offset, IDC entry selector, IDC data segment | PDD "AttachDD" |
| Request packet for generic IOCtl: header, category, function, parameter pointer, data pointer, system file number, parameter length, data length | PDD "GENERIC IOCtl - 10h" |
| Status codes of the IDC replies: 0x0100 accepted; reject = 0x0119 | USBPRM says 0x8019 for the reject of CHKSERV and the table above gives 0x8119-style codes elsewhere; **INTERFACE**: 0x0119 is the value of the working 0.1.1 driver (other drivers still get their devices) |

## 4. USB descriptors - USB2 chapter 9

Device descriptor (18 bytes), configuration/interface/endpoint descriptors, SET_CONFIGURATION, CLEAR_FEATURE
(ENDPOINT_HALT resets the data toggle), bulk max packet sizes (512 high speed, 64 full speed), data toggle (DATA0/DATA1).

## 5. Request packets and strategy commands - PDD pages 00100, 00101, 00102 (filled in as each is written)

Used by `rt_os2dd.h` / `rt_strat.c` (toolchain spike):

| Fact | Source | Status |
|---|---|---|
| Request packet header, 13 bytes: length, unit, command (BYTEs), status (WORD), reserved (DWORD), queue link (DWORD) | PDD "Request Packets" | spec |
| INIT = command 00h; block after the header: Data_1 BYTE (units, out), Pointer_1 DWORD (in: DevHlp entry; out: WORD end-of-code offset, WORD end-of-data offset), Pointer_2 DWORD (in: DEVICE= argument string), Data_2 BYTE (in: first drive) | PDD "INIT" | spec; verified OBS by the spike loading |
| OPEN = 0Dh, CLOSE = 0Eh (sent only with attribute bit 11) | PDD command summary | spec |
| Status word: 0100h done; bit 15 = error, low byte error code; code 03h unknown command | PDD "Request Packets" status word | spec |
| Strategy entry: far call, request packet at ES:BX; routine returns far | PDD "Strategy Entry Point" | spec |
| Device attribute word used: 0C980h = bit 15 character device, bit 14 IDC entry, bit 11 open/close, bits 9-7 = 011 (capabilities strip) | PDD page 00022 | spec; OBS |
| Save_Message (DevHlp 3Dh): DS:SI = message table (WORD id, WORD count, DWORD far pointers to fill-in strings), BX = 0, DL = 3Dh, far call through the DevHlp entry | PDD "Save_Message" (page 00283) | spec |
| Message id whose text is only "%1" (id 1178 used in the 0.1.x driver) | not in the PDD; lives in the system message file | **INTERFACE**: check by the boot message appearing (spike) |
| Module flags of the working 0.1.1 driver (NE flags 8009h: library + protected-mode-only + single data) differ from the Open Watcom default (8201h) | OBS (comparison of the headers) | to verify that the loader does not care |

Link-time facts (OW 2.0.1 wlink, OBS): `format os2 dll` + `segment class 'CODE' iopl` yields an NE image with the data
group (header first) as segment 1 and the code segment with IOPL as segment 2; `eliminate`, `nodefaultlibs`; the map
shows the end-marker offsets (data 0062h, code 009Ah in the spike).

## 6. NDIS 2.0.1 MAC - NDIS (3Com/Microsoft, 1990; DDK 2004 `HTML/REF/LAN/LANMANG.HTM` and `NDISPG.HTM`)

The NDIS 2.0.1 specification states that it may be copied freely and that products may be built from it
(copyright notice at its end). Used by `rt_ndis.h`, `rt_ndis.c`, `rt_ndisx.asm`:

| Fact | Source |
|---|---|
| Common characteristics table: field order and sizes (size, NDIS version bytes, reserved, module version, flags, 16-byte name, protocol/interface levels at both boundaries, module id, module DS, system request entry, pointers to the four subtables, two reserved) | LANMANG "Common Characteristics" |
| MAC service-specific characteristics (type name, station address length and addresses, functional address, multicast list pointer, link speed, service flags, max frame size, buffer capacities and block sizes, vendor code, adapter description, IRQ, transmit queue depth, max data blocks) and the multicast list | LANMANG "MAC Service-Specific Characteristics" |
| MAC service-specific status table (length, diagnostics date, MAC status bits 0-2 / 3 bound / 4 open, packet filter, statistics pointer and the ordered statistics counters; 0xFFFFFFFF = not supported) | LANMANG "MAC Service-Specific Status Table" |
| MAC upper dispatch table (back pointer, Request, TransmitChain, TransferData, ReceiveRelease, IndicationOn, IndicationOff) and protocol lower dispatch table (back pointer, interface flags, RequestConfirm, TransmitConfirm, ReceiveLookahead, IndicationComplete, ReceiveChain, StatusIndication) | LANMANG "MAC Upper Dispatch Table", "Protocol Lower Dispatch Table" |
| Calling convention of every NDIS entry: Microsoft C FAR Pascal (arguments pushed left to right, the last one is the called module's DS, callee removes them, caller's DS saved and restored, SI/DI preserved, direction flag clear) | LANMANG "calling conventions" |
| System request (Param1 dword, Param2 dword, Param3 word, Opcode word, TargetDS); Bind = opcode 2: Param1 = caller's table, Param2 = where to return ours | LANMANG "System requests", "Bind" |
| General request (ProtID, ReqHandle, Param1 word, Param2 dword, Opcode, MACDS) and the opcode numbers (OpenAdapter 4, CloseAdapter 5, ResetMAC 6, SetPacketFilter 7, ...) | LANMANG "General Requests" |
| TransmitChain (ProtID, ReqHandle, TxBufDescr far, MACDS); transmit buffer descriptor (immediate length + pointer, count, then blocks of pointer type 0 physical / 2 GDT, reserved byte, length, address) | LANMANG "TransmitChain", "Transmit Buffer Descriptor" |
| ReceiveChain indication (MACID, FrameSize, ReqHandle, RxBufDescr, Indicate, ProtDS), indicate flag semantics, WAIT_FOR_RELEASE and ReceiveRelease | LANMANG "ReceiveChain", "ReceiveRelease" |
| IndicationOn/IndicationOff: nesting reference count, both return with interrupts disabled, the MAC implicitly disables indications before an indication call | LANMANG "IndicationOff", "IndicationOn" |
| Return codes (0 success, 1 wait for release, 2 request queued, 6 out of resource, 7 invalid parameter, 8 invalid function, 9 not supported, 0xFF general failure, 0x20 already started, 0x22 driver not initialized) | LANMANG return code list |
| Protocol Manager: device PROTMAN$, request block of 14 bytes (opcode, status, pointer 1, pointer 2, word 1), RegisterModule = opcode 2 with pointer 1 = common characteristics table and pointer 2 = NULL for a MAC, issued with DosDevIOCtl category 81h function 58h | LANMANG "RegisterModule", NDISPG "Protocol Manager Requests" |
| 16-bit DOS API used at INIT: DosOpen, DosClose, DosRead, DosDevIOCtl (Pascal convention; ordinals 70, 59, 137, 53 of DOSCALLS) | OS/2 Control Program Programming Reference (signatures); **INTERFACE** ordinals checked by OBS (they are the imports of the working 0.1.1 module) |
| `LOWER_PROTOCOL_USB` = 0 / interface 0 in the common table | spec says 0 = physical; value unchanged from the working 0.1.1 driver |

Behaviour decisions that are the project's own: receive buffers are handed to the protocol as one-block ReceiveChain
indications; a frame is dropped (the protocol recovers by retransmission) when indications are off; TransmitChain
copies the data and returns SUCCESS (no confirmation). IndicationOff waits (bounded) for an indication running on
another CPU, because on an SMP kernel disabling local interrupts does not exclude it.


## 7. Verified by behaviour (INTERFACE)

| Fact | Check | Result |
|---|---|---|
| Loader accepts an NE module with data group first, header at offset 0, module flags 8201h (no protected-mode-only bit) | spike driver loaded and opened in the VM | OK (2026-10-21) |
| USBRB / DeviceInfo / cancel / setconf / class-registration layouts; IDC call and entry convention | the 0.1.2 build joins mambo and moves data on PC1 (EHCI, SMP kernel) | OK (2026-10-21) |
| NDIS tables, stubs and Protocol Manager registration | TCP/IP binds, lan1 gets its DHCP lease, ping and downloads work on PC1 | OK (2026-10-21) |
| DOSCALLS ordinals 70, 59, 137, 53 | the NDIS registration and PROTOCOL.INI read at INIT succeed | OK |
| Boot message id 1178 ("%1") | to be seen with the `/V` switch | pending |
| CHKSERV reject code 0119h | other USB devices (mouse, keyboard, Ethernet adapter, hub) still bind to their own drivers with this build loaded | PC1 boots with its USB devices; USB Ethernet not yet tested with this build |

## 8. Project code carried over (honest account)

The 0.1.x driver was written by the same author (the assistant working on this project) who had read the IBM sample
sources. The files below were rewritten from the specifications above, with new names, and not edited from the IBM-derived
originals; the *behaviour* that the project itself had worked out on real hardware was preserved on purpose, because it
is our own engineering and was found by test, not copied from any sample:

* `rt_usb.c`: per-endpoint data toggle tracking; broadcast-detach filter by controller id and address; pool of request
  packets claimed with an atomic exchange; atomic transmit slot claim; bulk endpoint maximum packet size read from the
  configuration descriptor; asynchronous receive request resubmitted from its own completion.
* `rt_ndis.c`: single-block ReceiveChain indications with a buffer pool and ReceiveRelease; transmit gather into one
  frame; lanN lookup in PROTOCOL.INI; indication exclusion for SMP (new in 0.1.2).
* `rt_strat.c`: the `/V /Q /W /AA` switches (EDM2 driver-argument suggestion), INIT COMPLETE registration, device read
  = DHCP lease text, device write = debug commands.

A reviewer other than the author should still read this register and spot-check the new files before publication.
