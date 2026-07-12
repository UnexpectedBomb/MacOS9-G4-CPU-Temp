# Technical notes

How this module reads real temperatures on a Power Mac G4 MDD (PowerMac3,6) under Mac OS 9,
and why it works where the older approach doesn't.

## Why not the on-chip TAU?

The PowerPC 744x/745x ("G4") has a Thermal Assist Unit — an on-die thermal sensor read through
special-purpose registers. It's what *Jeremy's CSM Bundle* temperature module uses. But
Motorola's errata disabled/deprecated the TAU on the MPC7450/7451/7455 family (the MDD's CPU),
and where it does respond it is wildly inaccurate (Motorola documents ±12 °C; in practice it
reads far low). So the TAU is a dead end on these machines. We read the board's dedicated
sensors instead.

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
