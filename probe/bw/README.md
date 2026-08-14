# B&W / G3 Temp Sensor Diagnostic

A small, **strictly read-only** Mac OS 9 application that answers one question for
any Power Mac G3/G4: *is any CPU/system temperature readable from OS 9 on this
machine?*

It was written for the Power Mac G3 Blue & White (PowerMac1,1 "Yosemite"), where
the CPU Temp Control Strip module comes up empty — but it works on any G3/G4 and
is handy whenever a machine shows `n/a` and you want to know *why*.

## What it does

It tries every avenue and prints a plain-language verdict at the bottom:

- **[A]** CPU + machine identity (Gestalt, and the device-tree root's
  `compatible`, e.g. `PowerMac1,1`)
- **[B]** `GetCoreProcessorTemperature` (the PowerMgrLib OS call)
- **[C]** I²C controllers — lists **every** one (matched by name / compatible /
  device_type), and does a read-only temperature-sensor address sweep **only** on
  Uni-N/KeyWest controllers
- **[D]** device-tree temperature/thermal/sensor nodes — the protocol-independent,
  authoritative "does this machine even *declare* a sensor?" check
- **VERDICT**

## Safety

Strictly **read-only**: it only reads sensor registers on known-safe (KeyWest/
Uni-N) i²c controllers, never writes a sensor/fan/config register, and never
drives an i²c controller of an unknown design (it lists those but does not touch
them). It does **not** access any privileged CPU register.

## How to run

1. Download `dist/BWProbe.hqx` (BinHex, safest for old Macs) or `dist/BWProbe.bin`
   (MacBinary II).
2. Expand it on the Mac (StuffIt Expander handles either).
3. Double-click `BWProbe`. A console window opens and prints the report.
4. It ends with `(Press Return to quit.)`.

The console has no scrollback, so the important part (the `VERDICT` block and the
`[C]`/`[D]` sections) is at the very bottom. A photo of the lower half is enough.

## Building from source

`bw_probe.c` + `CMakeLists.txt` build with the Retro68 PowerPC toolchain, the
same as the CSM (see the repository `BUILD.md`).
