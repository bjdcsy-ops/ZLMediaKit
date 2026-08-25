#!/usr/bin/env bash

# Creates and validates the single Linux ARM64 runtime artifact.

set -euo pipefail
shopt -s nullglob
umask 022
export LC_ALL=C

: "${BUILD_ROOT:?BUILD_ROOT is required}"
: "${INSTALL_ROOT:?INSTALL_ROOT is required}"
: "${LIBSRTP_SHA:?LIBSRTP_SHA is required}"
: "${OPENSSL_SHA:?OPENSSL_SHA is required}"
: "${REPOSITORY_ROOT:?REPOSITORY_ROOT is required}"
: "${SOURCE_DATE_EPOCH:?SOURCE_DATE_EPOCH is required}"
: "${USRSCTP_SHA:?USRSCTP_SHA is required}"
: "${ZLM_BUILD_DIR:?ZLM_BUILD_DIR is required}"
: "${fingerprint:?fingerprint is required}"
: "${input_hash:?input_hash is required}"
: "${source_dirty:?source_dirty is required}"
: "${source_revision:?source_revision is required}"
: "${source_sha:?source_sha is required}"
: "${source_state_sha:?source_state_sha is required}"

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ARTIFACT_NAME=zlmediakit-debian11-arm64
PACKAGE_DIR="$REPOSITORY_ROOT/artifact/$ARTIFACT_NAME"
PACKAGE_MANIFEST="$PACKAGE_DIR/MANIFEST.mtree"
RELEASE_DIR="$REPOSITORY_ROOT/release/linux/Release"

verify_arm64_binary() {
  local binary=$1

  if ! readelf -h "$binary" |
    grep -Eq '^[[:space:]]*Machine:[[:space:]]+AArch64$'; then
    printf 'Expected an AArch64 ELF binary: %s\n' "$binary" >&2
    return 1
  fi
}

verify_runtime_dependencies() {
  local binary=$1
  local ldd_output

  ldd_output=$(ldd "$binary")
  if grep -Fq 'not found' <<<"$ldd_output"; then
    printf 'Unresolved runtime dependency for %s:\n%s\n' \
      "$binary" "$ldd_output" >&2
    return 1
  fi
  if grep -Eq 'lib(crypto|ssl|srtp2|usrsctp)\.so' <<<"$ldd_output"; then
    printf 'Expected WebRTC dependencies to be statically linked in %s:\n%s\n' \
      "$binary" "$ldd_output" >&2
    return 1
  fi
}

generate_manifest() {
  local output_file=$1

  {
    printf '# ZLMediaKit artifact manifest\n'
    printf '# manifest_format=mtree-v1\n'
    printf '# keywords=type,mode,sha256digest,link\n'
    printf '# MANIFEST.mtree is excluded because a manifest cannot hash itself.\n'
    mtree \
      -c \
      -P \
      -k type,mode,sha256digest,link \
      -p "$PACKAGE_DIR" |
      awk '!/^#/ && NF { print }' |
      sed -E 's/(^|[[:space:]])sha256=/\1sha256digest=/'
  } >"$output_file"
}

for required_path in \
  MediaServer \
  libmk_api.so \
  config.ini \
  default.pem \
  www; do
  if [ ! -e "$RELEASE_DIR/$required_path" ]; then
    printf 'Required runtime path is missing: %s\n' \
      "$RELEASE_DIR/$required_path" >&2
    exit 1
  fi
done

rm -rf -- "$PACKAGE_DIR"
mkdir -p "$PACKAGE_DIR/www"
install -m 0755 "$RELEASE_DIR/MediaServer" "$PACKAGE_DIR/MediaServer"
install -m 0755 "$RELEASE_DIR/libmk_api.so" "$PACKAGE_DIR/libmk_api.so"
install -m 0644 "$RELEASE_DIR/config.ini" "$PACKAGE_DIR/config.ini"
install -m 0644 "$RELEASE_DIR/default.pem" "$PACKAGE_DIR/default.pem"
cp -a "$RELEASE_DIR/www/." "$PACKAGE_DIR/www/"
find "$PACKAGE_DIR/www" -name .git -type f -delete
find "$PACKAGE_DIR/www" -type d -exec chmod 0755 {} +
find "$PACKAGE_DIR/www" -type f -exec chmod 0644 {} +

version_date=$(date --utc --date="@$SOURCE_DATE_EPOCH" +%Y.%m.%d)
artifact_version="$version_date-$source_revision"
printf '%s\n' "$artifact_version" >"$PACKAGE_DIR/version.txt"

build_script_sha=$(sha256sum "$REPOSITORY_ROOT/package/build_linux_arm64.sh" |
  awk '{ print $1 }')
package_script_sha=$(sha256sum "$SCRIPT_DIR/package-runtime.sh" |
  awk '{ print $1 }')
source_date_utc=$(date --utc --date="@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)
glibc_version=$(getconf GNU_LIBC_VERSION | awk '{ print $2 }')

{
  printf '# ZLMediaKit build information\n'
  printf '# Format: key=value; ignore comments and blank lines.\n'
  printf 'buildinfo_format=1\n'
  printf 'artifact=%s\n' "$ARTIFACT_NAME"
  printf 'artifact_version=%s\n' "$artifact_version"
  printf 'artifact_manifest=MANIFEST.mtree\n'
  printf 'manifest_format=mtree-v1\n'
  printf 'source_sha=%s\n' "$source_sha"
  printf 'source_state_sha=%s\n' "$source_state_sha"
  printf 'source_dirty=%s\n' "$source_dirty"
  printf 'source_revision=%s\n' "$source_revision"
  printf 'source_date_epoch=%s\n' "$SOURCE_DATE_EPOCH"
  printf 'source_date_utc=%s\n' "$source_date_utc"
  printf 'libsrtp_sha=%s\n' "$LIBSRTP_SHA"
  printf 'openssl_sha=%s\n' "$OPENSSL_SHA"
  printf 'usrsctp_sha=%s\n' "$USRSCTP_SHA"
  printf 'builder_fingerprint=%s\n' "$fingerprint"
  printf 'dependency_input_hash=%s\n' "$input_hash"
  printf 'build_script_sha256=%s\n' "$build_script_sha"
  printf 'package_script_sha256=%s\n' "$package_script_sha"
  printf 'runner=%s\n' "$(uname -m)"
  printf 'glibc_version=%s\n' "$glibc_version"
  printf 'build_type=Release\n'
  printf 'enable_openssl=true\n'
  printf 'enable_webrtc=true\n'
  printf 'enable_sctp=true\n'
  printf 'enable_srt=true\n'
  printf 'enable_tests=false\n'
} >"$PACKAGE_DIR/BUILDINFO.txt"

cat >"$PACKAGE_DIR/README.txt" <<'EOF'
ZLMediaKit Linux ARM64 runtime

Built on Debian 11 for ARM64 with OpenSSL, WebRTC, SCTP, and SRT enabled.
The target system must provide ARM64 Linux with glibc 2.31 or newer.

Run from this directory:
  ./MediaServer -v
  ./MediaServer -c ./config.ini -s ./default.pem

MANIFEST.mtree records the packaged file set, modes, and SHA-256 digests.
EOF

chmod 0644 \
  "$PACKAGE_DIR/BUILDINFO.txt" \
  "$PACKAGE_DIR/README.txt" \
  "$PACKAGE_DIR/version.txt"

grep -Eq \
  '^[0-9]{4}\.[0-9]{2}\.[0-9]{2}-[0-9a-f]{7}(-dirty\.[0-9a-f]{7})?$' \
  "$PACKAGE_DIR/version.txt"
test ! -e "$PACKAGE_DIR/MediaServer.debug"
test ! -e "$PACKAGE_DIR/libmk_api.so.debug"
test -z "$(find "$PACKAGE_DIR" -maxdepth 1 -name 'test_*' -print -quit)"

verify_arm64_binary "$PACKAGE_DIR/MediaServer"
verify_arm64_binary "$PACKAGE_DIR/libmk_api.so"
verify_runtime_dependencies "$PACKAGE_DIR/MediaServer"
verify_runtime_dependencies "$PACKAGE_DIR/libmk_api.so"

version_output=$(cd "$PACKAGE_DIR" && ./MediaServer -v 2>&1)
if ! grep -Fq "${source_sha:0:7}" <<<"$version_output"; then
  printf 'MediaServer version output does not contain source revision:\n%s\n' \
    "$version_output" >&2
  exit 1
fi

manifest_tmp=$(mktemp "$BUILD_ROOT/.artifact-manifest.XXXXXX")
trap 'rm -f -- "$manifest_tmp"' EXIT
generate_manifest "$manifest_tmp"
chmod 0644 "$manifest_tmp"
mv -- "$manifest_tmp" "$PACKAGE_MANIFEST"
trap - EXIT

"$SCRIPT_DIR/tools/verify-artifact.sh" "$PACKAGE_DIR"

printf 'Build completed: %s\n' "$PACKAGE_DIR"
printf 'Artifact manifest: %s\n' "$PACKAGE_MANIFEST"
