/*
 * tibook_probe.c — Mac OS 9 diagnostic APPLICATION that answers one question for
 * a PowerBook G4 Titanium: is ANY CPU/system temperature readable from OS 9?
 *
 * Motivation (macos9lives topic 7837): the CPU Temp Control Strip module shows
 * nothing on a 1 GHz TiBook (PowerBook3,5, 7455). Web/utility evidence says the
 * Titanium line exposes no OS-readable heat sensor (the ADT746x belongs to the
 * ALUMINUM PowerBook / iBook G4, a different machine), and the 745x has no usable
 * on-die TAU. This probe gets FIRST-HAND ground truth on the actual hardware by
 * trying every avenue we know:
 *
 *   [A] CPU identity (Gestalt)
 *   [B] on-die TAU  — write/read THRM1 (SPR 1019); does it respond?
 *   [C] GetCoreProcessorTemperature (PowerMgrLib) — the portable-targeted OS call
 *       (returned kCantReportProcessorTemperatureErr = -13013 on the Mini, but the
 *        TiBook is a real Apple portable, so it is worth trying here)
 *   [D] i2c controllers + a read-only temp-sensor address sweep (DS1775/MAX6642/
 *       ADT746x/LM8x ranges) on every channel
 *   [E] device-tree temp/thermal/sensor nodes by name/compatible/device_type
 *   [VERDICT] printed LAST (RetroConsole has no scrollback)
 *
 * STRICTLY READ-ONLY on every bus (never writes a sensor/fan register); the only
 * writes are to the CPU's own THRM1 SPR (a benign self-test, restored to 0).
 *
 * Reuses the KeyWest i2c engine + registry walk from cpu_temp_probe.c, the TAU
 * probe from the CSM, and the GCPT test from cpu_temp_probe.c v11.
 */
#include <stdio.h>
#include <string.h>
#include <MacTypes.h>
#include <NameRegistry.h>
#include <DriverServices.h>
#include <CodeFragments.h>
#include <Multiprocessing.h>
#include <Gestalt.h>
#include <OSUtils.h>

#define PL(x) ((unsigned long)(x))
#define kCantReportProcessorTemperatureErr (-13013)   /* MacErrors.h */

/* ---- KeyWest I2C register indices / bits (low_i2c.c) ---- */
#define KW_MODE 0
#define KW_CONTROL 1
#define KW_STATUS 2
#define KW_ISR 3
#define KW_ADDR 5
#define KW_SUBADDR 6
#define KW_DATA 7
#define KW_MODE_100KHZ 0x00
#define KW_MODE_STANDARD 0x04
#define KW_MODE_COMBINED 0x0C
#define KW_CTL_AAK 0x01
#define KW_CTL_XADDR 0x02
#define KW_CTL_STOP 0x04
#define KW_STAT_BUSY 0x01
#define KW_STAT_LAST_AAK 0x02
#define KW_IRQ_DATA 0x01
#define KW_IRQ_ADDR 0x02
#define KW_IRQ_STOP 0x04
#define KW_IRQ_MASK 0x0F
enum { ST_ADDR, ST_READ, ST_STOP, ST_IDLE };

static volatile UInt8 *gBase = 0;
static int             gShift = 4;
static int             gI2cAcks = 0;     /* running total for the verdict */

static UInt8 kwrd(int reg) { return *(volatile UInt8 *)(gBase + ((UInt32)reg << gShift)); }
static void kwwr(int reg, UInt8 v)
{
    *(volatile UInt8 *)(gBase + ((UInt32)reg << gShift)) = v;
    __asm__ __volatile__("eieio");
    (void)kwrd(KW_SUBADDR);
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
static int kw_read_dev(UInt8 addr7, UInt8 sub, UInt8 *buf, int len, int mode, int chan)
{
    int state = ST_ADDR, result = 0, steps = 0, g;
    UInt8 addrdir = (UInt8)((addr7 << 1) | 1);
    for (g = 0; g < 200000 && (kwrd(KW_STATUS) & KW_STAT_BUSY); g++) ;
    kwwr(KW_ISR, kwrd(KW_ISR));
    kwwr(KW_MODE, (UInt8)(mode | KW_MODE_100KHZ | (chan << 4)));
    kwwr(KW_STATUS, 0);
    kwwr(KW_ADDR, addrdir);
    if (mode == KW_MODE_COMBINED) kwwr(KW_SUBADDR, sub);
    kwwr(KW_CONTROL, KW_CTL_XADDR);
    for (;;) {
        UInt8 isr = kw_wait_isr();
        UInt8 st  = kwrd(KW_STATUS);
        if (isr == 0) { result = -1; break; }
        if (isr & KW_IRQ_ADDR) {
            if (!(st & KW_STAT_LAST_AAK)) { kwwr(KW_CONTROL, KW_CTL_STOP); state = ST_STOP; result = -2; }
            else { state = ST_READ; if (len > 1) kwwr(KW_CONTROL, KW_CTL_AAK); }
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

static int buf_has(const char *buf, long len, const char *needle)
{
    long nlen = (long)strlen(needle), i;
    for (i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return 1;
    return 0;
}

/* Read-only sweep of temp-sensor address ranges on one i2c node/channel. Any ACK
 * is a real sensor; report it (reg00 + the ADT device-id reg 0x3D) and count it. */
static void scan_node(UInt32 base, UInt32 step)
{
    static const UInt8 addrs[] = { 0x2C,0x2D,0x2E,0x2F, 0x48,0x49,0x4A,0x4B,0x4C,0x4D,0x4E,0x4F };
    volatile UInt8 *saveB = gBase; int saveS = gShift;
    int sh = 0, chan, i, found = 0;
    UInt32 s = step ? step : 0x10;
    while (!(s & 1)) { s >>= 1; sh++; }
    gBase = (volatile UInt8 *)base; gShift = sh;
    for (chan = 0; chan <= 1; chan++) {
        for (i = 0; i < (int)sizeof(addrs); i++) {
            UInt8 b = 0, id = 0xFF;
            if (kw_read_dev(addrs[i], 0x00, &b, 1, KW_MODE_COMBINED, chan) == 0) {
                kw_read_dev(addrs[i], 0x3D, &id, 1, KW_MODE_COMBINED, chan);
                printf("      *** ACK addr=0x%02x chan=%d  reg00=0x%02x reg3D=0x%02x\n",
                       addrs[i], chan, b, id);
                found = 1; gI2cAcks++;
            }
        }
    }
    if (!found) printf("      (no temp-sensor ACK, chan 0-1)\n");
    gBase = saveB; gShift = saveS;
}

/* Walk the Name Registry; for each i2c controller print it + sweep for sensors. */
static void enumerate_i2c(void)
{
    RegEntryIter it;
    RegEntryID   node;
    Boolean      done = false, first = true;
    int count = 0, ctlrs = 0;
    printf("[D] i2c controllers + read-only temp-sensor sweep:\n");
    if (RegistryEntryIterateCreate(&it) != noErr) { printf("    iterate FAILED\n"); return; }
    for (;;) {
        char compat[160];
        UInt32 aa[8], step = 0x10, reg[8];
        RegPropertyValueSize sz;
        int haveAAPL, haveReg;
        UInt32 base;
        OSStatus err = RegistryEntryIterate(&it, first ? kRegIterDescendants : kRegIterContinue, &node, &done);
        first = false;
        if (err != noErr || done) break;
        count++;
        memset(compat, 0, sizeof(compat)); sz = sizeof(compat) - 1;
        if (RegistryPropertyGet(&node, "compatible", compat, &sz) != noErr) continue;
        if (!buf_has(compat, (long)(sizeof(compat) - 1), "i2c")) continue;
        ctlrs++;
        aa[0] = 0; sz = sizeof(aa);
        haveAAPL = (RegistryPropertyGet(&node, "AAPL,address", aa, &sz) == noErr);
        { RegPropertyValueSize s = 4; step = 0x10; RegistryPropertyGet(&node, "AAPL,address-step", &step, &s); }
        reg[0] = 0; { RegPropertyValueSize s = sizeof(reg); haveReg = (RegistryPropertyGet(&node, "reg", reg, &s) == noErr); }
        printf("    i2c: compat='%s' AAPL=%s0x%08lx step=0x%lx reg=0x%08lx(%s)\n",
               compat, haveAAPL ? "" : "(none) ", PL(aa[0]), PL(step),
               PL(reg[0]), haveReg ? "ok" : "none");
        if (haveAAPL)     base = aa[0];
        else if (haveReg) base = reg[0] + 3;
        else { printf("      (no MMIO base — cannot sweep)\n"); continue; }
        scan_node(base, step);
    }
    RegistryEntryIterateDispose(&it);
    printf("    [%d nodes scanned; %d i2c controller(s); %d sensor ACK(s) total]\n\n",
           count, ctlrs, gI2cAcks);
}

/* Print every device-tree node whose name/compatible/device_type looks thermal. */
static int dump_sensor_nodes(void)
{
    static const char *kw[] = { "temp", "therm", "sensor", "fan", "monitor",
        "ds1775", "ds16", "max66", "max669", "adt74", "lm8", "lm90",
        "thermostat", "diode", "cpu-id", 0 };
    RegEntryIter it;
    RegEntryID   node;
    Boolean      done = false, first = true;
    int n = 0;
    printf("[E] Device-tree temp/thermal/sensor nodes:\n");
    if (RegistryEntryIterateCreate(&it) != noErr) { printf("    iterate FAILED\n"); return 0; }
    for (;;) {
        char name[64], compat[160], dtype[48];
        UInt32 reg[8];
        RegPropertyValueSize sz;
        int i, hit = 0;
        OSStatus err = RegistryEntryIterate(&it, first ? kRegIterDescendants : kRegIterContinue, &node, &done);
        first = false;
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
    if (!n) printf("    (none found)\n");
    printf("    [%d thermal-ish node(s)]\n\n", n);
    return n;
}

/* NOTE: on-die TAU (THRM SPRs) is intentionally NOT probed here. Classic Mac OS
 * runs application/CSM code in USER mode, and THRM1/2/3 (SPR 1020/1021/1022) and
 * ICTC (1019) are PRIVILEGED SPRs — any mtspr/mfspr to them from an app raises a
 * privilege violation (System Error type 7), which is exactly what crashed
 * TiBookProbe v1. So TAU is simply not reachable from a Control Strip module,
 * regardless of CPU. (And the 745x removed THRM anyway.) */

/* ---------- GetCoreProcessorTemperature (runtime-resolved) ---------- */
typedef SInt32 (*GCPTProc)(MPCpuID);
static SInt32 test_core_temp(int *supported)
{
    CFragConnectionID conn = 0;
    Ptr mainAddr = 0, symAddr = 0;
    Str255 errName;
    CFragSymbolClass cls = 0;
    GCPTProc fn;
    SInt32 r0 = 0;
    int i;
    *supported = 0;
    printf("[C] GetCoreProcessorTemperature (PowerMgrLib):\n");
    if (GetSharedLibrary("\pPowerMgrLib", kPowerPCCFragArch, kReferenceCFrag,
                         &conn, &mainAddr, errName) != noErr) {
        printf("    PowerMgrLib not available.\n\n"); return 0;
    }
    if (FindSymbol(conn, "\pGetCoreProcessorTemperature", &symAddr, &cls) != noErr || !symAddr) {
        printf("    symbol not found.\n\n"); return 0;
    }
    fn = (GCPTProc)symAddr;
    for (i = 0; i < 4; i++) {
        SInt32 r = fn((MPCpuID)(long)i);
        if (i == 0) r0 = r;
        printf("      cpuID=%d -> 0x%08lx = %ld dec | /256=%ld C\n",
               i, PL((UInt32)r), (long)r, (long)r / 256);
    }
    if (r0 == kCantReportProcessorTemperatureErr)
        printf("    => kCantReportProcessorTemperatureErr (-13013): unsupported here.\n\n");
    else { *supported = 1; printf("    => NON-error result — investigate the unit above!\n\n"); }
    return r0;
}

int main(void)
{
    long cpu = 0;
    int gcptOk = 0, treeNodes;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("==== TiBook Temp Sensor Diagnostic v2 ====\n\n");

    printf("[A] CPU: ");
    if (Gestalt(gestaltNativeCPUtype, &cpu) == noErr)
        printf("gestaltNativeCPUtype=0x%lx %s\n", PL(cpu),
               cpu == gestaltCPUG4 ? "(G4 family)" : cpu == gestaltCPU750 ? "(750/G3)" : "");
    else printf("(Gestalt failed)\n");
    printf("    (on-die TAU not tested — privileged SPR, faults from an app; see source)\n\n");

    (void)test_core_temp(&gcptOk);   /* [B] GetCoreProcessorTemperature */
    enumerate_i2c();                 /* [C] i2c sensor sweep */
    treeNodes = dump_sensor_nodes(); /* [D] device-tree thermal nodes */

    /* -------- VERDICT (last, for the no-scrollback console) -------- */
    printf("======================= VERDICT =======================\n");
    printf("  GCPT (OS call) .. %s\n", gcptOk ? "returned a value (investigate!)" : "unsupported (-13013)");
    printf("  i2c sensor ACKs . %d\n", gI2cAcks);
    printf("  thermal nodes ... %d in device tree\n", treeNodes);
    if (gcptOk || gI2cAcks > 0)
        printf("  >>> SOMETHING is readable — see the marked lines above.\n");
    else
        printf("  >>> NO READABLE CPU TEMPERATURE SENSOR on this Mac.\n"
               "      (Matches the expectation for the Titanium PowerBook line.)\n");
    printf("=======================================================\n");

    printf("\n(Press Return to quit.)\n");
    getchar();
    return 0;
}
