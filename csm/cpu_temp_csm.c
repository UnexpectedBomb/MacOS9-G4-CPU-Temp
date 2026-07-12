/*
 * CPU Temp — a Control Strip Module ('sdev') for the Power Mac G4 MDD
 * (PowerMac3,6) that displays, live from the Uni-N/KeyWest I2C bus:
 *   - CPU temperature   (DS1775  @ 7-bit 0x49, register 0, 0.5 C)
 *   - Case temperature  (ADM1030 @ 7-bit 0x2c, local sensor reg 0x0A, 1 C)
 *
 * A click pops a menu (two independent checkmark groups): pick WHAT to show
 * (CPU / Case) and, separately, the UNITS (F / C). Units persist across both.
 *
 * STRICTLY READ-ONLY on both sensors — we never write a sensor or fan register,
 * so the ADM1030 autonomous fan-control loop is untouched. (v4 briefly tried a
 * fan-tachometer-enable write to add a Fan RPM reading; it audibly disturbed the
 * fan and produced no reading, so it was reverted — fan RPM is not obtainable
 * without disturbing Apple's fan loop, which we will not do.)
 *
 * The over-temperature alert always monitors the CPU (DS1775) regardless of
 * which reading is on screen.
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

#define DS1775_ADDR   0x49  /* CPU sensor (confirmed on MDD)      */
#define ADM1030_ADDR  0x2c  /* case sensor (fan controller)       */
#define ADM_REG_TEMP  0x0A  /* local (case) temperature value     */
#define READ_TICKS    120   /* ~2 s between refreshes             */

/* display modes */
#define M_CPU  0
#define M_CASE 1
#define M_MAX  M_CASE

/* popup menu item numbers */
#define kMenuID   200
#define kIt_CPU   1
#define kIt_Case  2
/* item 3 = divider */
#define kIt_Fahr  4
#define kIt_Cels  5

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

typedef struct {
    volatile UInt8 *base;
    int    shift, chan;
    Boolean valid;
    int    dispMode;            /* M_CPU / M_CASE */
    Boolean useF;
    Boolean alerted;
    Boolean haveCpu, haveCase;
    SInt16 cpuX10, caseX10;     /* tenths of a degree C */
    SInt16 cpuPeakX10;
    SInt16 shownVal;
    Boolean shownRed;
    int    shownMode;
    UInt32 lastRead;
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

/* Combined-mode read of `len` bytes from register `sub` of 7-bit `addr7`. */
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

/* ---------- discovery ---------- */

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

/* ---------- readings ---------- */

static void ReadAll(TempState *st)
{
    UInt8 b[2];
    if (!st->valid) return;

    /* CPU (DS1775) — also drives the always-on over-temp alert */
    if (kw_read(st->base, st->shift, DS1775_ADDR, 0x00, b, 2, st->chan) == 0) {
        SInt16 raw = (SInt16)(((UInt16)b[0] << 8) | b[1]);
        st->cpuX10 = (SInt16)(((long)raw * 10) / 256);
        if (!st->haveCpu || st->cpuX10 > st->cpuPeakX10) st->cpuPeakX10 = st->cpuX10;
        st->haveCpu = true;
        if (st->cpuX10 >= MAX_SAFE_C * 10) {
            if (!st->alerted) {
                st->alerted = true;
                memset(&st->nm, 0, sizeof(st->nm));
                st->nm.qType = 8;
                st->nm.nmSound = (Handle)-1L;
                st->nm.nmStr = st->nmMsg;
                st->nm.nmResp = (NMUPP)-1L;
                NMInstall(&st->nm);
            }
        } else if (st->cpuX10 < WARN_C * 10) {
            st->alerted = false;
        }
    }

    /* Case (ADM1030 local sensor): 1 byte, signed, 1 C/LSB — READ ONLY */
    if (kw_read(st->base, st->shift, ADM1030_ADDR, ADM_REG_TEMP, b, 1, st->chan) == 0) {
        st->caseX10 = (SInt16)((SInt16)(SInt8)b[0] * 10);
        st->haveCase = true;
    } else st->haveCase = false;
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

/* build the strip label, e.g. "47<deg>C" */
static void BuildLabel(TempState *st, StringPtr out)
{
    Str255 num;
    short n, i;
    if (!st || !st->valid) { CtoP("n/a", out); return; }
    if (!HaveActive(st))   { CtoP("--", out);  return; }
    NumToString(DisplayValue(st), num);
    n = num[0];
    for (i = 1; i <= n; i++) out[i] = num[i];
    out[n + 1] = 0xA1;                              /* degree symbol */
    out[n + 2] = (unsigned char)(st->useF ? 'F' : 'C');
    out[0] = (unsigned char)(n + 2);
}

static void SetupStripFont(GrafPtr port)
{
    short fontID = 0, fontSize = 0;
    if (port != NULL) SetPort(port);
    if (SBGetControlStripFontID(&fontID) == noErr) TextFont(fontID);
    if (SBGetControlStripFontSize(&fontSize) == noErr) TextSize(fontSize);
    TextFace(bold);
}

/* Fixed cell width = widest reading ("888<deg>F") so the cell never resizes. */
static short FixedWidth(GrafPtr port)
{
    Str255 s;
    s[0] = 5; s[1] = '8'; s[2] = '8'; s[3] = '8'; s[4] = 0xA1; s[5] = 'F';
    SetupStripFont(port);
    return (short)(StringWidth(s) + kArrowW + kCellPad);
}

/* small downward triangle (menu indicator) with top-left at (x,y) */
static void DrawArrow(short x, short y)
{
    MoveTo(x, y);         Line(4, 0);
    MoveTo(x + 1, y + 1); Line(2, 0);
    MoveTo(x + 2, y + 2); Line(0, 0);
}

/* ---------- selection popup ---------- */

static void DoMenu(TempState *st, const Rect *cell)
{
    MenuRef m = NewMenu(kMenuID, "\p");
    short sel;
    if (m == NULL) return;
    AppendMenu(m, "\pCPU Temperature;Case Temperature;(-;Fahrenheit;Celsius");
    CheckItem(m, kIt_CPU,  (Boolean)(st->dispMode == M_CPU));
    CheckItem(m, kIt_Case, (Boolean)(st->dispMode == M_CASE));
    CheckItem(m, kIt_Fahr, (Boolean)st->useF);
    CheckItem(m, kIt_Cels, (Boolean)!st->useF);
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
    st->shownVal = -30000;
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
                ns->valid = find_i2c(ns);
                if (ns->valid) ReadAll(ns);
            }
            return (long)ns;
        }

        case sdevCloseModule:
            if (st != NULL) DisposePtr((Ptr)st);
            return 0;

        case sdevFeatures:
            return (1L << sdevWantMouseClicks) | (1L << sdevHasCustomHelp);

        case sdevGetDisplayWidth:
            return FixedWidth(statusPort);         /* constant — never resizes */

        case sdevPeriodicTickle:
            if (st != NULL && st->valid) {
                UInt32 now = TickCount();
                if ((now - st->lastRead) >= READ_TICKS) {
                    SInt16 v; Boolean red;
                    st->lastRead = now;
                    ReadAll(st);
                    v = (SInt16)DisplayValue(st);
                    red = DisplayRed(st);
                    if (v != st->shownVal || red != st->shownRed || st->dispMode != st->shownMode) {
                        st->shownVal = v; st->shownRed = red; st->shownMode = st->dispMode;
                        return (1L << sdevResizeDisplay);
                    }
                }
            }
            return 0;

        case sdevDrawStatus: {
            Str255 s;
            RGBColor save, c, blk;
            FontInfo fi;
            short cellW, cellH, textW, total, x, baseline;
            if (statusPort != NULL) SetPort(statusPort);
            SetupStripFont(statusPort);
            BuildLabel(st, s);
            GetFontInfo(&fi);
            cellW = statusRect->right - statusRect->left;
            cellH = statusRect->bottom - statusRect->top;
            textW = StringWidth(s);
            total = textW + 2 + kArrowW;
            x = statusRect->left + (cellW - total) / 2;
            baseline = statusRect->top + (cellH - (fi.ascent + fi.descent)) / 2 + fi.ascent;

            GetForeColor(&save);
            blk.red = blk.green = blk.blue = 0;
            if (DisplayRed(st)) { c.red = 0xFFFF; c.green = 0; c.blue = 0; RGBForeColor(&c); }
            else RGBForeColor(&blk);
            MoveTo(x, baseline);
            DrawString(s);

            RGBForeColor(&blk);
            DrawArrow(x + textW + 2, statusRect->top + (cellH - 3) / 2);
            RGBForeColor(&save);
            return 0;
        }

        case sdevMouseClick:
            if (st != NULL) DoMenu(st, statusRect);
            return (1L << sdevResizeDisplay) | (1L << sdevNeedToSave);

        case sdevSaveSettings:
            if (st != NULL)
                return kSettingsMagic | ((long)(st->dispMode & 3) << 1) | (st->useF ? 1L : 0L);
            return 0;

        case sdevShowBalloonHelp:
            if (st != NULL && st->valid) {
                Str255 h, num;
                char buf[96];
                int i = 0, k;
                const char *p;
                #define APP(str) do { p = (str); while (*p) buf[i++] = *p++; } while (0)
                #define APN(val) do { NumToString((long)(val), num); for (k = 1; k <= num[0]; k++) buf[i++] = num[k]; } while (0)
                APP("CPU ");
                if (st->haveCpu) { APN(st->useF ? ((long)st->cpuX10 * 9 / 5 + 320 + 5) / 10 : (st->cpuX10 + 5) / 10); buf[i++] = (char)0xA1; buf[i++] = st->useF ? 'F' : 'C'; }
                else APP("--");
                APP("  Case ");
                if (st->haveCase) { APN(st->useF ? ((long)st->caseX10 * 9 / 5 + 320 + 5) / 10 : (st->caseX10 + 5) / 10); buf[i++] = (char)0xA1; buf[i++] = st->useF ? 'F' : 'C'; }
                else APP("--");
                buf[i] = 0;
                CtoP(buf, h);
                SBShowHelpString(statusRect, h);
                #undef APP
                #undef APN
            } else {
                Str255 h; CtoP("CPU / case temperature", h);
                SBShowHelpString(statusRect, h);
            }
            return 0;
    }
    return 0;
}
