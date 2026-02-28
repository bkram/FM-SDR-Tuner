#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
ARTIFACT_ROOT="${REPO_ROOT}/.docker-artifacts"
DEB_ARTIFACT_DIR="${ARTIFACT_ROOT}/debian-arm64"
UBUNTU_ARTIFACT_DIR="${ARTIFACT_ROOT}/ubuntu-arm64"
RPM_ARTIFACT_DIR="${ARTIFACT_ROOT}/fedora-arm64"

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required but not installed"
  exit 1
fi

run_ubuntu_24_04() {
  rm -rf "${UBUNTU_ARTIFACT_DIR}"
  mkdir -p "${UBUNTU_ARTIFACT_DIR}"

  docker run --rm --platform linux/arm64 \
    -v "${REPO_ROOT}:/src" \
    -w /src \
    ubuntu:24.04 \
    bash -lc "apt-get update && apt-get install -y cmake g++ file dpkg-dev pkg-config librtlsdr-dev libusb-1.0-0-dev libssl-dev libasound2-dev libliquid-dev && cmake -S . -B build-ubuntu-arm -DCMAKE_BUILD_TYPE=Release -DFM_TUNER_ENABLE_X86_AVX2=OFF && cmake --build build-ubuntu-arm -j\$(nproc) && cd build-ubuntu-arm && cpack -G DEB && cp ./*.deb /src/.docker-artifacts/ubuntu-arm64/"
}

run_debian_trixie() {
  rm -rf "${DEB_ARTIFACT_DIR}"
  mkdir -p "${DEB_ARTIFACT_DIR}"

  docker run --rm --platform linux/arm64 \
    -v "${REPO_ROOT}:/src" \
    -w /src \
    debian:trixie \
    bash -lc "apt-get update && apt-get install -y cmake g++ file dpkg-dev pkg-config librtlsdr-dev libusb-1.0-0-dev libssl-dev libasound2-dev libliquid-dev && cmake -S . -B build-debian-arm -DCMAKE_BUILD_TYPE=Release -DFM_TUNER_ENABLE_X86_AVX2=OFF && cmake --build build-debian-arm -j\$(nproc) && cd build-debian-arm && cpack -G DEB && cp ./*.deb /src/.docker-artifacts/debian-arm64/"
}

run_fedora_40() {
  rm -rf "${RPM_ARTIFACT_DIR}"
  mkdir -p "${RPM_ARTIFACT_DIR}"

  docker run --rm --platform linux/arm64 \
    -v "${REPO_ROOT}:/src" \
    -w /src \
    fedora:40 \
    bash -lc "dnf install -y cmake gcc-c++ make pkgconfig openssl-devel alsa-lib-devel rtl-sdr-devel liquid-dsp-devel rpm-build && cmake -S . -B build-fedora-arm -DCMAKE_BUILD_TYPE=Release -DFM_TUNER_ENABLE_X86_AVX2=OFF && cmake --build build-fedora-arm -j\$(nproc) && cd build-fedora-arm && cpack -G RPM && cp ./*.rpm /src/.docker-artifacts/fedora-arm64/"
}

smoke_test_deb_pkg() {
  local image="$1"
  local pkg_dir="$2"
  docker run --rm --platform linux/arm64 \
    -v "${pkg_dir}:/pkg:ro" \
    "$image" \
    bash -lc "apt-get update && apt-get install -y ca-certificates && apt-get install -y /pkg/*.deb && ldd /usr/bin/fm-sdr-tuner | tee /tmp/ldd.txt && ! grep -q 'not found' /tmp/ldd.txt && /usr/bin/fm-sdr-tuner --help >/tmp/help.txt 2>&1 && grep -q 'Usage:' /tmp/help.txt"
}

smoke_test_rpm_pkg() {
  docker run --rm --platform linux/arm64 \
    -v "${RPM_ARTIFACT_DIR}:/pkg:ro" \
    fedora:40 \
    bash -lc "dnf install -y /pkg/*.rpm && ldd /usr/bin/fm-sdr-tuner | tee /tmp/ldd.txt && ! grep -q 'not found' /tmp/ldd.txt && /usr/bin/fm-sdr-tuner --help >/tmp/help.txt 2>&1 && grep -q 'Usage:' /tmp/help.txt"
}

run_ubuntu_24_04
run_debian_trixie
run_fedora_40
smoke_test_deb_pkg debian:trixie "${DEB_ARTIFACT_DIR}"
smoke_test_deb_pkg ubuntu:24.04 "${UBUNTU_ARTIFACT_DIR}"
smoke_test_rpm_pkg

echo "arm64 Docker build and smoke tests passed for ubuntu:24.04, debian:trixie, and fedora:40"
