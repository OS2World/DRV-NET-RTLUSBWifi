# Building

Open-source tools only: Open Watcom C/C++ 2.0 (16-bit compiler `wcc`, linker `wlink`) and NASM 2.11 or later,
on ArcaOS / OS/2. Nothing from the IBM Device Driver Kit is needed.

Each driver has its own `src` folder with its own `build.cmd` and `rtl88eu.lnk`:

* `rtl8821au\src` builds `RTL8821AU.OS2`
* `rtl8188eu\src` builds `RTL88EU.OS2` (use `build_vm.cmd` to compile every file in one go; `build.cmd file ...`
  recompiles only the named files and then links)

## Steps

1. Put `C:\WATCOM\binp` (Open Watcom OS/2 host tools) and NASM on the `PATH`, set `WATCOM=C:\WATCOM`, and leave
   `INCLUDE` empty (the drivers include none of the Open Watcom headers).
2. In the `src` folder run the build script. It assembles `rt_hdr.asm`, `rt_lib.asm`, `rt_ndisx.asm` and `rt_end.asm`
   with `nasm -f obj`, compiles the C files with

   ```
   wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j file.c
   ```

   and links with `wlink @rtl88eu.lnk` (`format os2 dll`, code segments marked IOPL, `nodefaultlibs`).
3. Flags that matter: `-2` (286 code; `-6` lets the compiler use 32-bit registers in 16-bit code), `-ms` small model,
   `-zu` (SS is not DS in a driver), `-zl` (no default libraries), `-zp1` (packed structures), `-j` (plain `char` is
   signed), `-s` (no stack checks).

## Constraints

* The data group (header, data, constants, BSS) must stay below 64 KB; both builds are close to the limit. Reduce
  buffers before adding statics.
* `rt_hdr.asm` must be linked first (device header at offset 0, segment order) and `rt_end.asm` last (resident-end
  markers for the INIT packet).
* No 32-bit multiply, divide or variable shift (there is no runtime library): the link fails with an undefined symbol
  if one is introduced.
* The SMP helpers use 486/Pentium instructions (`CMPXCHG`, `CPUID`, `PAUSE`).
* `DBG_ADMIN` in `rt_usb.h`: 0 for releases (the debug channel is read-only), 1 for development (the debug channel can
  write chip registers and force a re-initialisation).
