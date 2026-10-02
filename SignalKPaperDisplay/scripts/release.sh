#!/usr/bin/env bash
# Cuts a release: builds every platform at the version in VERSION, publishes
# the binaries as a GitHub release, and rewrites update/manifest.json so
# devices can find them. Run it from WSL (it needs docker, gh and git).
#
#   1. bump VERSION
#   2. bash scripts/release.sh
#   3. review `git diff update/manifest.json`, then commit and push it
#
# Devices only see the new version once the manifest is pushed, so a
# release that fails halfway never reaches them. It deliberately does not
# commit or push for you.

set -euo pipefail
cd "$(dirname "$0")/.."

REPO="takigama/OpenBoatExperiments"
VERSION="$(tr -d '[:space:]' < VERSION)"
TAG="signalkpaperdisplay-v${VERSION}"
MANIFEST="update/manifest.json"

if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
  echo "Release $TAG already exists - bump VERSION first." >&2
  exit 1
fi

make test
make all

PLATFORMS="$(make --no-print-directory list)"
ASSETS=()
ENTRIES=""
for p in $PLATFORMS; do
  asset="dist/${p}/paperdisplay-${p}"
  cp "dist/${p}/paperdisplay" "$asset"
  ASSETS+=("$asset")
  sum="$(sha256sum "$asset" | cut -d' ' -f1)"
  url="https://github.com/${REPO}/releases/download/${TAG}/paperdisplay-${p}"
  ENTRIES+="${ENTRIES:+,}
    \"${p}\": { \"version\": ${VERSION}, \"url\": \"${url}\", \"sha256\": \"${sum}\" }"
done

gh release create "$TAG" "${ASSETS[@]}" --repo "$REPO" \
  --title "SignalKPaperDisplay v${VERSION}" \
  --notes "SignalKPaperDisplay build ${VERSION} for: ${PLATFORMS}"

mkdir -p update
printf '{\n  "platforms": {%s\n  }\n}\n' "$ENTRIES" > "$MANIFEST"
echo
echo "Release $TAG published and $MANIFEST rewritten. Review, then commit and push it:"
echo "  git diff $MANIFEST"
