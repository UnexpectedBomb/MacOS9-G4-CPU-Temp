#include "MixedMode.r"

/*
 * The 'sdev' code resource is a Mixed Mode routine descriptor wrapping our PPC
 * PEF. ProcInfo describes the Control Strip entry point:
 *
 *   pascal long ControlStripModule(long message, long params,
 *                                  Rect *statusRect, GrafPtr statusPort)
 *
 *   kPascalStackBased  (bits 0-3)          = 0x0000
 *   result  long   (4 bytes)  3 << 4       = 0x0030
 *   param1  long   (4 bytes)  3 << 6       = 0x00C0
 *   param2  long   (4 bytes)  3 << 8       = 0x0300
 *   param3  Rect*  (4 bytes)  3 << 10      = 0x0C00
 *   param4  GrafPtr(4 bytes)  3 << 12      = 0x3000
 *                                 ProcInfo = 0x00003FF0
 */
type 'sdev' as 'rdes';

resource 'sdev' (128, "CPU Temp", locked) {
    0x00003FF0,
    $$Read("CPUTempCSM.pef")
};

/* Version resource so Get Info shows a version string. Bytes: major (BCD),
 * minor+bugfix (BCD), release stage (0x80 = final), non-release rev, region (0 =
 * US), short version pstring, long "get info" pstring. */
type 'vers' {
    hex byte;                        /* major (BCD)            */
    hex byte;                        /* minor + bugfix (BCD)   */
    hex byte;                        /* release stage          */
    hex byte;                        /* non-release revision   */
    integer;                         /* region code            */
    pstring;                         /* short version          */
    pstring;                         /* long (Get Info) string */
};

resource 'vers' (1) {
    0x01, 0x50, 0x80, 0x00,
    0,
    "1.5",
    "1.5, github.com/UnexpectedBomb"
};

/* Thermometer Finder icon family (ICN#/icl8/ics#/ics8 at -16455). The file's
 * kHasCustomIcon flag is set post-Rez by scripts/set-custom-icon.py. */
#include "therm_icon.r"
