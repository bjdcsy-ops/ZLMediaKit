#!/usr/bin/env bash

# Verifies a ZLMediaKit runtime directory against its internal manifest.

set -euo pipefail

if [ "$#" -ne 1 ]; then
  printf 'Usage: %s <artifact-directory>\n' "$0" >&2
  exit 2
fi

artifact_dir=$1
manifest="$artifact_dir/MANIFEST.mtree"

if [ ! -d "$artifact_dir" ]; then
  printf 'Artifact directory not found: %s\n' "$artifact_dir" >&2
  exit 1
fi
if [ ! -f "$manifest" ]; then
  printf 'Artifact manifest not found: %s\n' "$manifest" >&2
  exit 1
fi
if ! command -v mtree >/dev/null 2>&1; then
  printf 'mtree is required to verify the artifact manifest.\n' >&2
  exit 1
fi

exclude_file=$(mktemp "${TMPDIR:-/tmp}/zlmediakit-mtree.XXXXXX")
trap 'rm -f -- "$exclude_file"' EXIT
printf 'MANIFEST.mtree\n' >"$exclude_file"

if ! mtree_output=$(
  mtree -P -p "$artifact_dir" -f "$manifest" -X "$exclude_file" 2>&1
); then
  printf 'Artifact does not match manifest: %s\n%s\n' \
    "$manifest" "$mtree_output" >&2
  exit 1
fi
if [ -n "$mtree_output" ]; then
  printf 'Artifact does not match manifest: %s\n%s\n' \
    "$manifest" "$mtree_output" >&2
  exit 1
fi

printf 'Verified artifact manifest: %s\n' "$manifest"
