#!/usr/bin/env bash
# Installs the Release build into ~/.vst3 (where REAPER looks) and checks libraries on the host.
set -euo pipefail
cd "$(dirname "$0")/.."
NAME="Axell De-Reverb.vst3"
SRC="build/AxellDeReverb_artefacts/Release/VST3/$NAME"
DEST="$HOME/.vst3"
[ -d "$SRC" ] || { echo "No Release build found. Run scripts/build.sh first."; exit 1; }
mkdir -p "$DEST"
rm -rf "$DEST/$NAME"
cp -r "$SRC" "$DEST/"
SO=$(find "$DEST/$NAME/Contents/x86_64-linux" -name '*.so' | head -n1)
if ldd "$SO" | grep -q "not found"; then
  echo "Missing host libraries:"; ldd "$SO" | grep "not found"; exit 1
fi
echo "Installed: $DEST/$NAME"
echo "Now fully close REAPER and start it again; it scans new plugins at startup."
