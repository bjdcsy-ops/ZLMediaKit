#!/usr/bin/env bash

# Runs static checks for the Linux ARM64 build and workflow.

set -euo pipefail

TOOLS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=$(cd -- "$TOOLS_DIR/.." && pwd)
REPOSITORY_ROOT=$(cd -- "$BUILD_DIR/../.." && pwd)

scripts=(
  "$REPOSITORY_ROOT/build-linux-arm64.sh"
  "$REPOSITORY_ROOT/package/build_linux_arm64.sh"
  "$BUILD_DIR"/*.sh
  "$BUILD_DIR"/lib/*.sh
  "$BUILD_DIR"/tools/*.sh
)

bash -n "${scripts[@]}"
shellcheck \
  --external-sources \
  --exclude=SC1090,SC1091 \
  "${scripts[@]}"
actionlint "$REPOSITORY_ROOT/.github/workflows/linux_arm64.yml"
hadolint --ignore DL3008 "$BUILD_DIR/Dockerfile"
command -v mtree >/dev/null

# shellcheck source=../dependencies.lock
source "$BUILD_DIR/dependencies.lock"
for sha in "$LIBSRTP_SHA" "$OPENSSL_SHA" "$USRSCTP_SHA"; do
  if ! grep -Eq '^[0-9a-f]{40}$' <<<"$sha"; then
    printf 'Dependency lock is not a full commit SHA: %s\n' "$sha" >&2
    exit 1
  fi
done

workflow="$REPOSITORY_ROOT/.github/workflows/linux_arm64.yml"
if [ "$(grep -Fc 'actions/upload-artifact@' "$workflow")" -ne 1 ]; then
  printf 'The workflow must upload exactly one artifact.\n' >&2
  exit 1
fi
grep -Fq 'archive: false' "$workflow"
if grep -Fq 'matrix:' "$workflow"; then
  printf 'The ZLMediaKit workflow must not use a build matrix.\n' >&2
  exit 1
fi

printf 'Linux ARM64 build checks passed.\n'
