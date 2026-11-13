#!/usr/bin/env bash
# Copies the built website into the GitHub Pages repository and commits it.
#   tools/publish-site.sh /path/to/northraysoftware-clone [--push]
# The repository's README.md is kept; everything else is replaced by website/dist.
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=${1:?usage: tools/publish-site.sh <pages-repo-clone> [--push]}
[ -d "$REPO/.git" ] || { echo "$REPO isn't a git clone"; exit 1; }
[ -f website/dist/index.html ] || { echo "Build the website first (cd website && npm run build)"; exit 1; }
find "$REPO" -mindepth 1 -maxdepth 1 ! -name .git ! -name README.md -exec rm -rf {} +
cp -a website/dist/. "$REPO/"
VERSION=$(tr -d '[:space:]' < VERSION)
git -C "$REPO" add -A
if git -C "$REPO" diff --cached --quiet; then echo "Nothing changed."; exit 0; fi
MSG="Website and downloads $VERSION ($(date -u +%Y-%m-%d))"
# Optional extra lines for the commit message (e.g. attribution trailers).
[ -n "${COMMIT_TRAILER:-}" ] && MSG="$MSG

$COMMIT_TRAILER"
git -C "$REPO" commit -q -m "$MSG"
echo "Committed: $(git -C "$REPO" log -1 --format=%s)"
if [ "${2:-}" = "--push" ]; then git -C "$REPO" -c push.negotiate=true push origin HEAD; fi
