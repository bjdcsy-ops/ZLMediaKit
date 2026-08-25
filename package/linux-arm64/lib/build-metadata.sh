#!/usr/bin/env bash

# Shared builder and dependency cache metadata helpers.

: "${ZLM_REPOSITORY_ROOT:?ZLM_REPOSITORY_ROOT is required}"
: "${ZLM_LINUX_ARM64_DIR:?ZLM_LINUX_ARM64_DIR is required}"
: "${ZLM_BUILD_SCRIPT:?ZLM_BUILD_SCRIPT is required}"

zlm_builder_fingerprint() {
  {
    cat /etc/os-release
    dpkg-query --show --showformat='${Package}=${Version}\n' | LC_ALL=C sort
    gcc --version
    ld --version
    cmake --version
  } | sha256sum | cut -c1-16
}

zlm_dependency_input_hash() {
  {
    cat "$ZLM_LINUX_ARM64_DIR/dependencies.lock"
    sha256sum "$ZLM_BUILD_SCRIPT"
    sha256sum "$ZLM_LINUX_ARM64_DIR/Dockerfile"
  } | sha256sum | cut -c1-16
}
