# SPDX-License-Identifier: Apache-2.0
# syntax=docker/dockerfile:1.7

FROM ubuntu:24.04

ARG TARGETARCH
ARG BAZEL_VERSION=8.2.1
ARG OVF_PREBUILD=0

LABEL org.opencontainers.image.title="OpenVehicleFramework Linux validation lab" \
      org.opencontainers.image.description="Reproducible Linux build, test, and debugging environment" \
      org.opencontainers.image.licenses="Apache-2.0"

ENV DEBIAN_FRONTEND=noninteractive \
    OVF_LAB_CACHE=/var/cache/ovf/bazel \
    OVF_LAB_LOGS=/var/log/ovf \
    OVF_LAB_WORKSPACE=/workspace

RUN apt-get update \
    && apt-get install --no-install-recommends -y \
        ca-certificates \
        clang \
        clang-format \
        cmake \
        curl \
        g++ \
        gdb \
        git \
        iproute2 \
        iputils-ping \
        jq \
        less \
        lsof \
        netcat-openbsd \
        ninja-build \
        openssh-server \
        procps \
        python3 \
        python3-venv \
        rsync \
        strace \
        sudo \
        tcpdump \
        tini \
        tmux \
        tree \
        vim-tiny \
    && case "${TARGETARCH}" in \
         amd64) bazel_arch=x86_64 ;; \
         arm64) bazel_arch=arm64 ;; \
         *) echo "unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
       esac \
    && curl --fail --location --retry 3 \
        --output /usr/local/bin/bazel \
        "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-${bazel_arch}" \
    && curl --fail --location --retry 3 \
        --output /tmp/bazel.sha256 \
        "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-${bazel_arch}.sha256" \
    && sed -E "s#  bazel-${BAZEL_VERSION}-linux-${bazel_arch}#  /usr/local/bin/bazel#" \
        /tmp/bazel.sha256 | sha256sum --check --strict - \
    && chmod 0755 /usr/local/bin/bazel \
    && existing_user="$(getent passwd 1000 | cut -d: -f1)" \
    && existing_group="$(getent group 1000 | cut -d: -f1)" \
    && if [ -n "${existing_user}" ]; then \
         usermod --login ovf --home /home/ovf --move-home --shell /bin/bash "${existing_user}"; \
       else \
         useradd --create-home --shell /bin/bash --uid 1000 ovf; \
       fi \
    && if [ -n "${existing_group}" ] && [ "${existing_group}" != "ovf" ]; then \
         groupmod --new-name ovf "${existing_group}"; \
       fi \
    && usermod --gid ovf ovf \
    && install -d -m 0755 /run/sshd /opt/ovf/source /workspace \
    && install -d -o ovf -g ovf -m 0755 /var/cache/ovf/bazel /var/log/ovf \
    && printf 'ovf ALL=(ALL) NOPASSWD:ALL\n' >/etc/sudoers.d/ovf \
    && chmod 0440 /etc/sudoers.d/ovf \
    && rm -rf /var/lib/apt/lists/* /tmp/bazel.sha256

COPY --chown=ovf:ovf . /opt/ovf/source
COPY --chmod=0755 lab/entrypoint.py /usr/local/bin/ovf-lab

WORKDIR /opt/ovf/source

RUN if [ "${OVF_PREBUILD}" = "1" ]; then \
      runuser --user ovf -- \
        bazel --output_user_root=/var/cache/ovf/bazel \
        test --build_tests_only //:all_tests; \
    fi

VOLUME ["/workspace", "/var/cache/ovf/bazel", "/var/log/ovf"]
EXPOSE 22

ENTRYPOINT ["/usr/bin/tini", "--", "/usr/local/bin/ovf-lab"]
CMD ["bazel", "test", "--config=strict", "//:all_tests"]
