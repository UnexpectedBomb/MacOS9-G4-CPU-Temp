# Building

Both targets build with the [Retro68](https://github.com/autc04/Retro68) PowerPC toolchain
(the same cross-compiler used for the other MacOS9-* projects). You need a Retro68 build with
the `powerpc-apple-macos` toolchain; the examples below assume it's at `$HOME/Retro68-build`.

## The Control Strip module (`csm/`)

```sh
cd csm
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build
```

Output: `build/CPUTempCSM.bin` (MacBinary). The build:

1. compiles `cpu_temp_csm.c` and links it as a code resource
   (`-Wl,-eControlStripModule`), linking `ControlStripLib` (the `SB…` helpers) and
   `NameRegistryLib` (device-tree access);
2. runs `MakePEF` to produce the PowerPC code fragment;
3. Rez's it into an `'sdev'` resource together with the Finder icon family
   (`therm_icon.r`), stamping file type `sdev` / creator `CPUt`;
4. runs `scripts/set-custom-icon.py` to set the Finder's `kHasCustomIcon` flag (Rez can't).

Copy `build/CPUTempCSM.bin` to the Mac, decompress, and drop the result into
**System Folder ▸ Control Strip Modules**; restart.

### Regenerating the icon

`csm/therm_icon.r` contains the icon family (`ICN#`/`icl8`/`ics#`/`ics8` at ID `-16455`).
`scripts/generate-icon.py` can emit a procedural thermometer icon if you want a starting point;
the shipped icon was drawn in Iconographer and its 1-bit planes synthesized from the colour art.

## The diagnostic probe (`probe/`)

A standalone console application that finds the I²C controller, prints the discovered
`AAPL,address`/registers, reads the DS1775 (auto-scanning modes/channels with a per-step
trace), and takes several live readings. Handy for bringing this up on a different machine.

```sh
cd probe
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/Retro68-build/toolchain/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake --build build
```

Output: `build/CPUTempProbe.bin`. Run it on the Mac; it opens a console window and reports.
See [`probe/OF-RECON.md`](probe/OF-RECON.md) for the Open Firmware commands used to confirm the
sensor topology before any code was written.
