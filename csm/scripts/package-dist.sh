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

# 1. extract the (intact) resource fork from the build's MacBinary and rebuild a
#    real forked file (data fork is empty; all content is in the resource fork).
python3 - "$SRC" "$TMP/$NAME" <<'PY'
import sys, struct
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
rlen = struct.unpack('>I', d[87:91])[0]
open(dst, 'wb').close()
open(dst + '/..namedfork/rsrc', 'wb').write(d[128:128+rlen])
print("resource fork: %d bytes" % rlen)
PY

# 2. stamp type/creator + the custom-icon flag
/usr/bin/SetFile -t "$TYPE" -c "$CREATOR" "$TMP/$NAME"
/usr/bin/SetFile -a C "$TMP/$NAME"

mkdir -p "$OUT"
# 3. clean MacBinary-II (correct CRC) + BinHex, both verified
/usr/bin/macbinary encode -t 2 "$TMP/$NAME" -o "$OUT/$NAME.bin" -n
/usr/bin/macbinary probe "$OUT/$NAME.bin"
/usr/bin/binhex encode "$TMP/$NAME" -o "$OUT/$NAME.hqx" -n
/usr/bin/binhex probe "$OUT/$NAME.hqx"
echo "OK: wrote $OUT/$NAME.bin (MacBinary II) and $OUT/$NAME.hqx (BinHex)"
