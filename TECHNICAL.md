# Technical notes

How this module reads real temperatures on a Power Mac G4 MDD (PowerMac3,6) under Mac OS 9,
and why it works where the older approach doesn't.

## Why not the on-chip TAU?

The PowerPC 744x/745x ("G4") has a Thermal Assist Unit — an on-die thermal sensor read through
special-purpose registers (THRM1/2/3). It's what *Jeremy's CSM Bundle* temperature module uses.
There are two independent reasons it is not an option here:

1. **It can't be read from a Control Strip module at all.** THRM1/2/3 (and ICTC) are *privileged*
   SPRs, and Mac OS 9 runs application / Control-Strip code in **user mode** (problem state). Any
   `mfspr`/`mtspr` to them raises a privilege violation (System Error type 7) that takes the whole
   Control Strip down. Reading the TAU would require a native driver running in supervisor mode,
   not an `sdev`.
2. **Even if it could, it's a dead end on these Macs.** Motorola disabled/deprecated the TAU on
   the MPC7450/7451/7455 family (the MDD's CPU), and where it does respond it is wildly inaccurate
   (±12 °C on paper, far low in practice).

So we read the board's dedicated I²C sensors instead. (Earlier builds carried a gated TAU
fallback; it was removed once the user-mode fault above was confirmed on a Titanium PowerBook.)

## Sensor topology

The MDD carries two temperature sensors on the **Uni-N I²C bus** (the I²C cell inside the
Uni-N/U2 memory controller, an implementation Apple/Linux call "KeyWest"):

| Sensor  | Part    | 7-bit addr | Measures            |
|---------|---------|-----------|---------------------|
| CPU     | DS1775  | `0x49`    | CPU temperature     |
| Case    | ADM1030 | `0x2c`    | Case/ambient + fan  |

This matches Linux's `drivers/macintosh/therm_windtunnel.c`, which is gated on
`of_machine_is_compatible("PowerMac3,6")` and binds the DS1775 as the CPU sensor and the
ADM1030 as the case sensor + fan controller.

The addresses and the I²C controller were confirmed on the actual machine at the Open Firmware
prompt — see [`probe/OF-RECON.md`](probe/OF-RECON.md). Note the DS1775 is strapped to `0x49`
(not the part's nominal `0x48`); OF reports the child nodes with 8-bit unit addresses
(`temp-monitor@92` = `0x92` = `0x49`<<1, `fan@58` = `0x58` = `0x2c`<<1).

## Finding and mapping the controller

Mac OS 9 exposes the Open Firmware device tree through the **Name Registry**. The module:

1. Iterates the registry (`RegistryEntryIterateCreate` / `RegistryEntryIterate`) looking for a
   node whose `compatible` property contains `uni-n-i2c`.
   - **Gotcha:** pass `kRegIterDescendants` on the *first* `RegistryEntryIterate` call and
     `kRegIterContinue` on every call after. Passing `kRegIterDescendants` each time re-descends
     from the just-returned node and dead-ends after a few nodes. (`RegistryEntrySearch` hides
     this because it loops internally.)
2. Reads the node's `AAPL,address` property — a pre-mapped **logical** address you can
   dereference directly (Apple's *Designing PCI Cards and Drivers* documents this) — and
   `AAPL,address-step` (0x10 here). On this machine `AAPL,address` = `0xf8001003`.
3. Register *n* is then a byte at `base + (n << log2(step))`. If `AAPL,address` were absent,
   the physical `reg` base + the observed +3 byte-lane offset is the fallback.

Classic Mac OS has no memory protection, so a normal application (or Control Strip module) can
read this memory-mapped I/O directly — no driver required.

## The KeyWest I²C register interface

Eight byte-wide registers, ported from Linux `arch/powerpc/platforms/powermac/low_i2c.c`:

```
mode(0) control(1) status(2) isr(3) ier(4) addr(5) subaddr(6) data(7)
```

- **mode**: `STANDARD 0x04`, `STANDARDSUB 0x08`, `COMBINED 0x0C`; bus speed in the low bits,
  channel in the high nibble.
- **control**: `AAK 0x01`, `XADDR 0x02` (kick off a framed transfer), `STOP 0x04`, `START 0x08`.
- **status**: `BUSY 0x01`, `LAST_AAK 0x02` (last byte was ACK'd).
- **isr**: `DATA 0x01`, `ADDR 0x02`, `STOP 0x04`, `START 0x08`.

A read is driven as a small polled state machine — no interrupt handler needed. For a register
read we use **COMBINED** mode (write the sub-address/pointer, repeated-start, read the data):

```
write isr := isr                     ; clear stale flags
write mode := COMBINED | speed | chan
write addr := (dev<<1)|1             ; read address
write subaddr := register
write control := XADDR               ; go
loop:
  isr = poll(isr)
  if ADDR: if !LAST_AAK -> NAK (no device); else start reading (set AAK if >1 byte left)
  if DATA: read data; on the 2nd-to-last byte clear AAK to NAK the last
  if STOP: done
  write isr := isr                   ; ack serviced flags
```

- **DS1775** temperature: pointer register `0`, two bytes, big-endian, signed two's-complement
  8.8 fixed-point (MSB = whole °C, MSB of the low byte = 0.5 °C). e.g. `0x2780` = 39.5 °C.
- **ADM1030** case temperature: register `0x0A`, one signed byte, 1 °C/LSB.

## The fan-tachometer finding (why there's no RPM readout)

The ADM1030 measures fan speed, but the tachometer counter must be enabled via CONF2 (`0x01`)
bit `0x04`, and Apple's firmware leaves it off (its automatic fan control is temperature-based
and never reads the tach back). The Linux `adm1031` driver documents that bit as
measurement-only.

On the test machine, enabling it (a read-modify-write that set only `0x04`, preserving all
other bits, never touching the CONF1 fan-control register) **audibly disturbed the fan** — it
began pulsing/hunting — and still produced no valid count. Whether that's an ADM1030-specific
quirk (PWM-driven fans need the drive gated to read a clean tach) or something else, the
conclusion was the same: reading fan RPM requires writing to the controller that keeps the CPU
alive, and it reacted badly. So fan RPM was dropped and the module is strictly read-only. The
change is reversible (the config register is volatile — a full power-off restores firmware
defaults), which is how the test machine was recovered.

## Control Strip module notes

- It's an `'sdev'` code resource: a native PowerPC PEF wrapped in a Mixed Mode routine
  descriptor (`ProcInfo 0x00003FF0` for `pascal long f(long, long, Rect*, GrafPtr)`).
- A Control-Strip-loaded PEF does **not** get its writable-data section instantiated, so
  mutable fragment globals crash the whole Control Strip. All per-instance state lives in a
  heap block returned from `sdevInitModule` as the refCon and handed back as `params`.
- The sensor read runs on the periodic tickle, throttled to ~2 s via `TickCount`.

## Universal backends (auto-detection)

At load, after finding a memory-mapped I²C controller, `detect_backend()` probes for a sensor,
first match wins (a probe "succeeds" when the device ACKs; an absent device just NAKs the address
phase, which is the normal "not fitted" result, not an error):

| Order | Backend | Address | Read |
|-------|---------|---------|------|
| 1 | DS1775 (+ ADM1030 case) | `0x49` / `0x2c` | reg 0 (2 bytes, 8.8) / reg `0x0A` (1 byte) |
| 2 | MAX6642 | `0x4A`* | remote (CPU) reg `0x01`, local reg `0x00`, 1 °C/LSB |
| 3 | ADT7460 / ADT7467 | `0x2E` | remote1 (CPU) `0x25`, local `0x26`; part from ID reg `0x3D` |
| 4 | none | — | display `n/a` |

The DS1775 path resolves its second ("case") reading lazily, one probe per tick, so a NAK is never
immediately followed by another i2c transaction (that back-to-back pattern wedged an early build
of the KeyWest engine). It first looks for an **ADM1030** case sensor on channel 0 (the MDD /
FW800); if none answers it tries a **second DS1775 on channel 1** (the 667 DVI Titanium carries one
there, reading ~41 °C, surfaced as the Case reading); if neither is present the label reads plain
*Sensor: DS1775* and the Case item is disabled. Once resolved, steady state issues no NAKs at all.
All backends are **read-only**; the alert + peak tracking run in the shared read path so they work
regardless of backend. The DS1775 path is hardware-verified (MDD, and both the case-via-ADM1030 and
case-via-channel-1-DS1775 variants on a 667 DVI Titanium); the MAX6642 and ADT paths are ported
from documented register maps (Linux `drivers/hwmon/adm1031.c`,
`drivers/macintosh/therm_adt746x.c`, the MAX6642 datasheet) and await
testers.

### There is no on-chip TAU backend

An earlier version had a TAU fallback. It was removed: reading the THRM SPRs needs a *privileged*
instruction, and OS 9 runs Control-Strip code in **user mode**, so `mfspr/mtspr` on THRM1 raises a
privilege violation (System Error type 7) that crashes the whole Control Strip — on *any* CPU, not
just the 745x that lack the registers. (This was mis-diagnosed at first as an illegal-instruction
fault gated by CPU type; a Titanium PowerBook test showed it faults regardless, because the fault
is the privilege level, not the opcode.) Reading the TAU would require a supervisor-mode driver,
and it is inaccurate anyway (see the top of this document), so the board's I²C sensors are used
exclusively.

## Why the Mac Mini G4 is out of reach from OS 9

The Mini G4 (PowerMac10,1) *does* have a MAX6642 — but the device tree shows its `temp-monitor`
node is a child of **`pmu-i2c`**, a bus with no `AAPL,address` (not memory-mapped). It's behind
the **PMU**. Confirmed on hardware: the MAX6642 never ACKs on either memory-mapped KeyWest bus
(all channels, both byte-lane bases), and `pmu-i2c`'s only readable path is via PMU commands.

Reading it means i2c-over-PMU (Linux does this via `via-pmu`: `PMU_I2C_CMD` + a
`{bus, mode, address, sub_addr, count, …}` header, send-then-poll). The protocol is known — but
**OS 9 provides no way to send raw PMU commands** on New-World machines: `Power.h` exposes only
the high-level Power Manager (battery/sleep/CPU-speed), and the old `PMgrOp` raw-command path was
Old-World-PowerBook-only. And since the Mini never officially ran OS 9, there's no Apple OS 9
thermal code to borrow. So the Mini's sensor is genuinely unreadable from OS 9 at runtime.

**Open Firmware can read it, though** — OF ships its own `pmu-i2c` driver. The `temp-monitor`
node exposes `.temp` / `read-reg`, so a boot-time spot-check works (see the README). This is how
the Mini's readings were confirmed: `.temp` → `Loc Temp 53 / Rem Temp 46` (°C), matching
`read-reg` 1 (remote/CPU) = `0x2e` = 46 and reg 0 (local) = `0x35` = 53.

\* Note the doc-supplied MAX6642 address (`0x4A`, KeyWest) did not match real hardware; on the
Mini the device tree reports the MAX6642 on the PMU bus at a different address entirely — another
reason to run `CPUTempProbe` on any new machine before trusting a table.
