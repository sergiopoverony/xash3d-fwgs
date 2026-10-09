#!/bin/sh
# Recompiles probe shaders: GLSL -> SPIR-V (glslang) -> PS4 shader binary (opengnm-psbc).
# Only needed after changing shaders/*.glsl, ready .sb files are kept in the repository.
#
# Usage: scripts/ps4/gnm-probe/compile-shaders.sh
#   GLSLANG - glslang standalone binary (default: glslang from PATH)
#   PSBC    - opengnm-psbc binary built for the host with our patch, see scripts/ps4/psbc/build.sh
set -e

DIR=$(cd "$(dirname "$0")/shaders" && pwd)
GLSLANG=${GLSLANG:-glslang}
PSBC=${PSBC:-opengnm-psbc}

for src in "$DIR"/*.glsl; do
	name=$(basename "$src" .glsl)
	case "$name" in
	*.vert) glsl=vert; stage=vertex ;;
	*.frag) glsl=frag; stage=fragment ;;
	*) echo "unknown shader stage: $src" >&2; exit 1 ;;
	esac
	"$GLSLANG" -V -S "$glsl" "$src" -o "$DIR/$name.spv" > /dev/null
	# -4: PS4 base GPU (GFX7), also runs on PS4 Pro
	"$PSBC" -s "$stage" -f "$DIR/$name.spv" -o "$DIR/$name.sb" -4
	rm -f "$DIR/$name.spv"
	echo "  $name.sb"
done
