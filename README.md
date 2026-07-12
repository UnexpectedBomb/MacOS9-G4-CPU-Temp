# Mac OS 9 — G4 CPU & Case Temperature (Control Strip module)

A **Control Strip module that shows live CPU and case temperature on the Power Mac G4
"Mirrored Drive Doors" (PowerMac3,6) under Mac OS 9** — read straight from the machine's
real hardware sensors over the Uni-N / "KeyWest" I²C bus. It's silent, loads at startup, and
sits in the Control Strip like a stock module.

As far as I can tell this is the **first accurate CPU-temperature readout for Mac OS 9 on the
7455-based Power Macs**. The one prior tool for classic Mac OS (the temperature module in
*Jeremy's CSM Bundle*) reads the PowerPC's on-chip Thermal Assist Unit (TAU) — which Motorola
disabled on the 7450/7455 family, so it can't read these machines at all. This driver ignores
the TAU and talks to the actual **DS1775** CPU sensor and **ADM1030** case sensor instead, the
same chips Apple's own firmware uses — giving a real reading to **0.5 °C** where the TAU spec
was only ±12 °C.

> **Status: working, seeking testers.** Developed and verified on a dual-1.25 GHz MDD
> (PowerMac3,6) running Mac OS 9.2.2. It should apply to other Uni-N G4s that carry a DS1775 +
> ADM1030 on the Uni-N I²C bus, but it has only been exercised on one machine — testing on
> other models is exactly how we build confidence. It is **strictly read-only** on the
> sensors (see *Safety* below).

## Features

- **CPU temperature** — from the DS1775 digital thermometer (I²C 0x49), 0.5 °C resolution.
- **Case temperature** — from the ADM1030 fan controller's local sensor (I²C 0x2c).
- **Click for a menu** — pick the reading (CPU / Case) and the units (°F / °C) as two
  independent groups; units apply to both.
- **Turns red** when the CPU reaches 10 °C below the safe limit (≥ 75 °C), or the case gets hot.
- **Over-temperature alert** — if the CPU exceeds the safe limit (≥ 85 °C) a Notification
  Manager alert pops up ("consider shutting down…"). It's latched, so it warns once per event,
  not in a loop, and it keeps watching the CPU even while you're viewing the case temperature.
- **Fixed-width cell**, bold text, and a custom Control-Strip-style icon.

Thresholds are conservative defaults for this sensor (the 7455's junction limit is ~105 °C;
the MDD idles around 40–55 °C). They're `#define`s at the top of
[`csm/cpu_temp_csm.c`](csm/cpu_temp_csm.c) if you want to change them.

## Install

1. Download **`dist/CPUTempCSM.bin`** (a MacBinary file) and decompress it on the Mac
   (Stuffit Expander, or any tool that unpacks MacBinary) → a file named `CPUTempCSM`.
2. Drop `CPUTempCSM` into **System Folder ▸ Control Strip Modules**.
3. **Restart.** The temperature tile appears in the Control Strip. Click it to switch reading
   or units.

(Or build it yourself — see [BUILD.md](BUILD.md).)

## Safety

This module is **read-only on the hardware** — it only *reads* the sensor temperature
registers as an SMBus master. It never writes a sensor, fan, or configuration register, so it
cannot interfere with the ADM1030's autonomous fan-control loop that keeps your CPU cool.

**Why there's no fan-RPM readout:** the ADM1030 *can* report fan speed, but only after its
tachometer counter is switched on via a config-register write — and Apple's firmware leaves it
off. Enabling it (a single, documented "measurement-only" bit) audibly disturbed the fan on
the test machine and produced no usable reading, so **fan RPM was deliberately dropped** rather
than risk writing to the chip that protects the CPU. If a non-disruptive method is ever found,
it can be added; for now, correctness and safety win.

## How it works

Short version: find the Uni-N I²C controller in the Name Registry, memory-map its registers,
and bit-bang the KeyWest I²C protocol (ported from the Linux `drivers/macintosh` sources) to
read the DS1775 and ADM1030. The full story — sensor topology, the KeyWest register interface,
why the TAU route is a dead end, the Open Firmware recon, and the fan-tachometer finding — is
in [TECHNICAL.md](TECHNICAL.md), with the on-machine Open Firmware capture in
[`probe/OF-RECON.md`](probe/OF-RECON.md).

The [`probe/`](probe/) directory has a standalone diagnostic application (`CPUTempProbe`) that
prints the discovery + raw reading to a console window — useful for bringing this up on a
different machine.

## Acknowledgements

Developed by UnexpectedBomb, with engineering assistance from Claude (Anthropic). Sensor
topology and the KeyWest I²C register interface were cross-referenced against the Linux kernel
`drivers/macintosh` / `drivers/hwmon` sources and the DS1775 / ADM1030 datasheets.

## License

[MIT](LICENSE) © 2026 UnexpectedBomb.
