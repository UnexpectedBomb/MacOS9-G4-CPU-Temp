#include "CodeFragments.r"

/*
 * Native PowerPC CFM code fragment — the form every shipping Control Strip
 * module uses (and the only form the Control Strip will let you drag a copy of
 * out of the strip; the older 'sdev'-resource / Mixed-Mode form loaded and ran
 * fine but was not draggable). The PEF lives in the DATA FORK (Rez `--data`);
 * this 'cfrg' locates it and names the fragment. usage = kImportLibraryCFrag
 * (matches every stock module). The fragment `main` is pointed at
 * ControlStripModule after MakePEF by scripts/patch-pef-main.py.
 *
 * The file is still Finder type 'sdev' (how the Control Strip recognizes a
 * module) but carries NO 'sdev' resource now — again matching stock modules.
 */
resource 'cfrg' (0) {
    {
        kPowerPCCFragArch, kIsCompleteCFrag, kNoVersionNum, kNoVersionNum,
        kDefaultStackSize, kNoAppSubFolder,
        kImportLibraryCFrag, kDataForkCFragLocator, kZeroOffset, kCFragGoesToEOF,
        "CPU Temp"
    }
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
    0x01, 0x70, 0x80, 0x00,
    0,
    "1.7",
    "1.7, github.com/UnexpectedBomb"
};

/* Finder bundle. Stock Control Strip modules all carry a BNDL/FREF (+ the
 * kHasBundle flag, set post-Rez by set-custom-icon.py); without it the Control
 * Strip will not let you drag a copy of the module out of the strip. Replicated
 * byte-for-byte from a stock module (SoundSource Strip), retargeted to our
 * creator 'CPUt' and our icon family at -16455.
 *
 * 'CPUt'(0) is the required signature resource (its content is just a version
 * string, like the stock modules'). */
data 'CPUt' (0) {
    "1.7, github.com/UnexpectedBomb"
};

/* FREF: file type 'sdev', local icon ID 0, empty name. */
data 'FREF' (128) {
    $"7364 6576 0000 00"
};

/* BNDL: owner 'CPUt'; FREF local 0 -> FREF 128; ICN# local 0 -> ICN# -16455
 * (0xBFB9 = -16455, our existing thermometer icon family). */
data 'BNDL' (128) {
    $"4350 5574 0000 0001"
    $"4652 4546 0000 0000 0080"
    $"4943 4E23 0000 0000 BFB9"
};

/* Thermometer Finder icon family (ICN#/icl8/ics#/ics8 at -16455). The file's
 * kHasCustomIcon flag is set post-Rez by scripts/set-custom-icon.py. */
#include "therm_icon.r"
