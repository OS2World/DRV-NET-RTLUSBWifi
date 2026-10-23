# Building RTL88EU 0.1.2

Open-source tools only. Tested with Open Watcom C/C++ 2.0.1 (16-bit compiler `wcc`, linker `wlink`) and NASM 2.11
on ArcaOS 5.1.1. Nothing from the IBM Device Driver Kit is needed.

## Build

1. Put `C:\WATCOM\binp` (Open Watcom OS/2 host tools) and NASM on the `PATH`, set `WATCOM=C:\WATCOM`, and leave
   `INCLUDE` empty (the driver includes none of the Open Watcom headers).
2. Run `build.cmd` in the `src` directory. It assembles `rt_hdr.asm`, `rt_lib.asm`, `rt_ndisx.asm` and `rt_end.asm`
   with `nasm -f obj`, compiles the C files with

   ```
   wcc -bt=os2 -6 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j file.c
   ```

   and links with `wlink @rtl88eu.lnk` (`format os2 dll`, code segments marked IOPL, `nodefaultlibs`; the four
   `DOSCALLS` imports are named in the link file). `build.cmd file1 file2 ...` recompiles only those C files.
   The result is `RTL88EU.OS2` and a map file.
3. Flags that matter: `-ms` small model, `-zu` (SS is not DS in a driver), `-zl` (no default libraries), `-zp1`
   (packed structures), `-j` (plain `char` is signed), `-s` (no stack checks).

## Constraints

* The data group (header, data, constants, BSS) must stay below 64 KB: the current build leaves about 1.5 KB.
* `rt_hdr.asm` must be linked first (the device header is at offset 0 of the data group, and it declares the segment
  order) and `rt_end.asm` last (its labels are the resident-end markers the INIT packet reports).
* No 32-bit multiply, divide or variable shift (there is no runtime library): the link fails with an undefined
  symbol if one is introduced.
