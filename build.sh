#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH="$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH"
test -f "$DEVKITPRO/libounce/switch2_rules" || {
    echo "Missing libounce Switch 2 toolchain at $DEVKITPRO" >&2
    exit 1
}
# devkitPro's rules expect make paths without spaces. Leave the workspace intact.
stage_dir=$(mktemp -d /tmp/local-update-tool.XXXXXX)
trap 'rm -rf "$stage_dir"' EXIT HUP INT TERM
cp "$project_dir/Makefile" "$stage_dir/"
cp -R "$project_dir/source" "$project_dir/include" "$stage_dir/"
make -C "$stage_dir" -j4
dist_dir="$project_dir/dist"
sd_path="switch/local-update-tool/local-update-tool.nro"
mkdir -p "$dist_dir/switch/local-update-tool"
cp "$stage_dir/local-update-tool.nro" "$dist_dir/$sd_path"
cp "$stage_dir/local-update-tool.elf" "$stage_dir/local-update-tool.map" "$dist_dir/"
python3 "$project_dir/tests/verify_artifact.py" "$dist_dir"
python3 - "$dist_dir" "$sd_path" <<'PY'
import hashlib, pathlib, sys, zipfile
root = pathlib.Path(sys.argv[1])
sd_path = sys.argv[2]
zip_path = root / 'local-update-tool-sd.zip'
with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as archive:
    archive.write(root / sd_path, sd_path)
paths = [root / sd_path, root / 'local-update-tool.elf', zip_path]
(root / 'SHA256SUMS').write_text(''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(root)}\n' for p in paths))
PY
echo "Built: $dist_dir/$sd_path"

