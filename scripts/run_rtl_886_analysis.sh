#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
BIN="$BUILD_DIR/tests/test_rtl_sdr_live"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  echo "configuring build directory: $BUILD_DIR"
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DFM_TUNER_ENABLE_X86_AVX2=OFF
fi

echo "Rebuilding test_rtl_sdr_live"
cmake --build "$BUILD_DIR" -j4 --target test_rtl_sdr_live

if [ ! -x "$BIN" ]; then
  echo "build completed but test binary is missing: $BIN" >&2
  exit 1
fi

export FM_TUNER_RUN_RTL_SDR_ANALYZE="${FM_TUNER_RUN_RTL_SDR_ANALYZE:-1}"
export FM_TUNER_ANALYZE_FREQ_KHZ="${FM_TUNER_ANALYZE_FREQ_KHZ:-88600}"
export FM_TUNER_RTL_DEVICE_INDEX="${FM_TUNER_RTL_DEVICE_INDEX:-0}"
export FM_TUNER_RTL_SAMPLE_RATE="${FM_TUNER_RTL_SAMPLE_RATE:-256000}"

echo "Running live RTL-SDR DSP analysis"
echo "  binary:    $BIN"
echo "  freq_khz:  $FM_TUNER_ANALYZE_FREQ_KHZ"
echo "  device:    $FM_TUNER_RTL_DEVICE_INDEX"
echo "  iq_rate:   $FM_TUNER_RTL_SAMPLE_RATE"
echo "  gain_0.1dB:${FM_TUNER_RTL_GAIN_TENTHS_DB:-auto}"

exec "$BIN" "[rtl_sdr_live]" --reporter console --success "$@"
