#!/bin/sh
# Publish a firmware release the watch can install over the internet.
#   ./tools/release.sh 1.2.0 "What changed"
# Tags v1.2.0, builds (the version comes from the tag), uploads build/esp-watch-os.bin
# to a GitHub release. Watches with "Check automatically" on will offer it within 6 hours,
# or immediately via Settings > Updates > Check.
set -e
VER="$1"; NOTES="${2:-ESP-Watch OS $1}"
[ -n "$VER" ] || { echo "usage: $0 <version e.g. 1.2.0> [notes]"; exit 1; }
cd "$(dirname "$0")/.."
git diff --quiet && git diff --cached --quiet || { echo "commit your changes first"; exit 1; }
TAG="v$VER"
git tag -a "$TAG" -m "$NOTES"
. "${IDF_PATH:-$HOME/.espressif/v6.1/esp-idf}/export.sh" >/dev/null
idf.py build
grep -q "\"$TAG\"" build/project_description.json || echo "warning: built version may not match $TAG"
git push origin main "$TAG"
gh release create "$TAG" build/esp-watch-os.bin --title "ESP-Watch OS $TAG" --notes "$NOTES"
echo "Released $TAG"
