#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BIN="$BUILD_DIR/fm-sdr-tuner"
CONFIG_FILE="${CONFIG_FILE:-$ROOT_DIR/fm-sdr-tuner.ini}"

if [ ! -x "$BIN" ]; then
  echo "binary not found: $BIN" >&2
  echo "build it first with:" >&2
  echo "  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DFM_TUNER_ENABLE_X86_AVX2=OFF" >&2
  echo "  cmake --build build -j4" >&2
  exit 1
fi

RTL_DEVICE_INDEX="${RTL_DEVICE_INDEX:-0}"
FREQ_KHZ="${FREQ_KHZ:-101100}"
IQ_RATE="${IQ_RATE:-256000}"
XDR_PASSWORD="${XDR_PASSWORD:-test123}"

echo "Starting fm-sdr-tuner in local auto-start mode"
echo "  binary:   $BIN"
echo "  config:   $CONFIG_FILE"
echo "  device:   $RTL_DEVICE_INDEX"
echo "  freq_khz: $FREQ_KHZ"
echo "  iq_rate:  $IQ_RATE"
echo "  xdr:      127.0.0.1:7373"
echo "  password: $XDR_PASSWORD"

exec "$BIN" \
  -c "$CONFIG_FILE" \
  --source rtl_sdr \
  --rtl-device "$RTL_DEVICE_INDEX" \
  --iq-rate "$IQ_RATE" \
  --auto-start \
  -f "$FREQ_KHZ" \
  -s \
  -P "$XDR_PASSWORD" \
  "$@"
