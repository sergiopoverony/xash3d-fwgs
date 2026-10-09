#!/bin/sh
# Packages Xash3D FWGS PS4 build into installable .pkg
# Usage: scripts/ps4/package.sh [build directory] [output directory]
set -e

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/build}
OUT=${2:-$BUILD}

TITLE="Xash3D FWGS"
VERSION="1.00"
TITLE_ID="XASH00001"
CONTENT_ID="IV0000-${TITLE_ID}_00-XASH3DFWGSPS4000"

if [ -z "$OO_PS4_TOOLCHAIN" ]; then
	echo "Set OO_PS4_TOOLCHAIN to OpenOrbis toolchain directory" >&2
	exit 1
fi

TOOLS="$OO_PS4_TOOLCHAIN/bin/linux"
SAMPLE="$OO_PS4_TOOLCHAIN/samples/hello_world"
STAGE="$BUILD/ps4pkg"

# PkgTool.Core is built on old .NET that needs these to run on modern distros
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

rm -rf "$STAGE"
mkdir -p "$STAGE/sce_sys/about" "$STAGE/sce_module" "$STAGE/valve"

fself_lib()
{
	in="$1"
	out="$2"
	"$TOOLS/create-fself" -in="$in" -out="$STAGE/$out.oelf" --lib="$STAGE/$out" --paid 0x3800000000000011 > /dev/null
	rm -f "$STAGE/$out.oelf"
	echo "  $out"
}

echo "Creating signed images:"
"$TOOLS/create-fself" -in="$BUILD/engine/xash" -out="$STAGE/eboot.oelf" --eboot "$STAGE/eboot.bin" --paid 0x3800000000000011 > /dev/null
rm -f "$STAGE/eboot.oelf"
echo "  eboot.bin"

fself_lib "$BUILD/filesystem/filesystem_stdio.so" filesystem_stdio.prx
fself_lib "$BUILD/3rdparty/mainui/libmenu.so" libmenu.prx

found_ref=0
for name in soft gl gles1 gles2 gl4es gles3compat null; do
	key=$(echo "$name" | tr '[:lower:]' '[:upper:]')
	# only renderers enabled in current configuration
	grep -q "^$key = True" "$BUILD/c4che/_cache.py" || continue
	ref=$(ls "$BUILD"/ref/*/libref_$name.so 2>/dev/null | head -n 1)
	[ -n "$ref" ] || continue
	fself_lib "$ref" "libref_$name.prx"
	found_ref=1
done

if [ $found_ref = 0 ]; then
	echo "ERROR: no renderers found in $BUILD/ref" >&2
	exit 1
fi

cp "$BUILD/3rdparty/extras/extras.pk3" "$STAGE/valve/extras.pk3"
# icon0.png - 512x512 icon, pic0.png/pic1.png - 1920x1080 images, the system shows
# one of them as startup splash until the engine hides it after initialization
cp "$ROOT/engine/platform/ps4/sce_sys/icon0.png" "$ROOT/engine/platform/ps4/sce_sys/pic0.png" "$STAGE/sce_sys/"

# custom images (e.g. from your copy of the game) override defaults, this directory is ignored by git
CUSTOM="$ROOT/ps4_sce_sys"
for f in icon0.png pic0.png pic1.png; do
	if [ -f "$CUSTOM/$f" ]; then
		cp "$CUSTOM/$f" "$STAGE/sce_sys/$f"
		echo "  using custom $f"
	fi
done
# same image is used for both if only one is given
[ -f "$STAGE/sce_sys/pic1.png" ] || cp "$STAGE/sce_sys/pic0.png" "$STAGE/sce_sys/pic1.png"
cp "$SAMPLE/sce_sys/about/right.sprx" "$STAGE/sce_sys/about/right.sprx"
cp "$SAMPLE/sce_module/libc.prx" "$SAMPLE/sce_module/libSceFios2.prx" "$STAGE/sce_module/"

echo "Creating param.sfo"
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

echo "Creating package"
cd "$STAGE"
# create-gp4 has hardcoded directory list, so project file is generated here
python3 - "$CONTENT_ID" <<'PYEOF'
import os, sys, time

content_id = sys.argv[1]
files = []
dirs = set()

for root, dnames, fnames in os.walk('.'):
	for f in fnames:
		path = os.path.relpath(os.path.join(root, f), '.')
		if path.endswith('.gp4'):
			continue
		files.append(path)
		d = os.path.dirname(path)
		while d:
			dirs.add(d)
			d = os.path.dirname(d)

def write_dirs(out, parent, depth):
	children = sorted(d for d in dirs if os.path.dirname(d) == parent)
	for d in children:
		name = os.path.basename(d)
		indent = '\t' * depth
		if any(os.path.dirname(x) == d for x in dirs):
			out.append('%s<dir targ_name="%s">' % (indent, name))
			write_dirs(out, d, depth + 1)
			out.append('%s</dir>' % indent)
		else:
			out.append('%s<dir targ_name="%s" />' % (indent, name))

out = [
	'<?xml version="1.0"?>',
	'<psproject xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" fmt="gp4" version="1000">',
	'\t<volume>',
	'\t\t<volume_type>pkg_ps4_app</volume_type>',
	'\t\t<volume_id>PS4VOLUME</volume_id>',
	'\t\t<volume_ts>%s</volume_ts>' % time.strftime('%Y-%m-%d %H:%M:%S'),
	'\t\t<package content_id="%s" passcode="00000000000000000000000000000000" storage_type="digital50" app_type="full" />' % content_id,
	'\t\t<chunk_info chunk_count="1" scenario_count="1">',
	'\t\t\t<chunks>',
	'\t\t\t\t<chunk id="0" layer_no="0" label="Chunk #0" />',
	'\t\t\t</chunks>',
	'\t\t\t<scenarios default_id="0">',
	'\t\t\t\t<scenario id="0" type="sp" initial_chunk_count="1" label="Scenario #0">0</scenario>',
	'\t\t\t</scenarios>',
	'\t\t</chunk_info>',
	'\t</volume>',
	'\t<files img_no="0">',
]
for f in sorted(files):
	out.append('\t\t<file targ_path="%s" orig_path="%s" />' % (f, f))
out.append('\t</files>')
out.append('\t<rootdir>')
write_dirs(out, '', 2)
out.append('\t</rootdir>')
out.append('</psproject>')

with open('pkg.gp4', 'w') as fp:
	fp.write('\n'.join(out) + '\n')
PYEOF
"$TOOLS/PkgTool.Core" pkg_build pkg.gp4 "$OUT" > /dev/null

echo "Done: $OUT/$CONTENT_ID.pkg"
