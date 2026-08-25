#!/usr/bin/env bash

# Creates the directly downloadable tar.gz consumed by GitHub Actions.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
ARTIFACT_NAME=zlmediakit-debian11-arm64
PACKAGE_DIR="$REPOSITORY_ROOT/artifact/$ARTIFACT_NAME"
ARCHIVE_PATH="$REPOSITORY_ROOT/artifact/$ARTIFACT_NAME.tar.gz"

if [ ! -f "$PACKAGE_DIR/BUILDINFO.txt" ]; then
  printf 'Build information is missing: %s\n' \
    "$PACKAGE_DIR/BUILDINFO.txt" >&2
  exit 1
fi

source_date_epoch=$(sed -n 's/^source_date_epoch=//p' \
  "$PACKAGE_DIR/BUILDINFO.txt")
if ! grep -Eq '^[0-9]+$' <<<"$source_date_epoch"; then
  printf 'Invalid source_date_epoch in BUILDINFO.txt\n' >&2
  exit 1
fi

rm -f -- "$ARCHIVE_PATH"
tar \
  --format=gnu \
  --sort=name \
  --mtime="@$source_date_epoch" \
  --owner=0 \
  --group=0 \
  --numeric-owner \
  -C "$REPOSITORY_ROOT/artifact" \
  -cf - \
  "$ARTIFACT_NAME" |
  gzip -n >"$ARCHIVE_PATH"

printf '%s\n' "artifact/$ARTIFACT_NAME.tar.gz"
