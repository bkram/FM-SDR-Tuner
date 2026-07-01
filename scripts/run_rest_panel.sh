#!/bin/sh
set -eu

# Launch the temporary REST control panel against a running fm-sdr-tuner that
# has the anonymous REST API enabled:
#
#   [rest]
#   enabled = true
#   port = 9090
#   bind_address = 127.0.0.1
#
# Override the target API or the panel port via env vars:
#   API_URL=http://127.0.0.1:9090 PANEL_PORT=9091 scripts/run_rest_panel.sh

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
API_URL="${API_URL:-http://127.0.0.1:9090}"
PANEL_PORT="${PANEL_PORT:-9091}"

echo "REST control panel"
echo "  api:   $API_URL"
echo "  panel: http://127.0.0.1:$PANEL_PORT"

exec python3 "$ROOT_DIR/scripts/rest_test_panel.py" --api "$API_URL" --port "$PANEL_PORT"
