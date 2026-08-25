#!/usr/bin/env bash

# Public entry point for local and GitHub Actions Linux ARM64 builds.

set -euo pipefail

REPOSITORY_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="$REPOSITORY_ROOT/package/linux-arm64"
BUILD_IMAGE=${ZLM_BUILD_IMAGE:-zlmediakit-build:debian11-arm64}

usage() {
  cat >&2 <<EOF
Usage:
  $0 [build]
  $0 image
  $0 check
  $0 cache-info
  $0 archive
  $0 clean
EOF
}

assert_docker_arm64() {
  local docker_arch
  local docker_os

  if ! command -v docker >/dev/null 2>&1; then
    printf 'Docker is required to build ZLMediaKit.\n' >&2
    exit 1
  fi

  docker_os=$(docker info --format '{{.OSType}}')
  docker_arch=$(docker info --format '{{.Architecture}}')
  case "$docker_os/$docker_arch" in
    linux/aarch64 | linux/arm64)
      ;;
    *)
      printf 'Unsupported Docker platform: %s/%s. Linux ARM64 is required.\n' \
        "$docker_os" "$docker_arch" >&2
      exit 1
      ;;
  esac
}

build_image() {
  docker build \
    --platform linux/arm64 \
    --file "$BUILD_DIR/Dockerfile" \
    --tag "$BUILD_IMAGE" \
    "$BUILD_DIR"
}

ensure_image() {
  if ! docker image inspect "$BUILD_IMAGE" >/dev/null 2>&1; then
    build_image
  fi
}

repository_is_dirty() {
  if ! git -C "$REPOSITORY_ROOT" diff --quiet HEAD --; then
    return 0
  fi

  [ -n "$(
    git -C "$REPOSITORY_ROOT" ls-files --others --exclude-standard
  )" ]
}

calculate_source_state_sha() {
  local untracked_path

  {
    printf 'head=%s\n' "$1"
    git -C "$REPOSITORY_ROOT" diff --binary HEAD --
    while IFS= read -r -d '' untracked_path; do
      printf 'untracked=%s\n' "$untracked_path"
      git -C "$REPOSITORY_ROOT" hash-object -- "$untracked_path"
    done < <(
      git -C "$REPOSITORY_ROOT" \
        ls-files -z --others --exclude-standard
    )
  } | git hash-object --stdin
}

run_check() {
  docker run \
    --rm \
    --platform linux/arm64 \
    --user "$(id -u):$(id -g)" \
    --workdir /workspace \
    --volume "$REPOSITORY_ROOT:/workspace" \
    --entrypoint /bin/bash \
    "$BUILD_IMAGE" \
    /workspace/package/linux-arm64/tools/check.sh
}

run_repository_script() {
  local script=$1

  docker run \
    --rm \
    --platform linux/arm64 \
    --user "$(id -u):$(id -g)" \
    --workdir /workspace \
    --volume "$REPOSITORY_ROOT:/workspace" \
    --entrypoint /bin/bash \
    "$BUILD_IMAGE" \
    "$script"
}

run_build() {
  local source_dirty=false
  local source_revision
  local source_sha
  local source_state_sha

  "$BUILD_DIR/prepare-sources.sh"

  source_sha=$(git -C "$REPOSITORY_ROOT" rev-parse HEAD)
  source_state_sha=$source_sha
  if repository_is_dirty; then
    source_dirty=true
    source_state_sha=$(calculate_source_state_sha "$source_sha")
  fi

  if [ "${GITHUB_ACTIONS:-false}" = true ] && [ "$source_dirty" = true ]; then
    printf 'GitHub Actions release builds require a clean source tree.\n' >&2
    exit 1
  fi

  source_revision=${source_sha:0:7}
  if [ "$source_dirty" = true ]; then
    source_revision="$source_revision-dirty.${source_state_sha:0:7}"
  fi

  docker run \
    --rm \
    --init \
    --platform linux/arm64 \
    --user "$(id -u):$(id -g)" \
    --workdir /workspace \
    --volume "$REPOSITORY_ROOT:/workspace" \
    --env HOME=/tmp/zlmediakit-build-home \
    --env SOURCE_DIR=/workspace \
    --env "SOURCE_SHA=$source_sha" \
    --env "SOURCE_STATE_SHA=$source_state_sha" \
    --env "SOURCE_DIRTY=$source_dirty" \
    --env "SOURCE_REVISION=$source_revision" \
    "$BUILD_IMAGE"
}

command_name=${1:-build}
if [ "$#" -gt 1 ]; then
  usage
  exit 2
fi

case "$command_name" in
  image)
    assert_docker_arm64
    build_image
    ;;
  check)
    assert_docker_arm64
    build_image
    run_check
    ;;
  cache-info)
    assert_docker_arm64
    ensure_image
    run_repository_script \
      /workspace/package/linux-arm64/tools/cache-info.sh
    ;;
  archive)
    assert_docker_arm64
    ensure_image
    run_repository_script /workspace/package/linux-arm64/archive.sh
    ;;
  build)
    assert_docker_arm64
    build_image
    run_build
    ;;
  clean)
    rm -rf -- \
      "$REPOSITORY_ROOT/.build/linux-arm64" \
      "$REPOSITORY_ROOT/artifact/zlmediakit-debian11-arm64" \
      "$REPOSITORY_ROOT/artifact/zlmediakit-debian11-arm64.tar.gz" \
      "$REPOSITORY_ROOT/release/linux/Release"
    printf 'Cleaned Linux ARM64 build state.\n'
    ;;
  *)
    usage
    exit 2
    ;;
esac
