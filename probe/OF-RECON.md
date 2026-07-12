# Open Firmware recon — G4 MDD (PowerMac3,6) I²C temperature sensors

Goal: confirm on THIS machine the four facts the OS 9 I²C driver needs before any code is
written. All steps are read-only inspection of the device tree — **safe**. Do NOT run any
`setenv`, `nvedit`, `nvstore`, `reset-all`, or `blvar` commands.

## What we're trying to capture (the deliverables of this pass)

1. **The I²C controller node's OF path** (expected `/uni-n@f8000000/i2c@f8001000`, but confirm).
2. **`AAPL,address`** on that node — the pre-mapped 32-bit LOGICAL base of the register block
   (this is the pointer OS 9 will deref via RegistryCStrEntryLookup + this property).
3. **`AAPL,address-step`** — register offset scaling (expected `0x10` → bsteps = 4, i.e.
   `reg_addr = base + (index << 4)`). If absent, we compute from `reg` + parent `ranges`.
4. **The two sensor child nodes + their `reg` (I²C address)**: DS1775 (CPU temp, 7-bit 0x48)
   and ADM1030 (case/fan, 7-bit 0x2c). Confirm whether `reg` stores the 7-bit addr (0x48/0x2c)
   or the 8-bit write addr (0x90/0x58 = 7-bit<<1) — the driver's addr register needs (7bit<<1)|rw.

OF has no scrollback and no easy disk logging — **photograph each screen** or copy the hex by hand.

---

## Step 0 — enter Open Firmware
Boot the MDD holding **Cmd-Opt-O-F**. You get a `0 >` prompt. (`bye` or `mac-boot` at the end
boots OS 9 normally.)

## Step 1 — locate the Uni-N node and its I²C child
```
dev /uni-n
ls
```
Look for a child like `i2c@f8001000` (name may be `i2c` or `i2c-bus`). Note its full unit name.
If `/uni-n` doesn't exist, run `dev /` then `ls` and scan the top level for `uni-n`/`u3`/`u2`.

## Step 2 — dump the I²C controller node's properties (THE key step)
```
dev /uni-n/i2c
.properties
```
Record these lines:
- `AAPL,address`        → e.g. `f5001000`  ← **logical register base (capture the FIRST 4 bytes)**
- `AAPL,address-step`   → e.g. `00000010`  ← **register spacing (capture verbatim)**
- `reg`                 → physical offset+size within uni-n (fallback if AAPL,address absent)
- `#address-cells`      → tells us how `reg` on the children is encoded
- `compatible` / `name` / `device_type`
- `interrupts` (informational — we're polling, so we don't need it, but record it)

If `AAPL,address` is NOT present here, that's important — tell me; we fall back to
`reg` + the parent's `ranges` to compute the physical address and map it.

## Step 3 — optional: list the node's Forth methods (may reveal a safe OF read path)
```
dev /uni-n/i2c
words
```
If you see methods like `read-i2c` / `write-i2c` / `do-transaction` / `setaddress`, note them —
some Apple OF builds can do a live I²C read from the prompt, which would let us confirm a real
temperature value before writing ANY OS 9 code. If nothing I²C-ish shows, skip — not required.

## Step 4 — enumerate the sensor child nodes
```
dev /uni-n/i2c
ls
```
Expect two children (names vary by ROM): something like `ds1775@48` + `adm1030@2c`, or
`MAC,ds1775` + `MAC,adm1030`, or generic `temp-monitor` + `fan`. For EACH child:
```
dev /uni-n/i2c/<child-name>
.properties
```
Record per child:
- node `name`
- `reg`         → the I²C address. **0x48 vs 0x90 for the DS1775 tells us the shift convention.**
- `compatible`  → e.g. `ds1775`, `adm1030` (confirms the part)
- `device_type` → e.g. `temperature-sensor`, `temp-monitor`, `fanctrl`

## Step 5 — (nice-to-have) capture the whole subtree in one shot
```
dev /uni-n/i2c
ls
```
then photograph. Also a top-level `dev /` + `ls` photo is useful context.

## Step 6 — boot normally
```
mac-boot
```
(or `bye`).

---

## RESULTS — captured on hardware 2026-07-11 (user's MDD, PowerMac3,6)
| # | Item | Value |
|---|------|-------|
| 1 | I²C controller OF path | `/uni-n/i2c@f8001000`, compatible `keywest-i2c, uni-n-i2c` |
| 2 | `AAPL,address` | `f8001003` (logical base; +3 = big-endian byte-lane offset, use verbatim) |
| 3 | `AAPL,address-step` | `0x10` → reg access = AAPL,address + (regIndex << 4) |
| 4 | DS1775 node + `reg` | `temp-monitor@92`, reg `0x92` = 8-bit → **7-bit 0x49**, compatible `ds1775` |
| 5 | ADM1030 node + `reg` | `fan@58`, reg `0x58` → 7-bit 0x2c, compatible `adm1030` (company-id 0x41, device-id 0x30) |
| 6 | DS1775 `compatible` | `ds1775` ✓ (device_type also ds1775) |
| 7 | I²C read methods? | Parent i2c: `read-i2c`/`read-i2c-at`/`read-i2c-at2`/`write-i2c`. temp-monitor: `read-temp`/`read-reg`/`write-reg`/`read-config`/limits |
| — | reg base + size | `reg = f8001000 00001000` (phys base 0xf8001000, size 0x1000); i2c-rate 100kHz; #address-cells 1 |

Other i2c children: `i2c-hwclock@ca` (RTC, 7-bit 0x65), `cereal` — both irrelevant.

## ★ LIVE TEMPERATURE READ — SUCCEEDED
```forth
" /uni-n/i2c/temp-monitor@92" open-dev value tmon    \ ihandle = ffbc5b00
" read-temp" tmon $call-method .                     \ → 2780
tmon close-dev
```
`0x2780` = MSB 0x27 (39 °C) + LSB 0x80 (0.5 °C) = **39.5 °C**. Real DS1775 reading — full chain
proven. (OF gotcha: `dev`+`open` gives "no current instance"; must `open-dev` + `$call-method`.)

## Consequences for the OS 9 driver
- Slave: 7-bit **0x49** → addr byte `(0x49<<1)|rw` = `0x92` write / `0x93` read.
- Read DS1775 register **0** (pointer defaults to 0), **2 bytes**, decode MSB.LSB as 8.8 fixed-point °C.
- Register block: RegistryPropertyGet `AAPL,address` (=0xf8001003) + `AAPL,address-step` (=0x10);
  reg N byte at `AAPL,address + (N << 4)`. Port the polled StandardSub/combined transaction from
  Linux `low_i2c.c`. Bus confirmed safe for read-only master access (ADM1030 fan loop is autonomous).

With (2) and (3) we finalize the register-address macro; with (4)/(5) we set the slave
addresses and confirm the 7-bit-vs-8-bit convention. Then we scaffold the polled read against
`low_i2c.c`.
