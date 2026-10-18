#!/usr/bin/env bash
# Publish a firmware image as a GitHub release, together with the manifest the
# device reads.
#
#   ota/publish_github.sh <bin> <version> "<notes>"
#   ota/publish_github.sh esp32/build-muse-waveshare-s3-touch-lcd-185b/muse-gadget.bin 1.1 "音量按钮左移 + TTS 校验"
#
# How the device reads it (esp32/main/ota_github.c):
#   https://github.com/<repo>/releases/latest/download/latest.json
#   {"version":"1.1","url":".../muse-gadget.bin","sha256":"...","size":N,"notes":"..."}
# The image URL is the tag's asset, not `latest`, so an older device that
# checked in late still installs the build that was current when it looked.
#
# The repo must be public: the device pulls the asset with no token. The bin has
# to be the signed one (muse-gadget.bin, not muse-gadget-unsigned.bin): this
# build has CONFIG_SECURE_SIGNED_APPS + CONFIG_SECURE_SIGNED_ON_UPDATE on, and
# esp_https_ota_finish() rejects an image the signing key did not sign.
set -euo pipefail

bin=${1:?usage: publish_github.sh <bin> <version> "<notes>"}
ver=${2:?usage: publish_github.sh <bin> <version> "<notes>"}
notes=${3:-"Muse 1.85B $ver"}

py=${PYTHON:-$HOME/.venv/bin/python3}
if ! command -v "$py" >/dev/null 2>&1; then
    py=python3
    command -v "$py" >/dev/null 2>&1 || py=python   # Windows/Git Bash
fi
[ -f "$bin" ] || { echo "no such image: $bin" >&2; exit 1; }

root=$(cd "$(dirname "$0")/.." && pwd)
repo=${REPO:-$(git -C "$root" config --get remote.origin.url | sed -E 's#(git@github.com:|https://github.com/)##; s#\.git$##')}
[ -n "$repo" ] || { echo "set REPO=owner/name (no git remote found)" >&2; exit 1; }

# The version inside the image is what the device's gate compares, and what it
# reports as running. Publishing a label that disagrees would make the release
# page and the device disagree, so refuse instead.
"$py" - "$bin" "$ver" <<'PY'
import struct, sys
path, want = sys.argv[1], sys.argv[2]
with open(path, "rb") as f:
    head = f.read(0x30 + 32)
magic, = struct.unpack_from("<I", head, 0x20)
if magic != 0xABCD5432:
    sys.exit(f"not an ESP-IDF app image (magic 0x{magic:08x} at 0x20): {path}")
got = head[0x30:0x30 + 32].split(b"\0")[0].decode(errors="replace")
if got != want:
    sys.exit(f"image says version '{got}', the tag says '{want}': rebuild with version.txt = {want}")
print(f"app version {got}")
PY

size=$(stat -c %s "$bin")
sha=$(sha256sum "$bin" | cut -d' ' -f1)
name=$(basename "$bin")
url="https://github.com/$repo/releases/download/v$ver/$name"
tag="v$ver"

out=$root/ota/latest.json
cat > "$out" <<JSON
{
  "version": "$ver",
  "url": "$url",
  "sha256": "$sha",
  "size": $size,
  "notes": "$notes"
}
JSON
echo "--- $out"
cat "$out"

if command -v gh >/dev/null 2>&1; then
    if gh release view "$tag" -R "$repo" >/dev/null 2>&1; then
        gh release upload "$tag" "$bin" "$out" -R "$repo" --clobber
        gh release edit "$tag" -R "$repo" --title "Muse 1.85B $ver" --notes "$notes"
    else
        gh release create "$tag" "$bin" "$out" -R "$repo" --title "Muse 1.85B $ver" --notes "$notes"
    fi
else
    # No GitHub CLI on this machine: same thing over the REST API, with the
    # token git already stores for this remote.
    echo "--- gh not found, using the REST API"
    "$py" "$root/ota/github_release.py" "$tag" "$notes" "$bin" "$out"
fi

echo "--- published $tag on $repo"
echo "device URL: https://github.com/$repo/releases/latest/download/latest.json"
# The first build with the Update page has to go in over USB (the version gate is
# numeric: a board running 1.0 skips anything that is not greater). Releases
# after that arrive from Settings > Update.
