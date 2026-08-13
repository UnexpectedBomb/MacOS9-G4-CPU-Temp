#!/usr/bin/env python3
"""
set-custom-icon.py -- turn on the Finder flags a built Control Strip module needs.

Rez stamps the output's Finder type/creator but has no option for Finder
flags, so a freshly Rez'd file has fdFlags = 0.  This patcher sets:
  * 0x0400 (kHasCustomIcon) so the custom icon at resource ID -16455 shows; and
  * 0x2000 (kHasBundle)     so the Finder reads the file's BNDL/FREF -- which is
                            what lets the Control Strip DRAG A COPY of the module
                            out of the strip (stock modules all have this bit).

It works on BOTH artifact formats because each lays the Finder type+creator
immediately before the Finder flags:
  * MacBinary (.bin): type@65, creator@69, flags hi-byte @73  (== type+8)
  * raw HFS  (.dsk):  the catalog FInfo has fdType, fdCreator, then fdFlags

So the rule is identical: find the 8-byte "<type><creator>" signature, then
OR 0x04 into the byte at +8 (the high byte of the big-endian 16-bit flags).
HFS catalog records carry no checksum, so an in-place byte patch is safe
(wrap-apm.py already edits the MDB the same way).

Usage:  set-custom-icon.py <file> [TYPECREA]   (default signature: sdevUTcs)
Idempotent: re-running just re-asserts the bit.
"""

import sys
from pathlib import Path

FLAGS_HI = 0x24   # high byte of kHasBundle (0x2000) | kHasCustomIcon (0x0400)


def patch(path: Path, sig: bytes) -> None:
    data = bytearray(path.read_bytes())
    n = data.count(sig)
    if n != 1:
        sys.exit(f"{path}: expected exactly one {sig!r}, found {n}")
    i = data.index(sig)
    flags_hi = i + len(sig)          # +8: high byte of fdFlags
    before = data[flags_hi]
    data[flags_hi] |= FLAGS_HI
    path.write_bytes(data)
    print(f"{path.name}: fdFlags hi 0x{before:02X} -> 0x{data[flags_hi]:02X} "
          f"(kHasBundle | kHasCustomIcon) at offset {flags_hi}")


if __name__ == "__main__":
    if not (2 <= len(sys.argv) <= 3):
        sys.exit(__doc__.strip())
    target = Path(sys.argv[1])
    signature = (sys.argv[2] if len(sys.argv) == 3 else "sdevUTcs").encode("ascii")
    patch(target, signature)
