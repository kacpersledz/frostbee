#!/bin/bash
# Usage: ./release.sh 0.5.0
#
# Updates ota_index.json with the computed OTA file version,
# commits, tags, and pushes — all in one commit so the tag
# always points to the correct index.

set -euo pipefail

VERSION="${1:?Usage: $0 <MAJOR.MINOR.PATCH>}"
VERSION="${VERSION#v}"  # strip leading v if present

IFS='.' read -r MAJOR MINOR PATCH <<< "$VERSION"

# Validate
if ! [[ "$MAJOR" =~ ^[0-9]+$ && "$MINOR" =~ ^[0-9]+$ && "$PATCH" =~ ^[0-9]+$ ]]; then
    echo "Error: version must be MAJOR.MINOR.PATCH (e.g. 1.2.3)" >&2
    exit 1
fi

FILE_VER=$(( (MAJOR << 24) | (MINOR << 16) | PATCH ))
FILE_VER_HEX=$(printf '0x%08X' "$FILE_VER")
TAG="v${VERSION}"

echo "Version:     ${VERSION}"
echo "Tag:         ${TAG}"
echo "fileVersion: ${FILE_VER} (${FILE_VER_HEX})"

# Update ota_index.json
printf '[
  {
    "url": "https://github.com/winterberryice/frostbee/releases/latest/download/frostbee.zigbee",
    "manufacturerCode": 4660,
    "imageType": 1,
    "fileVersion": %s
  }
]\n' "$FILE_VER" > ota_index.json

git add ota_index.json
git commit -m "release: v${VERSION} (OTA fileVersion: ${FILE_VER_HEX})"
git tag "$TAG"
echo ""
echo "Ready to push. Run:"
echo "  git push origin master ${TAG}"
