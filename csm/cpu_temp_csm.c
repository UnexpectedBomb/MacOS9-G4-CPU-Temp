/*
 * Temp Monitor — a universal G3/G4 temperature Control Strip module ('sdev').
 *
 * Shows live CPU (and, where available, case/board) temperature under Mac OS 9,
 * auto-detecting the machine's sensor at load. Backends:
 *   - DS1775 (+ ADM1030) (Power Mac G4 MDD / FW800, PowerBook G4 Titanium) CPU 0x49
 *   - MAX6642            (Mac Mini G4)                                      0x4A
 *   - ADT7460 / ADT7467  (iBook G4, PowerBook G4 Al)                        0x2E
 *
 * (There is no PowerPC-TAU backend: reading the on-chip THRM SPRs needs a
 * privileged instruction, and OS 9 runs Control-Strip code in USER mode, so any
 * mtspr/mfspr to THRM faults with a privilege violation. It is unreachable from
 * an 'sdev', and the 745x dropped THRM anyway.)
 *
 * A click pops a menu: an italic, disabled "Sensor: ..." label (informational,
 * like the AirPort CSM), then two independent checkmark groups — WHAT to show
 * (CPU / Case) and the UNITS (F / C).
 *
 * STRICTLY READ-ONLY on every sensor/fan controller — we never write a sensor,
 * fan, or configuration register. (Fan RPM was tried on the MDD via the ADM1030
 * tach-enable write; it disturbed the fan and was reverted. The same hard "no"
 * applies to the ADT fan/PWM registers.)
 *
 * The over-temperature alert monitors the CPU on ALL backends, regardless of
 * which reading is on screen.
 *
 * TESTED: DS1775 backend on a dual-1.25GHz MDD (PowerMac3,6) and a 667 MHz
 * PowerBook G4 Titanium (DS1775 at 0x49, no ADM1030). The MAX6642 and ADT
 * backends are implemented from documented register maps but are NOT yet
 * hardware-verified; use CPUTempProbe / the TiBook probe to confirm on a new
 * machine.
 *
 * Entry ABI (Apple patent US6493002): pascal long main(long msg, long params,
 * Rect*, GrafPtr). All per-instance state lives in the refCon (never fragment
 * globals — a Control-Strip PEF has no instantiated writable-data section).
 */

#include <ControlStrip.h>
#include <NameRegistry.h>
#include <Notification.h>
#include <Quickdraw.h>
#include <Fonts.h>
#include <Menus.h>
#include <Gestalt.h>
#include <MacMemory.h>
#include <MacTypes.h>
#include <string.h>

extern pascal Ptr NewPtrSys(long byteCount);
extern pascal void DisposePtr(Ptr p);
extern pascal void NumToString(long theNum, StringPtr theString);
extern pascal unsigned long TickCount(void);

/* ---- thresholds (deg C) — overridable via -D for test builds ---- */
#ifndef MAX_SAFE_C
#define MAX_SAFE_C   85     /* CPU over-temp alert at/above this  */
#endif
#ifndef WARN_C
#define WARN_C       75     /* CPU red text at/above this         */
#endif
#ifndef CASE_WARN_C
#define CASE_WARN_C  55     /* case red text at/above this        */
#endif

#define DS1775_ADDR   0x49
#define ADM1030_ADDR  0x2c
#define ADM_REG_TEMP  0x0A  /* ADM1030 local (case) temperature   */
#define MAX6642_ADDR  0x4A
#define ADT746X_ADDR  0x2E
#define READ_TICKS    120   /* ~2 s between refreshes             */

/* display modes */
#define M_CPU  0
#define M_CASE 1
#define M_MAX  M_CASE

/* popup menu item numbers (item 1 = sensor label, 2 = divider, 5 = divider) */
#define kMenuID   200
#define kIt_Sensor 1
#define kIt_CPU   3
#define kIt_Case  4
#define kIt_Fahr  6
#define kIt_Cels  7

/* ---- KeyWest I2C ---- */
#define KW_MODE 0
#define KW_CONTROL 1
#define KW_STATUS 2
#define KW_ISR 3
#define KW_ADDR 5
#define KW_SUBADDR 6
#define KW_DATA 7
#define KW_MODE_100KHZ 0x00
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
enum { ST_ADDR, ST_READ, ST_STOP };

#define kCellPad 6
#define kArrowW  6
#define kSettingsMagic 0x00005400L

typedef enum {
    kBackendNone    = 0,
    kBackendDS1775  = 1,   /* MDD / FW800: DS1775 + ADM1030 */
    kBackendMAX6642 = 2,   /* Mac Mini G4 */
    kBackendADT7460 = 3,   /* iBook G4, PowerBook G4 Al */
    kBackendADT7467 = 4
} TempBackend;

/* Where the DS1775 backend gets its second ("case") reading. Resolved lazily,
 * one probe per tick, so a NAK is never immediately followed by another i2c
 * transaction (that back-to-back pattern is what broke the KeyWest bus in v1.1). */
typedef enum {
    kCaseUnknown   = 0,   /* not yet probed (memset default) */
    kCaseTryCh1    = 1,   /* ADM1030 absent; try a 2nd DS1775 on channel 1 next tick */
    kCaseADM       = 2,   /* ADM1030 at 0x2c, channel 0 (MDD / FW800) */
    kCaseDS1775Ch1 = 3,   /* a 2nd DS1775 at 0x49, channel 1 (PowerBook G4 Titanium) */
    kCaseNone      = 4    /* no second sensor on this machine */
} CaseSource;

typedef struct {
    volatile UInt8 *base;
    int    shift, chan;
    Boolean valid;
    TempBackend backend;
    int    caseSource;         /* CaseSource: how the DS1775 backend finds a 2nd reading */
    int    dispMode;           /* M_CPU / M_CASE */
    Boolean useF;
    Boolean alerted;
    Boolean haveCpu, haveCase;
    SInt16 cpuX10, caseX10;    /* tenths of a degree C */
    SInt16 cpuPeakX10;
    SInt16 shownVal;
    Boolean shownRed;
    int    shownMode;
    UInt32 lastRead;
    GrafPtr cachePort;         /* strip port + our cell rect, cached from the last  */
    Rect    cacheRect;         /* sdevDrawStatus, so the periodic tickle can repaint */
    Boolean haveCache;         /* itself in place (the strip issues no redraw on its */
                               /* own for a fixed-width module whose value changes).  */
    NMRec  nm;
    Str255 nmMsg;
} TempState;

/* ---------- KeyWest I2C (read-only; base/shift passed in — no globals) ---------- */

static UInt8 kwrd(volatile UInt8 *b, int s, int r)
{ return *(volatile UInt8 *)(b + ((UInt32)r << s)); }
static void kwwr(volatile UInt8 *b, int s, int r, UInt8 v)
{ *(volatile UInt8 *)(b + ((UInt32)r << s)) = v; __asm__ __volatile__("eieio"); (void)kwrd(b, s, KW_SUBADDR); }

static UInt8 kw_wait_isr(volatile UInt8 *b, int s)
{
    int i, j;
    for (i = 0; i < 3000; i++) {
        UInt8 isr = kwrd(b, s, KW_ISR) & KW_IRQ_MASK;
        if (isr) return isr;
        for (j = 0; j < 20000; j++) __asm__ __volatile__("nop");
    }
    return 0;
}

/* Combined-mode read of `len` bytes from register `sub` of 7-bit `addr7`.
 * Returns 0 ok, negative on error (-2 = address NAK = device absent). */
static int kw_read(volatile UInt8 *b, int s, UInt8 addr7, UInt8 sub, UInt8 *buf, int len, int chan)
{
    int state = ST_ADDR, result = 0, steps = 0, g;
    UInt8 addrdir = (UInt8)((addr7 << 1) | 1);

    for (g = 0; g < 200000 && (kwrd(b, s, KW_STATUS) & KW_STAT_BUSY); g++) ;
    kwwr(b, s, KW_ISR, kwrd(b, s, KW_ISR));
    kwwr(b, s, KW_MODE, (UInt8)(KW_MODE_COMBINED | KW_MODE_100KHZ | (chan << 4)));
    kwwr(b, s, KW_STATUS, 0);
    kwwr(b, s, KW_ADDR, addrdir);
    kwwr(b, s, KW_SUBADDR, sub);
    kwwr(b, s, KW_CONTROL, KW_CTL_XADDR);

    for (;;) {
        UInt8 isr = kw_wait_isr(b, s);
        UInt8 stt = kwrd(b, s, KW_STATUS);
        if (isr == 0) { result = -1; break; }
        if (isr & KW_IRQ_ADDR) {
            if (!(stt & KW_STAT_LAST_AAK)) { kwwr(b, s, KW_CONTROL, KW_CTL_STOP); state = ST_STOP; result = -2; }
            else { state = ST_READ; if (len > 1) kwwr(b, s, KW_CONTROL, KW_CTL_AAK); }
        }
        if (isr & KW_IRQ_DATA) {
            if (state == ST_READ) {
                if (len > 0) { *buf++ = kwrd(b, s, KW_DATA); len--; }
                if (len == 1) kwwr(b, s, KW_CONTROL, 0);
                else if (len == 0) state = ST_STOP;
            }
        }
        if (isr & KW_IRQ_STOP) { kwwr(b, s, KW_ISR, isr); break; }
        kwwr(b, s, KW_ISR, isr);
        if (++steps > 64) { result = -4; break; }
    }
    if (result == 0 && len != 0) result = -5;
    return result;
}

/* ---------- I2C controller discovery ---------- */

static int buf_has(const char *buf, long len, const char *needle)
{
    long nlen = (long)strlen(needle), i;
    for (i = 0; i + nlen <= len; i++)
        if (memcmp(buf + i, needle, nlen) == 0) return 1;
    return 0;
}

static Boolean find_i2c(TempState *st)
{
    RegEntryIter it;
    RegEntryID   node;
    Boolean      done = false, first = true;
    UInt32 bestBase = 0, bestStep = 0x10;
    int bestPri = 0;

    if (RegistryEntryIterateCreate(&it) != noErr) return false;
    for (;;) {
        char compat[256];
        UInt32 aa[8], reg[8], step = 0;
        RegPropertyValueSize sz;
        int haveAAPL, haveReg, isUniN, pri;
        UInt32 base;
        OSStatus err = RegistryEntryIterate(&it, first ? kRegIterDescendants : kRegIterContinue, &node, &done);
        first = false;
        if (err != noErr || done) break;

        memset(compat, 0, sizeof(compat));
        sz = sizeof(compat) - 1;
        if (RegistryPropertyGet(&node, "compatible", compat, &sz) != noErr) continue;
        if (!buf_has(compat, (long)(sizeof(compat) - 1), "i2c")) continue;

        aa[0] = 0; sz = sizeof(aa);
        haveAAPL = (RegistryPropertyGet(&node, "AAPL,address", aa, &sz) == noErr);
        { RegPropertyValueSize s = 4; step = 0; RegistryPropertyGet(&node, "AAPL,address-step", &step, &s); }
        reg[0] = 0; { RegPropertyValueSize s = sizeof(reg); haveReg = (RegistryPropertyGet(&node, "reg", reg, &s) == noErr); }
        isUniN = buf_has(compat, (long)(sizeof(compat) - 1), "uni-n-i2c");

        if (haveAAPL) base = aa[0];
        else if (haveReg) base = reg[0] + 3;
        else continue;
        pri = (haveAAPL ? 2 : 1) + (isUniN ? 2 : 0);
        if (pri > bestPri) { bestPri = pri; bestBase = base; bestStep = step ? step : 0x10; }
        if (pri >= 4) break;
    }
    RegistryEntryIterateDispose(&it);
    if (bestPri == 0) return false;

    st->base = (volatile UInt8 *)bestBase;
    { int sh = 0; UInt32 s = bestStep ? bestStep : 0x10; while (!(s & 1)) { s >>= 1; sh++; } st->shift = sh; }
    st->chan = 0;
    return true;
}

/* ---------- backend detection (first match wins) ---------- */

static TempBackend detect_backend(TempState *st)
{
    UInt8 b, id;
    if (st->valid) {
        if (kw_read(st->base, st->shift, DS1775_ADDR, 0x00, &b, 1, st->chan) == 0)
            return kBackendDS1775;   /* haveADM is latched later, in ReadAll_MDD */
        if (kw_read(st->base, st->shift, MAX6642_ADDR, 0x01, &b, 1, st->chan) == 0)
            return kBackendMAX6642;
        if (kw_read(st->base, st->shift, ADT746X_ADDR, 0x26, &b, 1, st->chan) == 0) {
            if (kw_read(st->base, st->shift, ADT746X_ADDR, 0x3D, &id, 1, st->chan) == 0
                && (id == 0x68 || id == 0x6A))
                return kBackendADT7467;
            return kBackendADT7460;      /* 0x27, unknown, or ID read failed */
        }
    }
    return kBackendNone;   /* no readable sensor on this Mac */
}

/* ---------- per-backend reads (all populate cpuX10/haveCpu[/caseX10/haveCase]) ---------- */

/* Decode a DS1775 2-byte temperature register (reg 0) into tenths of deg C. */
static SInt16 ds1775_x10(const UInt8 *b)
{
    SInt16 raw = (SInt16)(((UInt16)b[0] << 8) | b[1]);
    return (SInt16)(((long)raw * 10) / 256);
}

static void ReadAll_MDD(TempState *st)
{
    UInt8 b[2];

    /* CPU: DS1775 at 0x49, channel 0. ALWAYS first in the tick, so it is never
     * immediately preceded by a NAK'd probe (which is what wedged v1.1). */
    if (kw_read(st->base, st->shift, DS1775_ADDR, 0x00, b, 2, st->chan) == 0) {
        st->cpuX10  = ds1775_x10(b);
        st->haveCpu = true;
    }

    /* Case / second reading. Resolve the source one probe per tick (never two
     * i2c transactions after a NAK within a tick), then read only that source in
     * steady state (no NAKs at all once resolved):
     *   - MDD / FW800: an ADM1030 on channel 0.
     *   - PowerBook G4 Titanium: a second DS1775 at 0x49 on channel 1.        */
    switch (st->caseSource) {
        case kCaseUnknown:                       /* tick: probe ADM1030 only */
            if (kw_read(st->base, st->shift, ADM1030_ADDR, ADM_REG_TEMP, b, 1, st->chan) == 0) {
                st->caseX10 = (SInt16)((SInt16)(SInt8)b[0] * 10);
                st->haveCase = true;
                st->caseSource = kCaseADM;
            } else {                             /* absent -> try ch1 NEXT tick (gap after NAK) */
                st->haveCase = false;
                st->caseSource = kCaseTryCh1;
            }
            break;
        case kCaseTryCh1:                        /* a 2nd DS1775 on channel 1? */
            if (kw_read(st->base, st->shift, DS1775_ADDR, 0x00, b, 2, 1) == 0) {
                st->caseX10 = ds1775_x10(b);
                st->haveCase = true;
                st->caseSource = kCaseDS1775Ch1;
            } else {
                st->haveCase = false;
                st->caseSource = kCaseNone;
            }
            break;
        case kCaseADM:
            if (kw_read(st->base, st->shift, ADM1030_ADDR, ADM_REG_TEMP, b, 1, st->chan) == 0) {
                st->caseX10 = (SInt16)((SInt16)(SInt8)b[0] * 10);
                st->haveCase = true;
            } else st->haveCase = false;
            break;
        case kCaseDS1775Ch1:
            if (kw_read(st->base, st->shift, DS1775_ADDR, 0x00, b, 2, 1) == 0) {
                st->caseX10 = ds1775_x10(b);
                st->haveCase = true;
            } else st->haveCase = false;
            break;
        default:                                 /* kCaseNone */
            st->haveCase = false;
            break;
    }
}

static void ReadAll_MAX6642(TempState *st)
{
    UInt8 b;
    if (kw_read(st->base, st->shift, MAX6642_ADDR, 0x01, &b, 1, st->chan) == 0) {  /* remote = CPU */
        st->cpuX10 = (SInt16)((SInt16)b * 10);                                     /* unsigned 1 C/LSB */
        st->haveCpu = true;
    }
    if (kw_read(st->base, st->shift, MAX6642_ADDR, 0x00, &b, 1, st->chan) == 0) {  /* local = board */
        st->caseX10 = (SInt16)((SInt16)b * 10);
        st->haveCase = true;
    } else st->haveCase = false;
}

static void ReadAll_ADT(TempState *st)
{
    UInt8 b;
    if (kw_read(st->base, st->shift, ADT746X_ADDR, 0x25, &b, 1, st->chan) == 0) {  /* remote 1 = CPU */
        st->cpuX10 = (SInt16)((SInt16)(SInt8)b * 10);                              /* signed 1 C/LSB */
        st->haveCpu = true;
    }
    if (kw_read(st->base, st->shift, ADT746X_ADDR, 0x26, &b, 1, st->chan) == 0) {  /* local = board */
        st->caseX10 = (SInt16)((SInt16)(SInt8)b * 10);
        st->haveCase = true;
    } else st->haveCase = false;
}

/* ---------- over-temp alert (backend-independent; monitors the CPU) ---------- */

static void FireAlert(TempState *st)
{
    memset(&st->nm, 0, sizeof(st->nm));
    st->nm.qType   = 8;                 /* nmType */
    st->nm.nmSound = (Handle)-1L;       /* system alert sound */
    st->nm.nmStr   = st->nmMsg;
    st->nm.nmResp  = (NMUPP)-1L;        /* auto-remove on dismiss */
    NMInstall(&st->nm);
}

static void CheckAlert(TempState *st)
{
    if (st->cpuX10 >= MAX_SAFE_C * 10) {
        if (!st->alerted) { st->alerted = true; FireAlert(st); }
    } else if (st->cpuX10 < WARN_C * 10) {
        st->alerted = false;
    }
}

static void ReadAll(TempState *st)
{
    if (!st->valid) return;
    switch (st->backend) {
        case kBackendDS1775:  ReadAll_MDD(st);     break;
        case kBackendMAX6642: ReadAll_MAX6642(st); break;
        case kBackendADT7460:
        case kBackendADT7467: ReadAll_ADT(st);     break;
        default: break;
    }
    if (st->haveCpu) {
        if (st->cpuPeakX10 == -30000 || st->cpuX10 > st->cpuPeakX10) st->cpuPeakX10 = st->cpuX10;
        CheckAlert(st);   /* runs on EVERY backend */
    }
}

/* ---------- display helpers ---------- */

static void CtoP(const char *c, StringPtr p)
{ int n = 0; while (c[n] && n < 255) { p[n + 1] = (unsigned char)c[n]; n++; } p[0] = (unsigned char)n; }

static Boolean HaveActive(TempState *st)
{ return (Boolean)(st->dispMode == M_CASE ? st->haveCase : st->haveCpu); }

static long DisplayValue(TempState *st)
{
    SInt16 x10 = (st->dispMode == M_CASE) ? st->caseX10 : st->cpuX10;
    if (st->useF) return ((long)x10 * 9 / 5 + 320 + 5) / 10;
    return (x10 + 5) / 10;
}

static Boolean DisplayRed(TempState *st)
{
    if (st->dispMode == M_CASE) return (Boolean)(st->haveCase && st->caseX10 >= CASE_WARN_C * 10);
    return (Boolean)(st->haveCpu && st->cpuX10 >= WARN_C * 10);
}

/* strip label, e.g. "47<deg>C" ("~47<deg>C" for an approximate TAU reading) */
static void BuildLabel(TempState *st, StringPtr out)
{
    Str255 num;
    short n, i, o = 0;
    if (!st || st->backend == kBackendNone) { CtoP("n/a", out); return; }
    if (!HaveActive(st)) { CtoP("--", out); return; }
    NumToString(DisplayValue(st), num);
    n = num[0];
    for (i = 1; i <= n; i++) out[++o] = num[i];
    out[++o] = 0xA1;                              /* degree symbol */
    out[++o] = (unsigned char)(st->useF ? 'F' : 'C');
    out[0] = (unsigned char)o;
}

static void SetupStripFont(GrafPtr port)
{
    short fontID = 0, fontSize = 0;
    if (port != NULL) SetPort(port);
    if (SBGetControlStripFontID(&fontID) == noErr) TextFont(fontID);
    if (SBGetControlStripFontSize(&fontSize) == noErr) TextSize(fontSize);
    TextFace(bold);
}

/* fixed cell width = widest reading ("888<deg>F") */
static short FixedWidth(TempState *st, GrafPtr port)
{
    Str255 s;
    short o = 0;
    (void)st;
    s[++o] = '8'; s[++o] = '8'; s[++o] = '8'; s[++o] = 0xA1; s[++o] = 'F';
    s[0] = (unsigned char)o;
    SetupStripFont(port);
    return (short)(StringWidth(s) + kArrowW + kCellPad);
}

static void DrawArrow(short x, short y)
{
    MoveTo(x, y);         Line(4, 0);
    MoveTo(x + 1, y + 1); Line(2, 0);
    MoveTo(x + 2, y + 2); Line(0, 0);
}

static void backend_name(TempState *st, StringPtr out)
{
    switch (st->backend) {
        case kBackendDS1775:
            if (st->caseSource == kCaseADM)            CtoP("Sensor: DS1775 + ADM1030", out);
            else if (st->caseSource == kCaseDS1775Ch1) CtoP("Sensor: DS1775 x2",        out);
            else                                       CtoP("Sensor: DS1775",           out);
            break;
        case kBackendMAX6642:  CtoP("Sensor: MAX6642",              out); break;
        case kBackendADT7460:  CtoP("Sensor: ADT7460",              out); break;
        case kBackendADT7467:  CtoP("Sensor: ADT7467",              out); break;
        default:               CtoP("Sensor: none found on this Mac", out); break;
    }
}

/* ---------- persistent settings (survives reboot) ----------
 * The Control Strip's own save-settings value is only kept for the session, so
 * C/F and CPU/Case choices were lost on restart. SBLoad/SavePreferences write a
 * tiny resource to the module's preferences file, which persists. Byte 0 = useF,
 * byte 1 = dispMode. */

static void LoadPrefs(TempState *st)
{
    Handle h = NULL;
    if (SBLoadPreferences("\pCPU Temp Settings", &h) == noErr
        && h != NULL && GetHandleSize(h) >= 2) {
        st->useF     = (Boolean)((*h)[0] & 1);
        st->dispMode = (int)((*h)[1] & 3);
        if (st->dispMode > M_MAX) st->dispMode = M_CPU;
    }
    if (h != NULL) DisposeHandle(h);
}

static void SavePrefs(TempState *st)
{
    Handle h = NewHandle(2);
    if (h != NULL) {
        (*h)[0] = (char)(st->useF ? 1 : 0);
        (*h)[1] = (char)(st->dispMode & 3);
        SBSavePreferences("\pCPU Temp Settings", h);
        DisposeHandle(h);
    }
}

/* ---------- selection popup ---------- */

static void DoMenu(TempState *st, const Rect *cell)
{
    MenuRef m = NewMenu(kMenuID, "\p");
    short sel;
    Str255 sname;
    if (m == NULL) return;

    AppendMenu(m, "\px");            /* item 1 — sensor label, text set below */
    AppendMenu(m, "\p(-");           /* item 2 — divider */
    AppendMenu(m, "\pCPU Temperature;Case Temperature;(-;Fahrenheit;Celsius");

    backend_name(st, sname);         /* SetMenuItemText bypasses metachar parsing */
    SetMenuItemText(m, kIt_Sensor, sname);
    SetItemStyle(m, kIt_Sensor, italic);   /* informational label (like the AirPort CSM) */
    DisableItem(m, kIt_Sensor);

    CheckItem(m, kIt_CPU,  (Boolean)(st->dispMode == M_CPU));
    CheckItem(m, kIt_Case, (Boolean)(st->dispMode == M_CASE));
    CheckItem(m, kIt_Fahr, (Boolean)st->useF);
    CheckItem(m, kIt_Cels, (Boolean)!st->useF);
    if (!st->haveCase) DisableItem(m, kIt_Case);   /* no second channel (e.g. TAU) */

    InsertMenu(m, -1);
    sel = SBTrackPopupMenu(cell, m);
    DeleteMenu(kMenuID);
    DisposeMenu(m);

    switch (sel) {
        case kIt_CPU:  st->dispMode = M_CPU;  break;
        case kIt_Case: st->dispMode = M_CASE; break;
        case kIt_Fahr: st->useF = true;       break;
        case kIt_Cels: st->useF = false;      break;
        default: return;
    }
    SavePrefs(st);          /* write immediately so a reboot keeps the choice */
    st->shownVal = -30000;
}

/* Draw the reading + arrow into rect `r` of the CURRENT port. `erase` TRUE when
 * we are repainting ourselves during the periodic tickle (the strip has not
 * cleared the cell for us, unlike a normal sdevDrawStatus); FALSE for the strip-
 * driven draw, where the cell is already clean. */
static void DrawCell(TempState *st, const Rect *r, Boolean erase)
{
    Str255 s;
    RGBColor save, c, blk;
    FontInfo fi;
    short cellW, cellH, textW, total, x, baseline;
    SetupStripFont(NULL);                    /* port is already set by the caller */
    BuildLabel(st, s);
    GetFontInfo(&fi);
    cellW = r->right - r->left;
    cellH = r->bottom - r->top;
    textW = StringWidth(s);
    total = textW + 2 + kArrowW;
    x = r->left + (cellW - total) / 2;
    baseline = r->top + (cellH - (fi.ascent + fi.descent)) / 2 + fi.ascent;

    if (erase) EraseRect(r);                  /* clear our own cell before repaint */

    GetForeColor(&save);
    blk.red = blk.green = blk.blue = 0;
    if (DisplayRed(st)) { c.red = 0xFFFF; c.green = 0; c.blue = 0; RGBForeColor(&c); }
    else RGBForeColor(&blk);
    MoveTo(x, baseline);
    DrawString(s);

    RGBForeColor(&blk);
    DrawArrow(x + textW + 2, r->top + (cellH - 3) / 2);
    RGBForeColor(&save);
}

/* ---------- Control Strip entry point ---------- */

pascal long ControlStripModule(long message, long params,
                               Rect *statusRect, GrafPtr statusPort)
{
    TempState *st = (TempState *)params;

    switch (message) {
        case sdevInitModule: {
            TempState *ns = (TempState *)NewPtrSys(sizeof(TempState));
            if (ns != NULL) {
                memset(ns, 0, sizeof(TempState));
                ns->cpuPeakX10 = -30000;
                ns->shownVal = -30000;
                ns->shownMode = -1;
                ns->dispMode = M_CPU;
                CtoP("Your CPU temperature exceeds safe operating levels. "
                     "Consider shutting down your computer to preserve CPU health.", ns->nmMsg);
                if ((params & 0xFFFFFF00L) == kSettingsMagic) {
                    ns->useF = (Boolean)(params & 1);
                    ns->dispMode = (int)((params >> 1) & 3);
                    if (ns->dispMode > M_MAX) ns->dispMode = M_CPU;
                }
                LoadPrefs(ns);   /* persistent prefs win over the in-session value */
                ns->valid   = find_i2c(ns);
                ns->backend = detect_backend(ns);
                if (ns->backend == kBackendNone) ns->valid = false;  /* nothing to read  */
                if (ns->valid) {
                    ReadAll(ns);
                    if (ns->dispMode == M_CASE && !ns->haveCase) ns->dispMode = M_CPU;
                }
            }
            return (long)ns;
        }

        case sdevCloseModule:
            if (st != NULL) {
                if (st->alerted) NMRemove(&st->nm);   /* drop any pending over-temp alert */
                DisposePtr((Ptr)st);
            }
            return 0;

        case sdevFeatures:
            /* sdevDontAutoTrack: take over tracking so the menu opens on mouse-DOWN
             * (and tracks while held), matching every other Control Strip module.
             * Without it the strip auto-tracks and only calls us on mouse-up. */
            return (1L << sdevWantMouseClicks) | (1L << sdevDontAutoTrack)
                 | (1L << sdevHasCustomHelp);

        case sdevGetDisplayWidth:
            return FixedWidth(st, statusPort);

        case sdevPeriodicTickle:
            /* The Control Strip only repaints a module when its width changes
             * (via sdevResizeDisplay) or when it is externally invalidated. This
             * module's width is fixed, so relying on the resize return left the
             * reading frozen until the user prodded it (reported on the forum).
             * Instead we repaint OURSELVES here, in place, whenever the displayed
             * value changes, using the port/rect cached from the last real draw. */
            if (st != NULL && st->valid) {
                UInt32 now = TickCount();
                if ((now - st->lastRead) >= READ_TICKS) {
                    SInt16 nv; Boolean nr; int nm;
                    st->lastRead = now;
                    ReadAll(st);
                    nv = (SInt16)DisplayValue(st);
                    nr = DisplayRed(st);
                    nm = st->dispMode;
                    if (st->haveCache && st->cachePort != NULL
                        && (nv != st->shownVal || nr != st->shownRed || nm != st->shownMode)
                        && SBIsControlStripVisible()) {
                        GrafPtr savePort;
                        GetPort(&savePort);
                        SetPort(st->cachePort);
                        DrawCell(st, &st->cacheRect, true);   /* erase + repaint in place */
                        SetPort(savePort);
                    }
                    st->shownVal = nv;
                    st->shownRed = nr;
                    st->shownMode = nm;
                }
            }
            return 0;

        case sdevDrawStatus:
            if (statusPort != NULL) SetPort(statusPort);
            if (st != NULL && statusRect != NULL && statusPort != NULL) {
                st->cachePort = statusPort;   /* remember where/how to self-repaint */
                st->cacheRect = *statusRect;  /* during the periodic tickle          */
                st->haveCache = true;
                /* keep the change-tracking current so the next tickle does not
                 * repaint needlessly right after this strip-driven draw */
                st->shownVal  = (SInt16)DisplayValue(st);
                st->shownRed  = DisplayRed(st);
                st->shownMode = st->dispMode;
            }
            DrawCell(st, statusRect, false);  /* strip already cleared the cell */
            return 0;

        case sdevMouseClick:
            if (st != NULL) DoMenu(st, statusRect);
            return (1L << sdevResizeDisplay) | (1L << sdevNeedToSave);

        case sdevSaveSettings:
            if (st != NULL) {
                SavePrefs(st);   /* persist to disk (in addition to the session value) */
                return kSettingsMagic | ((long)(st->dispMode & 3) << 1) | (st->useF ? 1L : 0L);
            }
            return 0;

        case sdevShowBalloonHelp:
            if (st != NULL && st->backend != kBackendNone) {
                Str255 h, num;
                char buf[128];
                int i = 0, k;
                const char *p;
                #define APP(str) do { p = (str); while (*p) buf[i++] = *p++; } while (0)
                #define APN(val) do { NumToString((long)(val), num); for (k = 1; k <= num[0]; k++) buf[i++] = num[k]; } while (0)
                APP("CPU ");
                if (st->haveCpu) {
                    APN(st->useF ? ((long)st->cpuX10 * 9 / 5 + 320 + 5) / 10 : (st->cpuX10 + 5) / 10);
                    buf[i++] = (char)0xA1; buf[i++] = st->useF ? 'F' : 'C';
                } else APP("--");
                if (st->haveCase) {
                    APP("  Case ");
                    APN(st->useF ? ((long)st->caseX10 * 9 / 5 + 320 + 5) / 10 : (st->caseX10 + 5) / 10);
                    buf[i++] = (char)0xA1; buf[i++] = st->useF ? 'F' : 'C';
                }
                buf[i] = 0;
                CtoP(buf, h);
                SBShowHelpString(statusRect, h);
                #undef APP
                #undef APN
            } else {
                Str255 h; CtoP("No readable temperature sensor was found on this Mac.", h);
                SBShowHelpString(statusRect, h);
            }
            return 0;
    }
    return 0;
}
