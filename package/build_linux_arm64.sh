#!/usr/bin/env bash

# Container-side Linux ARM64 build orchestrator.

set -euo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPOSITORY_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)
LINUX_ARM64_DIR="$SCRIPT_DIR/linux-arm64"

# shellcheck source=linux-arm64/dependencies.lock
source "$LINUX_ARM64_DIR/dependencies.lock"

# Consumed by the sourced build metadata helpers.
# shellcheck disable=SC2034
ZLM_REPOSITORY_ROOT=$REPOSITORY_ROOT
# shellcheck disable=SC2034
ZLM_LINUX_ARM64_DIR=$LINUX_ARM64_DIR
# shellcheck disable=SC2034
ZLM_BUILD_SCRIPT="$SCRIPT_DIR/build_linux_arm64.sh"
# shellcheck source=linux-arm64/lib/build-metadata.sh
source "$LINUX_ARM64_DIR/lib/build-metadata.sh"

BUILD_ROOT="$REPOSITORY_ROOT/.build/linux-arm64"
SOURCE_ROOT="$BUILD_ROOT/sources"
INSTALL_ROOT="$BUILD_ROOT/install"
ZLM_BUILD_DIR="$BUILD_ROOT/zlmediakit"
CCACHE_DIR=${CCACHE_DIR:-$BUILD_ROOT/ccache}

assert_linux_arm64() {
  case "$(uname -s)/$(uname -m)" in
    Linux/aarch64 | Linux/arm64)
      ;;
    *)
      printf 'Unsupported build platform: %s/%s. Linux ARM64 is required.\n' \
        "$(uname -s)" "$(uname -m)" >&2
      exit 1
      ;;
  esac
}

metadata_matches() {
  local metadata=$1
  local fingerprint=$2
  local input_hash=$3

  [ -f "$metadata" ] &&
    grep -Fqx "builder_fingerprint=$fingerprint" "$metadata" &&
    grep -Fqx "dependency_input_hash=$input_hash" "$metadata" &&
    grep -Fqx "libsrtp_sha=$LIBSRTP_SHA" "$metadata" &&
    grep -Fqx "openssl_sha=$OPENSSL_SHA" "$metadata" &&
    grep -Fqx "usrsctp_sha=$USRSCTP_SHA" "$metadata"
}

build_dependencies() {
  local jobs=$1

  rm -rf -- "$INSTALL_ROOT"
  mkdir -p "$INSTALL_ROOT"

  cd "$SOURCE_ROOT/openssl"
  make distclean >/dev/null 2>&1 || true
  ./config \
    no-shared \
    no-tests \
    no-asm \
    no-dso \
    -fPIC \
    --prefix="$INSTALL_ROOT"
  make -j"$jobs"
  make install_sw

  rm -rf -- "$SOURCE_ROOT/usrsctp/.zlm-build"
  cmake \
    -S "$SOURCE_ROOT/usrsctp" \
    -B "$SOURCE_ROOT/usrsctp/.zlm-build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_ROOT" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -Dsctp_build_programs=OFF \
    -Dsctp_build_shared_lib=OFF \
    -Dsctp_build_static_lib=ON
  cmake --build "$SOURCE_ROOT/usrsctp/.zlm-build" --parallel "$jobs"
  cmake --install "$SOURCE_ROOT/usrsctp/.zlm-build"

  cd "$SOURCE_ROOT/libsrtp"
  make distclean >/dev/null 2>&1 || true
  CFLAGS="-fcommon -I$INSTALL_ROOT/include" \
    LDFLAGS="-L$INSTALL_ROOT/lib" \
    LIBS='-ldl -lpthread' \
    ./configure \
      --enable-openssl \
      --with-openssl-dir="$INSTALL_ROOT" \
      --prefix="$INSTALL_ROOT"
  make -j"$jobs"
  make install
}

assert_linux_arm64

for source_dir in openssl usrsctp libsrtp; do
  if [ ! -d "$SOURCE_ROOT/$source_dir/.git" ]; then
    printf 'Dependency source is missing: %s\n' "$SOURCE_ROOT/$source_dir" >&2
    printf 'Run ./build-linux-arm64.sh so sources are prepared first.\n' >&2
    exit 1
  fi
done

jobs=${BUILD_JOBS:-$(nproc)}
source_sha=${SOURCE_SHA:-$(git -C "$REPOSITORY_ROOT" rev-parse HEAD)}
source_state_sha=${SOURCE_STATE_SHA:-$source_sha}
source_dirty=${SOURCE_DIRTY:-false}
source_revision=${SOURCE_REVISION:-${source_sha:0:7}}
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(
  git -C "$REPOSITORY_ROOT" show -s --format=%ct "$source_sha"
)}

fingerprint=$(zlm_builder_fingerprint)
input_hash=$(zlm_dependency_input_hash)
dependency_metadata="$INSTALL_ROOT/.zlmediakit-build-metadata"

if metadata_matches "$dependency_metadata" "$fingerprint" "$input_hash"; then
  printf 'ZLMediaKit dependencies: cache hit\n'
else
  printf 'ZLMediaKit dependencies: cache miss\n'
  build_dependencies "$jobs"
  {
    printf 'builder_fingerprint=%s\n' "$fingerprint"
    printf 'dependency_input_hash=%s\n' "$input_hash"
    printf 'libsrtp_sha=%s\n' "$LIBSRTP_SHA"
    printf 'openssl_sha=%s\n' "$OPENSSL_SHA"
    printf 'usrsctp_sha=%s\n' "$USRSCTP_SHA"
  } >"$dependency_metadata"
fi

export CCACHE_DIR
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-750M}
mkdir -p "$CCACHE_DIR"
ccache --set-config=max_size="$CCACHE_MAXSIZE"
ccache --zero-stats >/dev/null
trap 'ccache --show-stats || true' EXIT

rm -rf -- "$ZLM_BUILD_DIR" "$REPOSITORY_ROOT/release/linux/Release"
mkdir -p "$ZLM_BUILD_DIR"

export PKG_CONFIG_PATH="$INSTALL_ROOT/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake \
  -S "$REPOSITORY_ROOT" \
  -B "$ZLM_BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_ROOT" \
  -DCMAKE_PREFIX_PATH="$INSTALL_ROOT" \
  -DOPENSSL_ROOT_DIR="$INSTALL_ROOT" \
  -DOPENSSL_USE_STATIC_LIBS=TRUE \
  -DSRTP_PREFIX="$INSTALL_ROOT" \
  -DENABLE_OPENSSL=ON \
  -DENABLE_WEBRTC=ON \
  -DENABLE_SCTP=ON \
  -DENABLE_SRT=ON \
  -DENABLE_TESTS=OFF
cmake --build "$ZLM_BUILD_DIR" --parallel "$jobs"

for feature in ENABLE_OPENSSL ENABLE_WEBRTC ENABLE_SCTP ENABLE_SRT; do
  if ! grep -Fq -- "-D$feature" "$ZLM_BUILD_DIR/compile_commands.json"; then
    printf 'Expected feature was not compiled: %s\n' "$feature" >&2
    exit 1
  fi
done

test_status=0
cmake -S "$REPOSITORY_ROOT" -B "$ZLM_BUILD_DIR" -DENABLE_TESTS=ON
cmake --build "$ZLM_BUILD_DIR" --target test_http_cookie --parallel "$jobs" || test_status=$?
if [ "$test_status" -eq 0 ]; then
  "$REPOSITORY_ROOT/release/linux/Release/test_http_cookie" || test_status=$?
fi
cmake -S "$REPOSITORY_ROOT" -B "$ZLM_BUILD_DIR" -DENABLE_TESTS=OFF
if [ "$test_status" -ne 0 ]; then
  exit "$test_status"
fi

export \
  BUILD_ROOT \
  INSTALL_ROOT \
  LIBSRTP_SHA \
  OPENSSL_SHA \
  REPOSITORY_ROOT \
  SOURCE_DATE_EPOCH \
  ZLM_BUILD_DIR \
  fingerprint \
  input_hash \
  source_dirty \
  source_revision \
  source_sha \
  source_state_sha \
  USRSCTP_SHA
"$LINUX_ARM64_DIR/package-runtime.sh"
