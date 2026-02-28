#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required but not installed"
  exit 1
fi

run_debian_trixie() {
  docker run --rm --platform linux/arm64 \
    -v "${REPO_ROOT}:/src" \
    -w /src \
    debian:trixie \
    bash -lc "apt-get update && apt-get install -y cmake g++ pkg-config librtlsdr-dev libusb-1.0-0-dev libssl-dev libasound2-dev libliquid-dev && cmake -S . -B build-debian-arm -DCMAKE_BUILD_TYPE=Release -DFM_TUNER_ENABLE_X86_AVX2=OFF && cmake --build build-debian-arm -j\$(nproc)"
}

run_fedora_40() {
  docker run --rm --platform linux/arm64 \
    -v "${REPO_ROOT}:/src" \
    -w /src \
    fedora:40 \
    bash -lc "dnf install -y cmake gcc-c++ make pkgconfig openssl-devel alsa-lib-devel rtl-sdr-devel liquid-dsp-devel && cmake -S . -B build-fedora-arm -DCMAKE_BUILD_TYPE=Release -DFM_TUNER_ENABLE_X86_AVX2=OFF && cmake --build build-fedora-arm -j\$(nproc)"
}

run_debian_trixie
run_fedora_40

echo "arm64 Docker build checks passed for debian:trixie and fedora:40"
