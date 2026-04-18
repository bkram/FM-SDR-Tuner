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
FREQ_KHZ="${FREQ_KHZ:-88600}"
IQ_RATE="${IQ_RATE:-256000}"
GAIN_DB="${GAIN_DB:-0}"
DURATION_SEC="${DURATION_SEC:-60}"
STEREO_WAV="${STEREO_WAV:-$ROOT_DIR/stereo_88600_60s.wav}"
MPX_WAV="${MPX_WAV:-$ROOT_DIR/mpx_88600_60s.wav}"

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
    --duration)
      if [ "$#" -lt 2 ]; then
        echo "missing value for $1" >&2
        exit 1
      fi
      DURATION_SEC="$2"
      shift 2
      ;;
    --stereo-out)
      if [ "$#" -lt 2 ]; then
        echo "missing value for $1" >&2
        exit 1
      fi
      STEREO_WAV="$2"
      shift 2
      ;;
    --mpx-out)
      if [ "$#" -lt 2 ]; then
        echo "missing value for $1" >&2
        exit 1
      fi
      MPX_WAV="$2"
      shift 2
      ;;
    *)
      echo "unsupported argument: $1" >&2
      echo "usage: $0 [--gain <db>] [--duration <sec>] [--stereo-out <file>] [--mpx-out <file>]" >&2
      exit 1
      ;;
  esac
done

mkdir -p "$(dirname "$STEREO_WAV")"
mkdir -p "$(dirname "$MPX_WAV")"

echo "Capturing decoded stereo WAV and MPX WAV"
echo "  binary:      $BIN"
echo "  config:      $CONFIG_FILE"
echo "  device:      $RTL_DEVICE_INDEX"
echo "  freq_khz:    $FREQ_KHZ"
echo "  iq_rate:     $IQ_RATE"
echo "  gain_db:     $GAIN_DB"
echo "  duration:    $DURATION_SEC s"
echo "  stereo_wav:  $STEREO_WAV"
echo "  mpx_wav:     $MPX_WAV"

cleanup() {
  if [ -n "${APP_PID:-}" ] && kill -0 "$APP_PID" 2>/dev/null; then
    kill -INT "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
  fi
}

trap cleanup INT TERM EXIT

"$BIN" \
  -c "$CONFIG_FILE" \
  --source rtl_sdr \
  --rtl-device "$RTL_DEVICE_INDEX" \
  --iq-rate "$IQ_RATE" \
  --auto-start \
  -f "$FREQ_KHZ" \
  --gain "$GAIN_DB" \
  -w "$STEREO_WAV" \
  --mpx-wav "$MPX_WAV" \
  >/tmp/fm_tuner_both_capture.log 2>&1 &
APP_PID=$!

sleep "$DURATION_SEC"

kill -INT "$APP_PID"
wait "$APP_PID"
APP_PID=

trap - INT TERM EXIT

echo "Capture complete"
echo "  stereo: $STEREO_WAV"
echo "  mpx:    $MPX_WAV"
echo "  log:    /tmp/fm_tuner_both_capture.log"
