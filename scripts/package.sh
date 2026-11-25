#!/usr/bin/env bash
# Build the submission archive BC2025025_OSLab_Project.zip
#
# Contents: source code (git-tracked files), documentation, slides and the
# demo video. Put the slides and video in deliverables/ before running.
set -euo pipefail
cd "$(dirname "$0")/.."

ROLL="BC2025025"
NAME="${ROLL}_OSLab_Project"
OUT="$PWD/${NAME}.zip"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/$NAME"
git archive --format=tar HEAD | tar -x -C "$STAGE/$NAME"

if [[ -d deliverables ]]; then
    cp -r deliverables/. "$STAGE/$NAME/deliverables/" 2>/dev/null || true
fi

rm -f "$OUT"
(cd "$STAGE" && zip -qr "$OUT" "$NAME")
echo "Created $OUT"
unzip -l "$OUT" | tail -n 1
