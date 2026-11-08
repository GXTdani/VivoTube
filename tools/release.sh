#!/bin/sh
# Builds VivoTube.vpk and publishes it as a GitHub release, which installed copies then offer
# as an update. Needs the GitHub CLI (gh), signed in with push access to the repository.
#   sh tools/release.sh [notes-file]
# The tag is v<VT_VERSION> from CMakeLists.txt; bump VITA_VERSION and VT_VERSION there first.
set -e
cd "$(dirname "$0")/.."
REPO="${VT_REPO:-hamburger101/VivoTube}"
VERSION=$(sed -n 's/^set(VT_VERSION "\(.*\)")$/\1/p' CMakeLists.txt)
[ -n "$VERSION" ] || { echo "VT_VERSION not found in CMakeLists.txt" >&2; exit 1; }
TAG="v$VERSION"
if [ -f secrets.cmake ]; then
    echo "secrets.cmake is present: its OAuth client would be built into the public release." >&2
    echo "Move it aside (or set VT_ALLOW_SECRETS=1) and run again." >&2
    [ "$VT_ALLOW_SECRETS" = "1" ] || exit 1
fi
sh tools/build_vita.sh
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    echo "Release $TAG already exists; uploading the VPK to it."
    gh release upload "$TAG" build/VivoTube.vpk --repo "$REPO" --clobber
else
    if [ -n "$1" ]; then NOTES="--notes-file $1"; else NOTES="--generate-notes"; fi
    gh release create "$TAG" build/VivoTube.vpk --repo "$REPO" --title "VivoTube $VERSION" $NOTES
fi
echo "Published $TAG"
