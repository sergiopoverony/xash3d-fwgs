#!/bin/sh
# Builds GNM probe: a small separate PS4 app that checks GPU rendering through opengnm.
# Result is written to /data/xash/gnmprobe.txt on the console.
#
# Usage: scripts/ps4/gnm-probe/build.sh [PS4 IP address]
#   With IP address, uploads .pkg to /data/pkg on the console through FTP (GoldHEN, port 2121).
#
# opengnm and freegnm-examples helpers are fetched at pinned commits, shaders are
# precompiled with opengnm-psbc (see compile-shaders.sh), so only OpenOrbis toolchain is needed.
set -e

PROBE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$PROBE/../../.." && pwd)
OUT="$ROOT/build-gnm-probe"
DEPS="$OUT/deps"

OPENGNM_REPO=https://github.com/PS4-OpenGNM/opengnm
OPENGNM_COMMIT=4b295ca54c82c83acf308d1c646a2dfa9ae57350
EXAMPLES_REPO=https://github.com/PS4-OpenGNM/freegnm-examples
EXAMPLES_COMMIT=ffd055563e5b849ebef1b276ba29e730199670f8

TITLE="GNM Probe"
VERSION="1.00"
TITLE_ID="GNMP00001"
CONTENT_ID="IV0000-${TITLE_ID}_00-XASHGNMPROBE0000"

if [ -z "$OO_PS4_TOOLCHAIN" ] || [ ! -f "$OO_PS4_TOOLCHAIN/link.x" ]; then
	echo "Set OO_PS4_TOOLCHAIN to OpenOrbis toolchain directory" >&2
	exit 1
fi
TC="$OO_PS4_TOOLCHAIN"
TOOLS="$TC/bin/linux"
SAMPLE="$TC/samples/hello_world"

# same compiler and linker as the engine, see scripts/waifulib/xcompile.py
CC=clang-18
LD=ld.lld-18
AR=llvm-ar-18
command -v $AR > /dev/null || AR=ar

fetch()
{
	dir="$DEPS/$1"
	if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
		rm -rf "$dir"
		git init -q "$dir"
		git -C "$dir" fetch -q --depth 1 "$2" "$3"
		git -C "$dir" checkout -q FETCH_HEAD
	fi
	echo "  $1 $(git -C "$dir" rev-parse --short HEAD)"
}

echo "=== Fetching dependencies"
mkdir -p "$DEPS"
fetch opengnm "$OPENGNM_REPO" "$OPENGNM_COMMIT"
fetch freegnm-examples "$EXAMPLES_REPO" "$EXAMPLES_COMMIT"

# log redirection is force-included everywhere, so opengnm messages go to the log too
FORCE="-include $PROBE/probe_log.h"

echo "=== Building opengnm"
cp "$DEPS/opengnm/config.orbis.mak" "$DEPS/opengnm/config.mak"
make -s -C "$DEPS/opengnm" lib CC="$CC $FORCE" AR="$AR" TOOLCHAIN="$TC" > "$OUT/opengnm-build.log" 2>&1 || {
	tail -n 30 "$OUT/opengnm-build.log"
	echo "opengnm build failed, full log: $OUT/opengnm-build.log" >&2
	exit 1
}

echo "=== Building probe"
SHARED="$DEPS/freegnm-examples/shared/src"
OBJ="$OUT/obj"
rm -rf "$OBJ"
mkdir -p "$OBJ"
# flags as in freegnm-examples triangle, which runs on hardware
CFLAGS="--target=x86_64-ps4-elf -fPIC -isysroot $TC -isystem $TC/include -std=c11 -O2 -g \
	-Wall -D_BSD_SOURCE -DUSE_OPENGNM -I$DEPS/opengnm/include -I$SHARED $FORCE"
for src in "$PROBE/probe.c" "$SHARED/u/fs.c" "$SHARED/u/utility.c" "$SHARED/displayctx.c" "$SHARED/memalloc.c" "$SHARED/misc.c"; do
	$CC $CFLAGS -c "$src" -o "$OBJ/$(basename "$src" .c).o"
done

CRT="$TC/lib/crt1.o"
for i in crti.o crtn.o; do
	[ -f "$TC/lib/$i" ] && CRT="$CRT $TC/lib/$i"
done
$LD -o "$OUT/gnmprobe.elf" "$OBJ"/*.o "$DEPS/opengnm/libopengnm.a" \
	-m elf_x86_64 -pie --script "$TC/link.x" --eh-frame-hdr -L"$TC/lib" \
	-lc -lkernel -lSceGnmDriver -lSceVideoOut -lSceSystemService $CRT

echo "=== Packaging"
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
STAGE="$OUT/pkg"
rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$STAGE/assets/misc"
"$TOOLS/create-fself" -in="$OUT/gnmprobe.elf" -out="$STAGE/eboot.oelf" --eboot "$STAGE/eboot.bin" --paid 0x3800000000000011 > /dev/null
rm -f "$STAGE/eboot.oelf"
cp "$PROBE"/shaders/*.sb "$STAGE/assets/misc/"
cp "$ROOT/engine/platform/ps4/sce_sys/icon0.png" "$STAGE/sce_sys/"
cp "$SAMPLE/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/right.sprx"
cp "$SAMPLE/sce_module/libc.prx" "$SAMPLE/sce_module/libSceFios2.prx" "$STAGE/sce_module/"

SFO="$STAGE/sce_sys/param.sfo"
"$TOOLS/PkgTool.Core" sfo_new "$SFO" > /dev/null
set_sfo() { "$TOOLS/PkgTool.Core" sfo_setentry "$SFO" "$@" > /dev/null; }
set_sfo APP_TYPE --type Integer --maxsize 4 --value 1
set_sfo APP_VER --type Utf8 --maxsize 8 --value "$VERSION"
set_sfo ATTRIBUTE --type Integer --maxsize 4 --value 0
set_sfo CATEGORY --type Utf8 --maxsize 4 --value gd
set_sfo CONTENT_ID --type Utf8 --maxsize 48 --value "$CONTENT_ID"
set_sfo DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
set_sfo SYSTEM_VER --type Integer --maxsize 4 --value 0
set_sfo TITLE --type Utf8 --maxsize 128 --value "$TITLE"
set_sfo TITLE_ID --type Utf8 --maxsize 12 --value "$TITLE_ID"
set_sfo VERSION --type Utf8 --maxsize 8 --value "$VERSION"

(cd "$STAGE" && python3 "$ROOT/scripts/ps4/make-gp4.py" "$CONTENT_ID" && "$TOOLS/PkgTool.Core" pkg_build pkg.gp4 "$OUT" > /dev/null)
PKG="$OUT/$CONTENT_ID.pkg"
echo "Done: $PKG"

if [ -n "$1" ]; then
	echo "=== Uploading to $1"
	curl --ftp-create-dirs -T "$PKG" "ftp://$1:2121/data/pkg/"
	echo "Uploaded. Install it with Package Installer, run GNM Probe, then fetch /data/xash/gnmprobe.txt"
fi
