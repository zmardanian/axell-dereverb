#!/usr/bin/env bash
# pluginval 1.0.4 passes .../Name.vst3/Contents/x86_64-linux/Name.so; the VST3 SDK 3.8 validator
# wants the bundle folder. Translate the path and forward everything else unchanged.
set -uo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
args=()
for a in "$@"; do
  if [[ "$a" == *.vst3/Contents/*/*.so ]]; then
    a="${a%%/Contents/*}"
  fi
  args+=("$a")
done
exec "$DIR/tools/vst3-validator" "${args[@]}"
