#!/usr/bin/env bash

# Emits GitHub Actions outputs for dependency and compiler caches.

set -euo pipefail

TOOLS_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
LINUX_ARM64_DIR=$(cd -- "$TOOLS_DIR/.." && pwd)
REPOSITORY_ROOT=$(cd -- "$LINUX_ARM64_DIR/../.." && pwd)

# Consumed by the sourced build metadata helpers.
# shellcheck disable=SC2034
ZLM_REPOSITORY_ROOT=$REPOSITORY_ROOT
# shellcheck disable=SC2034
ZLM_LINUX_ARM64_DIR=$LINUX_ARM64_DIR
# shellcheck disable=SC2034
ZLM_BUILD_SCRIPT="$REPOSITORY_ROOT/package/build_linux_arm64.sh"
# shellcheck source=../lib/build-metadata.sh
source "$LINUX_ARM64_DIR/lib/build-metadata.sh"

fingerprint=$(zlm_builder_fingerprint)
input_hash=$(zlm_dependency_input_hash)

printf 'builder_fingerprint=%s\n' "$fingerprint"
printf 'dependency_cache_key=%s-%s\n' "$fingerprint" "$input_hash"
printf 'dependency_cache_path=.build/linux-arm64/install\n'
printf 'ccache_path=.build/linux-arm64/ccache\n'
