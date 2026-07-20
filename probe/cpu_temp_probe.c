/*
 * cpu_temp_probe.c — Mac OS 9 diagnostic APPLICATION that reads the G4 MDD
 * (PowerMac3,6) CPU temperature from the DS1775 over the Uni-N/KeyWest I2C bus,
 * entirely from user space. Classic Mac OS has no memory protection, so an app
 * can do Name Registry + MMIO directly (same technique as the EHCI probe).
 *
 * On-hardware Open Firmware recon (2026-07-11):
 *   - I2C controller node: name "i2c", compatible "keywest-i2c","uni-n-i2c"
 *   - AAPL,address = 0xf8001003 (logical reg base, +3 byte-lane baked in)
 *   - AAPL,address-step = 0x10  (reg N byte at base + (N << 4))
 *   - reg = f8001000 00001000   (phys base + size; fallback base = reg+3)
 *   - DS1775 CPU sensor: 7-bit addr 0x49 (node temp-monitor@92), register 0
 *   - OF `read-temp` returned 0x2780 = 39.5 C  <-- our target to match
 *
 * KeyWest register protocol ported from Linux
 *   arch/powerpc/platforms/powermac/low_i2c.c (kw_i2c_* state machine).
 *
 * v2: discovery now matches the "compatible" property (v1 matched "name" and
 *     found nothing) and dumps every i2c-ish node + a node count for diagnosis.
 *
 * Read-only SMBus master (never writes a sensor/fan register) -> cannot disturb
 * the ADM1030's autonomous fan loop.
 */
#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <NameRegistry.h>
#include <DriverServices.h>
#include <OSUtils.h>

#define PL(x) ((unsigned long)(x))

/* ---- KeyWest I2C register indices (low_i2c.c reg_t) ---- */
#define KW_MODE     0
#define KW_CONTROL  1
#define KW_STATUS   2
#define KW_ISR      3
#define KW_IER      4
#define KW_ADDR     5
#define KW_SUBADDR  6
#define KW_DATA     7

#define KW_MODE_100KHZ      0x00
#define KW_MODE_STANDARD    0x04
#define KW_MODE_STANDARDSUB 0x08
#define KW_MODE_COMBINED    0x0C
#define KW_CTL_AAK          0x01
#define KW_CTL_XADDR        0x02
#define KW_CTL_STOP         0x04
#define KW_CTL_START        0x08
#define KW_STAT_BUSY        0x01
#define KW_STAT_LAST_AAK    0x02
#define KW_IRQ_DATA         0x01
#define KW_IRQ_ADDR         0x02
#define KW_IRQ_STOP         0x04
#define KW_IRQ_START        0x08
#define KW_IRQ_MASK         0x0F

enum { ST_IDLE, ST_ADDR, ST_READ, ST_WRITE, ST_STOP, ST_DEAD };

static volatile UInt8 *gBase = 0;
static int             gShift = 4;

static UInt8 kwrd(int reg)
{
    return *(volatile UInt8 *)(gBase + ((UInt32)reg << gShift));
}
static void kwwr(int reg, UInt8 v)
{
    *(volatile UInt8 *)(gBase + ((UInt32)reg << gShift)) = v;
    __asm__ __volatile__("eieio");
    (void)kwrd(KW_SUBADDR);          /* write-post flush (mirrors low_i2c) */
}

static UInt8 kw_wait_isr(void)
{
    int i, j;
    for (i = 0; i < 3000; i++) {
        UInt8 isr = kwrd(KW_ISR) & KW_IRQ_MASK;
        if (isr) return isr;
        for (j = 0; j < 20000; j++) __asm__ __volatile__("nop");
    }
    return 0;
}

/* Polled port of the low_i2c.c kw_i2c state machine. 0 ok, negative on error. */
static int kw_read_dev(UInt8 addr7, UInt8 sub, UInt8 *buf, int len,
                       int mode, int chan, int verbose)
{
    int state = ST_ADDR, result = 0, steps = 0, g;
    UInt8 addrdir = (UInt8)((addr7 << 1) | 1);

    for (g = 0; g < 200000 && (kwrd(KW_STATUS) & KW_STAT_BUSY); g++) ;

    kwwr(KW_ISR, kwrd(KW_ISR));
    kwwr(KW_MODE, (UInt8)(mode | KW_MODE_100KHZ | (chan << 4)));
    kwwr(KW_STATUS, 0);
    kwwr(KW_ADDR, addrdir);
    if (mode == KW_MODE_COMBINED || mode == KW_MODE_STANDARDSUB)
        kwwr(KW_SUBADDR, sub);
    kwwr(KW_CONTROL, KW_CTL_XADDR);

    for (;;) {
        UInt8 isr = kw_wait_isr();
        UInt8 st  = kwrd(KW_STATUS);
        if (verbose) printf("      isr=0x%02x status=0x%02x state=%d len=%d\n",
                            isr, st, state, len);
        if (isr == 0) { result = -1; break; }

        if (isr & KW_IRQ_ADDR) {
            if (!(st & KW_STAT_LAST_AAK)) {
                kwwr(KW_CONTROL, KW_CTL_STOP);
                state = ST_STOP; result = -2;
            } else {
                state = ST_READ;
                if (len > 1) kwwr(KW_CONTROL, KW_CTL_AAK);
            }
        }
        if (isr & KW_IRQ_DATA) {
            if (state == ST_READ) {
                if (len > 0) { *buf++ = kwrd(KW_DATA); len--; }
                if (len == 1) kwwr(KW_CONTROL, 0);
                else if (len == 0) state = ST_STOP;
            }
        }
        if (isr & KW_IRQ_STOP) { kwwr(KW_ISR, isr); state = ST_IDLE; break; }

        kwwr(KW_ISR, isr);
        if (++steps > 64) { result = -4; break; }
    }
    if (result == 0 && len != 0) result = -5;
    return result;
}

static void decode_and_print(UInt8 b0, UInt8 b1)
{
    SInt16 raw = (SInt16)(((UInt16)b0 << 8) | b1);
    long x10  = ((long)raw * 10) / 256;
    long whole = x10 / 10, frac = x10 % 10;
    if (frac < 0) frac = -frac;
    printf("  raw=0x%02x%02x  ->  %ld.%ld C\n", b0, b1, whole, frac);
}

static int buf_has(const char *buf, long len, const char *needle)
{
    long nlen = (long)strlen(needle), i;
    for (i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return 1;
    return 0;
}

/* Targeted scan of temperature-sensor address ranges on ONE i2c node, so a new
 * machine's sensor is found even if it sits on a non-preferred node or channel.
 * Read-only. Ranges: 0x2C-0x2F (ADM1030/ADT746x family), 0x48-0x4F (DS1775 /
 * MAX6642 / LM8x family). For each ACK it also reports reg 0x01 and the ADT
 * device-ID reg 0x3D, which help identify the part. */
static void scan_node(UInt32 base, UInt32 step)
{
    static const UInt8 addrs[] = { 0x2C,0x2D,0x2E,0x2F,
                                   0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F };
    volatile UInt8 *saveB = gBase; int saveS = gShift;
    int sh = 0, chan, i, found = 0;
    UInt32 s = step ? step : 0x10;
    while (!(s & 1)) { s >>= 1; sh++; }
    gBase = (volatile UInt8 *)base; gShift = sh;
    for (chan = 0; chan <= 1; chan++) {
        for (i = 0; i < (int)sizeof(addrs); i++) {
            UInt8 b = 0, r1 = 0xFF, id = 0xFF;
            if (kw_read_dev(addrs[i], 0x00, &b, 1, KW_MODE_COMBINED, chan, 0) == 0) {
                kw_read_dev(addrs[i], 0x01, &r1, 1, KW_MODE_COMBINED, chan, 0);
                kw_read_dev(addrs[i], 0x3D, &id, 1, KW_MODE_COMBINED, chan, 0);
                printf("      ACK addr=0x%02x chan=%d  reg00=0x%02x reg01=0x%02x reg3D=0x%02x\n",
                       addrs[i], chan, b, r1, id);
                found = 1;
            }
        }
    }
    if (!found) printf("      (no temp-sensor ACK on this node, chan 0-1)\n");
    gBase = saveB; gShift = saveS;
}

/*
 * Enumerate the whole Name Registry; find the Uni-N i2c controller by its
 * "compatible" property. Prints every i2c-ish node it sees plus a node count
 * (so if iteration or the tree is unexpected, we see it). Returns 1 if a usable
 * register base was determined; fills base/step. `*viaAAPL` says whether the
 * base came from AAPL,address (1) or the reg-property fallback (0).
 */
static int find_i2c(UInt32 *baseOut, UInt32 *stepOut, int *viaAAPL)
{
    RegEntryIter it;
    RegEntryID   node;
    Boolean      done = false;
    OSStatus     err;
    int count = 0, nAA = 0, nCompat = 0, bestPri = 0, first = 1;
    UInt32 bestBase = 0, bestStep = 0x10; int bestAAPL = 0;

    if (RegistryEntryIterateCreate(&it) != noErr) { printf("    iterate-create FAILED\n"); return 0; }

    for (;;) {
        char compat[256];
        UInt32 aa[8], reg[8], st = 0;
        RegPropertyValueSize sz;
        int haveAAPL, haveReg, isUniN, isI2c, pri;
        UInt32 base;

        /* First call establishes the descendants walk from root; subsequent
         * calls MUST use kRegIterContinue (0x01) to advance it — passing
         * kRegIterDescendants every time re-descends from the current node and
         * dead-ends after a few nodes (the v2 "scanned 3 nodes" bug). */
        err = RegistryEntryIterate(&it, first ? kRegIterDescendants : kRegIterContinue,
                                   &node, &done);
        first = 0;
        if (err != noErr) { printf("    iterate err=%ld after %d nodes\n", (long)err, count); break; }
        if (done) break;
        count++;

        memset(compat, 0, sizeof(compat));
        sz = sizeof(compat) - 1;
        if (RegistryPropertyGet(&node, "compatible", compat, &sz) != noErr) continue;
        nCompat++;
        if (!buf_has(compat, (long)(sizeof(compat) - 1), "i2c")) continue;
        isI2c = 1;

        /* this is an i2c-ish node — gather everything */
        aa[0] = 0; sz = sizeof(aa);
        haveAAPL = (RegistryPropertyGet(&node, "AAPL,address", aa, &sz) == noErr);
        if (haveAAPL) nAA++;
        st = 0; { RegPropertyValueSize s = 4; RegistryPropertyGet(&node, "AAPL,address-step", &st, &s); }
        reg[0] = reg[1] = 0; { RegPropertyValueSize s = sizeof(reg); haveReg = (RegistryPropertyGet(&node, "reg", reg, &s) == noErr); }
        isUniN = buf_has(compat, (long)(sizeof(compat) - 1), "uni-n-i2c");

        printf("    i2c node: compat0='%s' uni-n=%d\n", compat, isUniN);
        printf("      AAPL=%s0x%08lx step=0x%lx  reg=0x%08lx(%s)\n",
               haveAAPL ? "" : "(none)", PL(aa[0]), PL(st ? st : 0x10),
               PL(reg[0]), haveReg ? "ok" : "none");

        /* choose a base + priority: AAPL beats reg; uni-n beats other */
        if (haveAAPL)      { base = aa[0]; }
        else if (haveReg)  { base = reg[0] + 3; }   /* +3 = observed byte lane */
        else               { continue; }
        scan_node(base, st ? st : 0x10);            /* report any temp sensor on this node */
        pri = (haveAAPL ? 2 : 1) + (isUniN ? 2 : 0); /* uni-n+AAPL=4 best */
        if (pri > bestPri) {
            bestPri = pri; bestBase = base; bestStep = st ? st : 0x10; bestAAPL = haveAAPL;
        }
        (void)isI2c;
    }
    RegistryEntryIterateDispose(&it);
    printf("    [scanned %d nodes; %d had 'compatible'; %d i2c had AAPL,address]\n",
           count, nCompat, nAA);

    if (bestPri == 0) return 0;
    *baseOut = bestBase; *stepOut = bestStep; *viaAAPL = bestAAPL;
    return 1;
}

/* Print every device-tree node that looks like a temperature/thermal sensor, from
 * its name/compatible/device_type — the authoritative way to learn a machine's
 * sensor part + address + bus (a small `reg` value = an i2c child address). This
 * surfaces sensors on buses we can't MMIO-scan (e.g. pmu-i2c). */
static void dump_sensor_nodes(void)
{
    static const char *kw[] = { "temp", "therm", "sensor", "fan", "monitor",
        "ds1775", "ds16", "max66", "max669", "adt74", "lm8", "lm90",
        "cpu-id", "adc", "thermostat", "diode", 0 };
    RegEntryIter it;
    RegEntryID   node;
    Boolean      done = false, first = true;
    int n = 0;

    printf("[S] Device-tree sensor/thermal nodes:\n");
    if (RegistryEntryIterateCreate(&it) != noErr) { printf("    iterate FAILED\n"); return; }
    for (;;) {
        char name[64], compat[160], dtype[48];
        UInt32 reg[8];
        RegPropertyValueSize sz;
        int i, hit = 0;
        OSStatus err = RegistryEntryIterate(&it, first ? kRegIterDescendants : kRegIterContinue, &node, &done);
        first = 0;
        if (err != noErr || done) break;

        memset(name, 0, sizeof(name));   sz = sizeof(name) - 1;   RegistryPropertyGet(&node, "name", name, &sz);
        memset(compat, 0, sizeof(compat)); sz = sizeof(compat) - 1; RegistryPropertyGet(&node, "compatible", compat, &sz);
        memset(dtype, 0, sizeof(dtype));  sz = sizeof(dtype) - 1;  RegistryPropertyGet(&node, "device_type", dtype, &sz);
        for (i = 0; kw[i]; i++)
            if (buf_has(name, (long)sizeof(name), kw[i])
                || buf_has(compat, (long)sizeof(compat), kw[i])
                || buf_has(dtype, (long)sizeof(dtype), kw[i])) { hit = 1; break; }
        if (!hit) continue;

        reg[0] = reg[1] = 0; sz = sizeof(reg); RegistryPropertyGet(&node, "reg", reg, &sz);
        printf("    '%s' compat='%s' type='%s' reg=0x%lx,0x%lx\n",
               name, compat, dtype, PL(reg[0]), PL(reg[1]));
        n++;
    }
    RegistryEntryIterateDispose(&it);
    printf("    [%d sensor/thermal node(s) found]\n\n", n);
}

/* List the immediate children of one node (name/compatible/reg) — maps a sensor
 * to its parent i2c controller. Uses a separate iterator so the caller's walk is
 * undisturbed. */
static void list_children(RegEntryID *parent)
{
    RegEntryIter ci;
    RegEntryID   ch;
    Boolean      d = false;
    int          n = 0;
    Boolean cfirst = true;
    if (RegistryEntryIterateCreate(&ci) != noErr) return;
    if (RegistryEntryIterateSet(&ci, parent) != noErr) { RegistryEntryIterateDispose(&ci); return; }
    for (;;) {
        char nm[64], cp[128];
        UInt32 reg[8];
        RegPropertyValueSize sz;
        /* kRegIterChildren on the FIRST call, kRegIterContinue after — else it
         * only ever returns the first child (same gotcha as kRegIterDescendants). */
        OSStatus e = RegistryEntryIterate(&ci, cfirst ? kRegIterChildren : kRegIterContinue, &ch, &d);
        cfirst = false;
        if (e != noErr || d) break;
        memset(nm, 0, sizeof(nm)); sz = sizeof(nm) - 1; RegistryPropertyGet(&ch, "name", nm, &sz);
        memset(cp, 0, sizeof(cp)); sz = sizeof(cp) - 1; RegistryPropertyGet(&ch, "compatible", cp, &sz);
        reg[0] = 0; sz = sizeof(reg); RegistryPropertyGet(&ch, "reg", reg, &sz);
        printf("        child '%s' compat='%s' reg=0x%lx\n", nm, cp, PL(reg[0]));
        n++;
    }
    RegistryEntryIterateDispose(&ci);
    if (!n) printf("        (no children enumerated)\n");
}

/* Brute-force the MAX6642 at 7-bit 0x48: try this controller's base both as-is
 * and with the +3 big-endian byte-lane forced, across channels 0-7. On any ACK,
 * print the local (reg 0x00) and remote/CPU (reg 0x01) temperatures. */
static void sweep_max6642(UInt32 aapl, UInt32 step)
{
    volatile UInt8 *saveB = gBase; int saveS = gShift;
    UInt32 bases[2]; int nb = 1, bi, ch, sh = 0;
    UInt32 s = step ? step : 0x10;
    while (!(s & 1)) { s >>= 1; sh++; }
    gShift = sh;
    bases[0] = aapl;
    if ((aapl & 3) != 3) { bases[1] = (aapl & ~3UL) | 3UL; nb = 2; }   /* also try +3 lane */
    for (bi = 0; bi < nb; bi++) {
        gBase = (volatile UInt8 *)bases[bi];
        for (ch = 0; ch <= 7; ch++) {
            UInt8 lo = 0, rem = 0;
            if (kw_read_dev(0x48, 0x00, &lo, 1, KW_MODE_COMBINED, ch, 0) == 0) {
                kw_read_dev(0x48, 0x01, &rem, 1, KW_MODE_COMBINED, ch, 0);
                printf("      *** MAX6642 ACK  base=0x%08lx chan=%d  local=0x%02x (%d C)  remote/CPU=0x%02x (%d C)\n",
                       PL(bases[bi]), ch, lo, (int)lo, rem, (int)rem);
            }
        }
    }
    gBase = saveB; gShift = saveS;
}

/* For each memory-mapped i2c controller: print it, list its children (to find
 * which one hosts the MAX6642), and brute-force the 0x48 read. */
static void diagnose_controllers(void)
{
    RegEntryIter it;
    RegEntryID   node;
    Boolean      done = false, first = true;
    printf("[C] i2c controllers — children + MAX6642(0x48) read sweep:\n");
    if (RegistryEntryIterateCreate(&it) != noErr) { printf("    iterate FAILED\n"); return; }
    for (;;) {
        char compat[160];
        UInt32 aa[8], step = 0x10;
        RegPropertyValueSize sz;
        OSStatus err = RegistryEntryIterate(&it, first ? kRegIterDescendants : kRegIterContinue, &node, &done);
        first = 0;
        if (err != noErr || done) break;
        int haveAAPL;
        memset(compat, 0, sizeof(compat)); sz = sizeof(compat) - 1;
        if (RegistryPropertyGet(&node, "compatible", compat, &sz) != noErr) continue;
        if (!buf_has(compat, (long)(sizeof(compat) - 1), "i2c")) continue;
        aa[0] = 0; sz = sizeof(aa);
        haveAAPL = (RegistryPropertyGet(&node, "AAPL,address", aa, &sz) == noErr);
        { RegPropertyValueSize s = 4; step = 0x10; RegistryPropertyGet(&node, "AAPL,address-step", &step, &s); }
        printf("    i2c node compat='%s' AAPL=%s0x%08lx step=0x%lx\n",
               compat, haveAAPL ? "" : "(none) ", PL(aa[0]), PL(step));
        list_children(&node);                       /* list children of EVERY i2c node (incl. pmu-i2c) */
        if (haveAAPL) sweep_max6642(aa[0], step);    /* sweep only the memory-mapped ones */
    }
    RegistryEntryIterateDispose(&it);
    printf("\n");
}

int main(void)
{
    UInt32 base = 0, step = 0x10;
    UInt8 buf[2];
    int shift, mi, ch, i, ok = -99, usedMode = -1, usedChan = -1, viaAAPL = 0;
    int modes[2]; const char *mn[2];

    modes[0] = KW_MODE_COMBINED; mn[0] = "COMBINED";
    modes[1] = KW_MODE_STANDARD; mn[1] = "STANDARD";

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==== G3/G4 Temp Probe v7 (full child enum + MAX6642 sweep) ====\n\n");

    dump_sensor_nodes();
    diagnose_controllers();

    printf("[1] Scanning Name Registry for the i2c controller (by compatible)...\n");
    if (!find_i2c(&base, &step, &viaAAPL)) {
        printf("    No usable i2c controller found. Stopping.\n");
        goto done;
    }

    shift = 0; { UInt32 s = step ? step : 0x10; while (!(s & 1)) { s >>= 1; shift++; } }
    gBase = (volatile UInt8 *)base;
    gShift = shift;

    printf("\n[2] Using base=0x%08lx step=0x%lx (shift=%d) via %s\n",
           PL(base), PL(step), shift, viaAAPL ? "AAPL,address" : "reg+3 fallback");
    printf("    reg addrs: mode=0x%08lx ctl=0x%08lx stat=0x%08lx isr=0x%08lx addr=0x%08lx data=0x%08lx\n",
           PL((UInt32)(gBase + (KW_MODE    << shift))),
           PL((UInt32)(gBase + (KW_CONTROL << shift))),
           PL((UInt32)(gBase + (KW_STATUS  << shift))),
           PL((UInt32)(gBase + (KW_ISR     << shift))),
           PL((UInt32)(gBase + (KW_ADDR    << shift))),
           PL((UInt32)(gBase + (KW_DATA    << shift))));
    printf("    target DS1775 7-bit 0x49, register 0 (OF read-temp = 0x2780 = 39.5 C)\n\n");

    printf("[3] Reading temperature (auto-scan mode/channel, verbose)...\n");
    for (mi = 0; mi < 2 && ok != 0; mi++) {
        for (ch = 0; ch < 4 && ok != 0; ch++) {
            printf("  attempt mode=%s chan=%d:\n", mn[mi], ch);
            buf[0] = buf[1] = 0;
            ok = kw_read_dev(0x49, 0x00, buf, 2, modes[mi], ch, 1);
            printf("    result=%d bytes=0x%02x 0x%02x\n", ok, buf[0], buf[1]);
            if (ok == 0) { usedMode = mi; usedChan = ch; decode_and_print(buf[0], buf[1]); }
        }
    }
    if (ok != 0) { printf("\n[!] All attempts failed (see traces).\n"); goto done; }

    printf("\n[4] SUCCESS via %s chan=%d.  Live readings (~1s apart):\n", mn[usedMode], usedChan);
    for (i = 0; i < 8; i++) {
        unsigned long t;
        buf[0] = buf[1] = 0;
        if (kw_read_dev(0x49, 0x00, buf, 2, modes[usedMode], usedChan, 0) == 0)
            decode_and_print(buf[0], buf[1]);
        else
            printf("  read failed\n");
        Delay(60, &t);
    }

done:
    printf("\n==== done ====\n(Press Return to quit.)\n");
    getchar();
    return 0;
}
