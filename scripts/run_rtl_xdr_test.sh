#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BIN="$BUILD_DIR/fm-sdr-tuner"
CONFIG_FILE="${CONFIG_FILE:-$ROOT_DIR/fm-sdr-tuner.ini}"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "configuring build directory: $BUILD_DIR"
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DFM_TUNER_ENABLE_X86_AVX2=OFF
fi

echo "Rebuilding fm-sdr-tuner"
cmake --build "$BUILD_DIR" -j4 --target fm-sdr-tuner

if [ ! -x "$BIN" ]; then
  echo "build completed but binary is missing: $BIN" >&2
  exit 1
fi

RTL_DEVICE_INDEX="${RTL_DEVICE_INDEX:-0}"
FREQ_KHZ="${FREQ_KHZ:-101100}"
IQ_RATE="${IQ_RATE:-256000}"
XDR_PASSWORD="${XDR_PASSWORD:-test123}"
GAIN_DB="${GAIN_DB:-}"

while [ "$#" -gt 0 ]; do
  case "$1" in
    --gain|-g)
      if [ "$#" -lt 2 ]; then
        echo "missing value for $1" >&2
        exit 1
      fi
      GAIN_DB="$2"
      shift 2
      ;;
    *)
      echo "unsupported argument: $1" >&2
      echo "usage: $0 [--gain <db>]" >&2
      exit 1
      ;;
  esac
done

echo "Starting fm-sdr-tuner"
echo "  binary:   $BIN"
echo "  config:   $CONFIG_FILE"
echo "  device:   $RTL_DEVICE_INDEX"
echo "  freq_khz: $FREQ_KHZ"
echo "  iq_rate:  $IQ_RATE"
if [ -n "$GAIN_DB" ]; then
  echo "  gain_db:  $GAIN_DB (CLI override)"
else
  echo "  gain_db:  config/default"
fi
echo "  xdr:      127.0.0.1:7373"
echo "  password: $XDR_PASSWORD"

set -- \
  -c "$CONFIG_FILE" \
  --source rtl_sdr \
  --rtl-device "$RTL_DEVICE_INDEX" \
  --iq-rate "$IQ_RATE" \
  -f "$FREQ_KHZ" \
  -s \
  -P "$XDR_PASSWORD" \
  "$@"

if [ -n "$GAIN_DB" ]; then
  set -- "$@" --gain "$GAIN_DB"
fi

exec "$BIN" "$@"
