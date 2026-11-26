#!/bin/sh
# NASBridge - Makes the .ffpfsc test images (tests/fixtures/*.ffpfsc.gz).
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The images come from the two tools people make them with, so the tests read
# what is really out there rather than what NASBridge thinks is there:
#   MkPFS 1.1.0 (https://github.com/PSBrew/MkPFS, commit 1d5df56)
#   Dump Forge 0.4.0 (https://github.com/quer3q/ps5-dump-forge, commit 3f7b671)
# Neither is part of the build: run this by hand, with both installed, only
# when the images need making again, and commit what it writes.
#
#   tests/fixtures/make_ffpfsc.sh <mkpfs> <ps5-dump-forge>
#
# The game inside is made up (PPSA01234, "Fixture Quest"): param.json, a
# compressed icon0.png, a 1 MB uncompressed pic0.png (more than a .ffpkg's 12
# direct blocks, and it compresses, so the PFSC blocks are zlib), and some
# random bytes (blocks stored raw).

set -eu
MKPFS=$1
FORGE=$2
OUT=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

mkdir -p PPSA01234-app/sce_sys PPSA01234-app/data/sub
python3 - <<'EOF'
import json, random, struct, zlib

def png(width, height, blue, level):
    rows = [b"\0" + b"".join(bytes((x * 255 // width, y * 255 // height, blue))
                             for x in range(width)) for y in range(height)]
    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data)))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows), level)) + chunk(b"IEND", b""))

sce_sys = "PPSA01234-app/sce_sys/"
open(sce_sys + "icon0.png", "wb").write(png(512, 512, 7, 9))
open(sce_sys + "pic0.png", "wb").write(png(640, 540, 9, 0))
json.dump({"titleId": "PPSA01234", "contentId": "UP0000-PPSA01234_00-NASBRIDGETEST000",
           "contentVersion": "01.020.000",
           "localizedParameters": {"defaultLanguage": "en-US",
                                   "en-US": {"titleName": "Fixture Quest"}}},
          open(sce_sys + "param.json", "w"))
r = random.Random(3)
open("PPSA01234-app/eboot.bin", "wb").write(b"\x7fELF" + bytes(r.getrandbits(8) for _ in range(4000)))
open("PPSA01234-app/data/sub/level.bin", "wb").write(
    bytes(r.getrandbits(8) for _ in range(140000)) + b"\0" * 3000000)
EOF

# Dump Forge: one .ffpfsc per image it can put inside.
for inner in ffpkg exfat ffpfs; do
  "$FORGE" convert PPSA01234-app --to ffpfsc --inner "$inner" -o "forge-$inner.ffpfsc" > /dev/null
done

# MkPFS: the images ShadowMountPlus's README packs ("pack file" of an .exfat,
# a .ffpkg, or an uncompressed PFS image named pfs_image.dat), and what
# "pack folder" makes on its own (an .exfat inside, nested once more).
"$FORGE" convert PPSA01234-app --to exfat -o PPSA01234.exfat > /dev/null
"$FORGE" convert PPSA01234-app --to ffpkg -o PPSA01234.ffpkg > /dev/null
for image in PPSA01234.exfat PPSA01234.ffpkg; do
  mkdir -p "in-$image" && mv "$image" "in-$image/"
done
"$MKPFS" pack file --verify --version PS5 --inode-bits 32 in-PPSA01234.exfat/PPSA01234.exfat mkpfs-exfat.ffpfsc > /dev/null
"$MKPFS" pack file --verify --version PS5 --inode-bits 32 in-PPSA01234.ffpkg/PPSA01234.ffpkg mkpfs-ffpkg.ffpfsc > /dev/null
mkdir -p in-pfs nested
"$MKPFS" pack folder --raw --verify --no-compress --no-adjust-output-file-extension --version PS5 \
  --inode-bits 32 PPSA01234-app in-pfs/pfs_image.dat > /dev/null
"$MKPFS" pack file --verify --version PS5 --inode-bits 32 in-pfs/pfs_image.dat mkpfs-pfs.ffpfsc > /dev/null
"$MKPFS" pack folder --verify --no-adjust-output-file-extension --version PS5 --inode-bits 32 \
  PPSA01234-app nested/pfs_image.dat > /dev/null
"$MKPFS" pack file --verify --version PS5 --inode-bits 32 nested/pfs_image.dat mkpfs-nested.ffpfsc > /dev/null

for image in *.ffpfsc; do
  gzip -9nc "$image" > "$OUT/$image.gz"
done
# What the tests compare against: each sce_sys file's name, size and CRC-32.
python3 - "$OUT/ffpfsc-sce_sys.txt" <<'EOF'
import os, sys, zlib
with open(sys.argv[1], "w") as out:
    for name in sorted(os.listdir("PPSA01234-app/sce_sys")):
        data = open("PPSA01234-app/sce_sys/" + name, "rb").read()
        out.write("%s %d %08x\n" % (name, len(data), zlib.crc32(data)))
EOF
ls -l "$OUT"
