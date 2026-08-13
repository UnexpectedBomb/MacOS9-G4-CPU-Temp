#!/bin/bash
# package-dist.sh — produce clean distribution artifacts from the Retro68 build.
#
# Retro68's `Rez -o file.bin` writes a MacBinary header whose CRC strict decoders
# (The Unarchiver, modern StuffIt Expander/Deluxe, Apple's `macbinary`) reject as
# invalid — so the raw build .bin often "can't be un-binned" on the target Mac.
# This re-encodes it with Apple's tools into a MacBinary-II file with a correct
# CRC, plus a BinHex (.hqx) that is pure 7-bit ASCII and survives any transfer.
#
# Requires macOS (uses /usr/bin/macbinary, /usr/bin/binhex, /usr/bin/SetFile).
# Usage:  scripts/package-dist.sh <built CPUTempCSM.bin> <output dir>

set -eu
SRC="${1:?path to the Retro68-built CPUTempCSM.bin}"
OUT="${2:?output directory}"
TYPE="sdev"; CREATOR="CPUt"; NAME="CPUTempCSM"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# 1. extract BOTH forks from the build's MacBinary and rebuild a real forked
#    file. The module is now a 'cfrg' fragment: the PEF lives in the DATA FORK
#    and the resources in the resource fork. (An earlier version assumed an empty
#    data fork and read the resource fork from offset 128 -- which silently
#    dropped the PEF and garbled the resources once we moved to a cfrg build.)
python3 - "$SRC" "$TMP/$NAME" <<'PY'
import sys, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
dlen = struct.unpack('>I', d[83:87])[0]                 # data fork (PEF)
rlen = struct.unpack('>I', d[87:91])[0]                 # resource fork
dstart = 128
rstart = 128 + ((dlen + 127) // 128) * 128              # forks are 128-byte padded
open(dst, 'wb').write(d[dstart:dstart+dlen])
open(dst + '/..namedfork/rsrc', 'wb').write(d[rstart:rstart+rlen])
print("data fork: %d bytes; resource fork: %d bytes" % (dlen, rlen))
PY

# 2. stamp type/creator + Finder flags: B = kHasBundle (needed to drag a copy of
#    the module out of the Control Strip), C = kHasCustomIcon (shows our icon).
/usr/bin/SetFile -t "$TYPE" -c "$CREATOR" "$TMP/$NAME"
/usr/bin/SetFile -a BC "$TMP/$NAME"

mkdir -p "$OUT"
# 3. clean MacBinary-II (correct CRC) + BinHex, both verified
/usr/bin/macbinary encode -t 2 "$TMP/$NAME" -o "$OUT/$NAME.bin" -n
/usr/bin/macbinary probe "$OUT/$NAME.bin"
/usr/bin/binhex encode "$TMP/$NAME" -o "$OUT/$NAME.hqx" -n
/usr/bin/binhex probe "$OUT/$NAME.hqx"
echo "OK: wrote $OUT/$NAME.bin (MacBinary II) and $OUT/$NAME.hqx (BinHex)"
