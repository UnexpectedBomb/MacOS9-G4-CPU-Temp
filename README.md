# Mac OS 9 — G4 CPU & Case Temperature (Control Strip module)

A **Control Strip module that shows live CPU (and, where available, case) temperature on Power
Mac G3/G4 machines under Mac OS 9.** It auto-detects the machine's temperature sensor at load,
reads it straight from the hardware, and sits in the Control Strip like a stock module — silent,
loading at startup.

It is fully developed and verified on the **Power Mac G4 "Mirrored Drive Doors" (PowerMac3,6)**,
where it reads the **DS1775** CPU sensor and **ADM1030** case sensor over the Uni-N / "KeyWest"
I²C bus. As far as I can tell this is the **first accurate CPU-temperature readout for Mac OS 9
on the 7455-based Power Macs**: the one prior tool for classic Mac OS (the temperature module in
*Jeremy's CSM Bundle*) reads the PowerPC's on-chip Thermal Assist Unit (TAU), which Motorola
disabled on the 7450/7455 family — so it can't read these machines at all. This module ignores
the TAU and talks to the actual sensor chips Apple's own firmware uses, giving a real reading to
**0.5 °C** where the TAU spec was only ±12 °C.

Beyond the MDD it also **auto-detects other sensors** — and it has since been confirmed on a
**PowerBook G4 Titanium**, which carries its own DS1775. See *Supported machines*.

> **Status: working, seeking testers.** Verified on a dual-1.25 GHz MDD (PowerMac3,6) and a
> 667 MHz PowerBook G4 Titanium, both running Mac OS 9.2.2. The other sensor backends are
> implemented from documented register maps but are **not yet hardware-verified** — testing on
> other models is how we build confidence. The module is **strictly read-only** on all sensors
> (see *Safety*), and if it finds no readable sensor it simply shows `n/a` — it never hangs or
> crashes the Control Strip.

![The CPU Temp module in the Mac OS 9 Control Strip, showing the 47°C reading and its menu — with the auto-detected "Sensor: DS1775 + ADM1030" header at the top](docs/screenshot-menu.jpg)

## Features

- **Auto-detects the sensor** at load and shows which one it found — an italic, informational
  line at the top of the menu (e.g. *Sensor: DS1775 + ADM1030*), in the style of the AirPort CSM.
- **Live reading** — the temperature refreshes on its own every couple of seconds, no clicking
  needed (the cell repaints itself in place as the value changes).
- **CPU temperature** — plus **case/board temperature** where the machine has a second channel.
- **Click for a menu** — pick the reading (CPU / Case) and the units (°F / °C) as two
  independent groups; units apply to both. The menu opens on mouse-down, like other modules.
- **Remembers your settings** — the °C/°F and CPU/Case choices persist across restarts.
- **Turns red** near the safe limit (CPU ≥ 75 °C; the case channel has its own threshold).
- **Over-temperature alert** — if the CPU exceeds the safe limit (≥ 85 °C) a Notification
  Manager alert pops up ("consider shutting down…"). It's latched (warns once per event, not in
  a loop) and keeps watching the CPU on every backend, even while you're viewing case temp.
- **Fixed-width cell**, bold text, and a custom Control-Strip-style icon.
- **Behaves like a stock module** — it's built as a native PowerPC code fragment, so it shows a
  version in Get Info and you can option-drag a copy of it out of the Control Strip, just like the
  built-in modules.

Thresholds are conservative defaults (the 7455's junction limit is ~105 °C; the MDD idles around
40–55 °C). They're `#define`s at the top of [`csm/cpu_temp_csm.c`](csm/cpu_temp_csm.c) if you
want to change them.

## Supported machines

Sensor access under OS 9 depends entirely on *how* a machine exposes its sensor. The list below
grows as people report results — **if you run the module on a model that isn't listed, please
[open an issue](../../issues) or post on the [macos9lives.com thread](https://macos9lives.com/smforum/index.php?topic=7837.0)
with the result** (a run of `CPUTempProbe` / the TiBook probe is ideal) and it will be added.

### ✅ Confirmed working on hardware

| Model | Model ID | CPU | Sensor(s) detected | Readings |
|-------|----------|-----|--------------------|----------|
| Power Mac G4 "Mirrored Drive Doors" / FW800 | PowerMac3,6 | 7455 | DS1775 (CPU) + ADM1030 (case), Uni-N I²C | CPU + case |
| PowerBook G4 Titanium, 667 MHz "DVI" | PowerBook3,4 | 7455 | Two DS1775 at `0x49` (ch 0 + ch 1), Uni-N I²C; no ADM1030 | CPU + a 2nd sensor |

The Titanium result is notable: the Titanium PowerBooks are widely believed to expose *no*
OS-readable temperature sensor (even Mac OS X's Temperature Monitor shows none), but the 667 DVI
carries **two** DS1775 sensors on the Uni-N bus — one per I²C channel. The module shows the
channel-0 sensor as the CPU and the channel-1 sensor as the "Case" reading (its exact location on
the board is unconfirmed). Note this varies within the line: see the 1 GHz below.

### ⏳ Implemented, seeking testers

Auto-detected from documented register maps, not yet confirmed on hardware:

- Any Uni-N / KeyWest G4 carrying a **DS1775 / MAX6642 / ADT746x** on a *memory-mapped* I²C bus
  (this likely includes several other G4 desktops and the iBook G4 / aluminum PowerBook G4, if
  their sensor is memory-mapped rather than PMU-side).

### ❌ Detected but NOT readable from OS 9

- **Mac Mini G4 (PowerMac10,1).** It *has* a **MAX6642**, but the sensor lives on the **PMU's**
  private I²C bus, not a memory-mapped one. Reading it means sending i2c-over-PMU commands, and
  classic Mac OS exposes no way to do that on these New-World machines. Confirmed dead-end (the
  `GetCoreProcessorTemperature` OS call also returns "can't report" here). The module safely shows
  `n/a`; read the temperature another way (below).
- **PowerBook G4 Titanium, 1 GHz (PowerBook3,5).** Unlike the 667 DVI, this model (the first
  Titanium with an active fan) has **no I²C temperature sensor** on the Uni-N bus (a community
  probe found zero sensors on both KeyWest controllers). Cooling is handled by an on-board
  **Cypress PSoC** fan controller (a `fan` node of type `Psoc` in the device tree) that senses
  temperature internally and never exposes it as a readable sensor. The module shows `n/a`; use
  Open Firmware or Mac OS X.
- **Power Mac G3 Blue & White (PowerMac1,1) — and pre-Uni-N G3s generally.** These have **no
  temperature sensor at all** that the OS can see: no memory-mapped I²C controller, and no thermal
  node anywhere in the device tree (a probe on a 350 MHz B&W scanned 98 nodes and found zero of
  each). It's a Grackle / Heathrow machine that predates the Uni-N/KeyWest I²C the module scans for.
  Not "trapped behind the PMU" like the Mini — simply absent. The module shows `n/a`.

Not sure what your machine has? Run a probe — they print the device-tree sensor nodes and I²C
controllers and end with a verdict: [`CPUTempProbe`](probe/) (the original), the
[TiBook probe](probe/tibook/), or the general [B&W / G3 probe](probe/bw/) (works on any G3/G4).

### Reading temperature on the Mac Mini G4 (and other PMU-sensor machines)

OS 9 can't reach the PMU sensor at runtime, so use one of these:

- **Mac OS X** — Apple's drivers read the same MAX6642 live (*Temperature Monitor*, iStat, …).
  This is the right tool for watching temperature under load.
- **Open Firmware spot-check** — OF has its own PMU I²C driver, so it can read the sensor on
  demand. Boot holding **⌘-⌥-O-F** and run (paths shown for the Mac Mini G4 / PowerMac10,1):

  ```forth
  " /pci@f2000000/mac-io@17/via-pmu@16000/pmu-i2c/temp-monitor@190" open-dev value tmon
  " .temp" tmon $call-method
  tmon close-dev
  ```

  It prints `Loc Temp` (board) and `Rem Temp` (CPU die) in °C. Find the exact path for your
  machine with `dev / ls` (the node is `temp-monitor` under `pmu-i2c`). Note this reflects the
  temperature at OF time — essentially idle after a reboot — so it's a health spot-check, not a
  load monitor.

## Install

Two prebuilt downloads are in [`dist/`](dist/). **Use the `.hqx` if you have any trouble** —
it's plain 7-bit text and survives any download/transfer intact.

- **`dist/CPUTempCSM.hqx`** (BinHex — recommended) — decode with StuffIt Expander.
- **`dist/CPUTempCSM.bin`** (MacBinary II) — decode with StuffIt Expander / DropStuff.

Steps:

1. On GitHub, get the file with the **"Download raw file"** button (the download icon on the
   file's page) — *don't* copy-paste it or "Save As" the web page, which corrupts binary data.
2. Get it onto the Mac and **decode it there** (dropping it on **StuffIt Expander** works for
   both `.hqx` and `.bin`) → a file named `CPUTempCSM`. Decode on the classic Mac, or transfer
   in a way that preserves resource forks — the module's content lives in its resource fork.
3. Drop `CPUTempCSM` into **System Folder ▸ Control Strip Modules**.
4. **Restart.** The temperature tile appears in the Control Strip. Click it to switch reading
   or units.

> **Note on the `.bin`:** it is re-encoded with Apple's `macbinary` tool so its CRC validates
> in strict decoders (The Unarchiver, current StuffIt). A MacBinary produced directly by the
> Retro68 build tools has a CRC those tools reject — if you build your own, run
> [`csm/scripts/package-dist.sh`](csm/scripts/package-dist.sh) to produce clean `.bin`/`.hqx`.

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

Short version: find the Uni-N/KeyWest I²C controller in the Name Registry, memory-map its
registers, and bit-bang the KeyWest I²C protocol (ported from the Linux `drivers/macintosh`
sources) to read the sensor. At load a small probe picks the backend — DS1775, MAX6642, or
ADT746x on the I²C bus. The full story — sensor topology, the KeyWest register interface, why the
on-chip TAU is not an option under OS 9, the Open Firmware recon, the fan-tachometer finding, and
why the Mac Mini's PMU-side sensor is out of reach — is in
[TECHNICAL.md](TECHNICAL.md), with the on-machine Open Firmware capture in
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
