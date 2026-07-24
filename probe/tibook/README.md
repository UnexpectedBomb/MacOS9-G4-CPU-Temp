# TiBook Temp Sensor Diagnostic

A tiny, **strictly read-only** Mac OS 9 application that answers one question for a
PowerBook G4 Titanium: *is any CPU/system temperature actually readable from OS 9?*

It was written to settle whether the Titanium PowerBooks expose a temperature
sensor. On a 667 MHz TiBook the answer turned out to be **yes**: a DS1775-class
sensor sits at I²C address `0x49` on the Uni-N/KeyWest bus (the same part the
Power Mac G4 MDD uses), so the CPU Temp Control Strip module reads it fine. We do
not yet know whether every TiBook model is the same, which is what this tool
checks.

## What it does

It tries every avenue and prints a plain-language verdict at the bottom:

- **[A]** CPU identity (Gestalt)
- **[B]** `GetCoreProcessorTemperature` (the PowerMgrLib OS call)
- **[C]** I²C controllers + a read-only temperature-sensor address sweep
  (0x2C–0x2F and 0x48–0x4F, both bus channels)
- **[D]** device-tree temperature/thermal/sensor nodes
- **VERDICT** — whether anything readable was found

## Safety

It is **read-only on every bus**: it only reads sensor registers, never writes a
sensor, fan, or configuration register, and it does **not** touch any privileged
CPU register (an earlier version did, and that faulted on the 745x; it was
removed). It cannot disturb the fan or the machine.

## How to run

1. Download `dist/TiBookProbe.hqx` (BinHex, safest for old Macs) or
   `dist/TiBookProbe.bin` (MacBinary II).
2. Expand it on the Mac (StuffIt Expander handles either).
3. Double-click `TiBookProbe`. A console window opens and prints the report.
4. It ends with `(Press Return to quit.)`.

The console has no scrollback, so the important part (the `VERDICT` block, and the
`[C]` i²c section) is at the very bottom. A photo of the lower half of the window
is all that is needed.

## Building from source

`tibook_probe.c` + `CMakeLists.txt` build with the Retro68 PowerPC toolchain, the
same as the CSM (see the repository `BUILD.md`).
