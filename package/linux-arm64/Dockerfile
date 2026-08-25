# check=error=true

FROM debian:11@sha256:32fc609441a9c8417b2bb5faa62a60afd6af19013e8e82fdeed5d08dc7ac1ad9

ARG DEBIAN_FRONTEND=noninteractive
ARG ACTIONLINT_VERSION=1.7.12
ARG ACTIONLINT_SHA256=325e971b6ba9bfa504672e29be93c24981eeb1c07576d730e9f7c8805afff0c6
ARG HADOLINT_VERSION=2.14.0
ARG HADOLINT_SHA256=331f1d3511b84a4f1e3d18d52fec284723e4019552f4f47b19322a53ce9a40ed

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        autoconf \
        automake \
        binutils \
        build-essential \
        ca-certificates \
        ccache \
        cmake \
        curl \
        file \
        git \
        libtool \
        mtree-netbsd \
        perl \
        pkg-config \
        python3 \
        shellcheck \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

RUN curl --retry 5 --retry-all-errors --retry-delay 5 \
        -fsSLo /tmp/actionlint.tar.gz \
        "https://github.com/rhysd/actionlint/releases/download/v${ACTIONLINT_VERSION}/actionlint_${ACTIONLINT_VERSION}_linux_arm64.tar.gz" \
    && printf '%s  %s\n' "$ACTIONLINT_SHA256" /tmp/actionlint.tar.gz \
        | sha256sum --check - \
    && tar -xzf /tmp/actionlint.tar.gz -C /tmp actionlint \
    && install -m 0755 /tmp/actionlint /usr/local/bin/actionlint \
    && curl --retry 5 --retry-all-errors --retry-delay 5 \
        -fsSLo /tmp/hadolint \
        "https://github.com/hadolint/hadolint/releases/download/v${HADOLINT_VERSION}/hadolint-linux-arm64" \
    && printf '%s  %s\n' "$HADOLINT_SHA256" /tmp/hadolint \
        | sha256sum --check - \
    && install -m 0755 /tmp/hadolint /usr/local/bin/hadolint \
    && rm -f /tmp/actionlint.tar.gz /tmp/actionlint /tmp/hadolint

WORKDIR /workspace

ENTRYPOINT ["/bin/bash", "/workspace/package/build_linux_arm64.sh"]
