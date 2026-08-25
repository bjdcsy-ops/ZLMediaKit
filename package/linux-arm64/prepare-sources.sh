#!/usr/bin/env bash

# Initializes exact ZLMediaKit submodules and external WebRTC dependencies.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/../.." && pwd)
SOURCE_ROOT="$REPOSITORY_ROOT/.build/linux-arm64/sources"

# shellcheck source=dependencies.lock
source "$SCRIPT_DIR/dependencies.lock"

git_retry() {
  local attempt=1
  local delay=5

  until "$@"; do
    if [ "$attempt" -ge 5 ]; then
      return 1
    fi
    printf 'Git command failed; retrying in %s seconds (%s/5).\n' \
      "$delay" "$attempt" >&2
    sleep "$delay"
    attempt=$((attempt + 1))
    delay=$((delay * 2))
  done
}

prepare_repository() {
  local destination=$1
  local repository=$2
  local sha=$3

  if [ ! -d "$destination/.git" ]; then
    if [ -e "$destination" ]; then
      printf 'Dependency path exists but is not a Git repository: %s\n' \
        "$destination" >&2
      return 1
    fi
    mkdir -p "$destination"
    git -C "$destination" init --quiet
    git -C "$destination" remote add origin "$repository"
  else
    git -C "$destination" remote set-url origin "$repository"
  fi

  if ! git -C "$destination" cat-file -e "$sha^{commit}" 2>/dev/null; then
    git_retry git -C "$destination" fetch \
      --depth=1 \
      --no-tags \
      origin \
      "$sha"
  fi
  git -C "$destination" checkout --detach --force "$sha" >/dev/null

  if [ "$(git -C "$destination" rev-parse HEAD)" != "$sha" ]; then
    printf 'Dependency revision mismatch: %s\n' "$destination" >&2
    return 1
  fi
}

export GIT_TERMINAL_PROMPT=0

git -C "$REPOSITORY_ROOT" config \
  submodule.ZLToolKit.url \
  https://github.com/ZLMediaKit/ZLToolKit.git
git -C "$REPOSITORY_ROOT" config \
  submodule.3rdpart/media-server.url \
  https://github.com/ireader/media-server.git
git -C "$REPOSITORY_ROOT" config \
  submodule.3rdpart/jsoncpp.url \
  https://github.com/open-source-parsers/jsoncpp.git
git -C "$REPOSITORY_ROOT" config \
  submodule.www/webassist.url \
  https://github.com/1002victor/zlm_webassist.git
git -C "$REPOSITORY_ROOT" config \
  submodule.3rdpart/pybind11.url \
  https://github.com/pybind/pybind11.git

git_retry git -C "$REPOSITORY_ROOT" submodule update \
  --init \
  --checkout \
  --jobs 5

if git -C "$REPOSITORY_ROOT" submodule status | grep -Eq '^[+-]'; then
  printf 'One or more submodules are not at the recorded revision.\n' >&2
  exit 1
fi

mkdir -p "$SOURCE_ROOT"
prepare_repository "$SOURCE_ROOT/libsrtp" \
  "$LIBSRTP_REPOSITORY" "$LIBSRTP_SHA"
prepare_repository "$SOURCE_ROOT/openssl" \
  "$OPENSSL_REPOSITORY" "$OPENSSL_SHA"
prepare_repository "$SOURCE_ROOT/usrsctp" \
  "$USRSCTP_REPOSITORY" "$USRSCTP_SHA"

printf 'Linux ARM64 dependency sources are ready.\n'
