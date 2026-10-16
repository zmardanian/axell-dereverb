#!/usr/bin/env bash
# Builds the GitHub-ready public repository (~/axell-dereverb-public): sources, the trained weights and their parity
# reference, the Axell core (vendored into
# external/axell-core, so the repository builds on its own), tests, docs, audit and licences - no build output or
# downloaded tools. A new repository gets one clean commit; an existing one is updated in place
# ("Axell De-Reverb v<ver>", tag v<ver>). Sets the remote, never pushes.
set -euo pipefail
cd "$(dirname "$0")/.."
PUB="${DR_PUBLIC_DIR:-$HOME/axell-dereverb-public}"
CORE="${AXELL_CORE:-$(realpath ../axell-core)}"
VER=$(sed -n 's/^project(AxellDeReverb VERSION \([0-9.]*\)).*/\1/p' CMakeLists.txt)
FINAL="$PUB"; UPDATE=0
if [ -d "$PUB/.git" ]; then UPDATE=1; PUB="$(mktemp -d)/stage"
elif [ -e "$PUB" ]; then echo "$PUB exists but is not a git repository - remove it first."; exit 1; fi
mkdir -p "$PUB/tests/independent" "$PUB/docs" "$PUB/external/axell-core"
for f in CMakeLists.txt LICENSE README.md CHANGELOG.md THIRD_PARTY_NOTICES.md; do cp "$f" "$PUB/"; done
cp -r Source scripts Resources "$PUB/"
mkdir -p "$PUB/tools" "$PUB/research"
cp tools/*.cpp tools/*.h "$PUB/tools/"
cp research/*.py "$PUB/research/"
cp tests/*.cpp tests/lsan.supp tests/report*.txt tests/pluginval-report.txt tests/vst3-validator-report.txt "$PUB/tests/"
cp tests/independent/*.py tests/independent/audit-report.txt "$PUB/tests/independent/"
cp -r tests/reference "$PUB/tests/"
cp -r docs/. "$PUB/docs/"
# the core at the exact commit this release was built with
(cd "$CORE" && git diff --quiet && git diff --cached --quiet) || { echo "$CORE has uncommitted changes."; exit 1; }
for d in CMakeLists.txt LICENSE README.md include src resources tests tools; do cp -r "$CORE/$d" "$PUB/external/axell-core/"; done
rm -rf "$PUB/external/axell-core/tools/__pycache__"
echo "axell-core $(git -C "$CORE" rev-parse --short HEAD)" > "$PUB/external/axell-core/VERSION"
cat > "$PUB/.gitignore" <<'IGN'
build/
build-*/
tools/pluginval*
tools/vst3-validator
__pycache__/
*.user
IGN
if [ "$UPDATE" = 1 ]; then
  git -C "$FINAL" diff --quiet && git -C "$FINAL" diff --cached --quiet || { echo "$FINAL has uncommitted changes."; exit 1; }
  git -C "$FINAL" rev-parse -q --verify "refs/tags/v$VER" >/dev/null && { echo "tag v$VER already exists in $FINAL."; exit 1; }
  rsync -a --delete --exclude .git "$PUB/" "$FINAL/"; rm -rf "$(dirname "$PUB")"
  cd "$FINAL"; git add -A; git commit -qm "Axell De-Reverb v$VER" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"; git tag "v$VER"
  echo "Public repo updated: $FINAL (tag v$VER, NOT pushed)"; exit 0
fi
cd "$PUB"
git init -q -b main
git config user.name "zmardanian"
git config user.email "333182917+zmardanian@users.noreply.github.com"
git add -A
git commit -qm "Axell De-Reverb v$VER" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
git tag "v$VER"
git remote add origin "https://github.com/zmardanian/axell-dereverb.git"
echo "Public repo ready: $PUB (commit $(git rev-parse --short HEAD), tag v$VER, remote origin set, NOT pushed)"
