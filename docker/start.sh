#!/usr/bin/env sh
set -eu

API_PORT="${API_PORT:-3001}"
UI_PORT="${UI_PORT:-3000}"
DATA_PATH="${DATA_PATH:-/data/timer-data.json}"
API_BASE_URL="${API_BASE_URL:-http://127.0.0.1:${API_PORT}}"

echo "Starting API on port ${API_PORT}"
PORT="${API_PORT}" DATA_PATH="${DATA_PATH}" bun run api.ts &
api_pid="$!"

echo "Starting UI on port ${UI_PORT} (proxy to ${API_BASE_URL})"
PORT="${UI_PORT}" API_BASE_URL="${API_BASE_URL}" bun run ui.ts &
ui_pid="$!"

shutdown() {
  kill "${api_pid}" "${ui_pid}" 2>/dev/null || true
  wait "${api_pid}" 2>/dev/null || true
  wait "${ui_pid}" 2>/dev/null || true
}

trap 'shutdown; exit 143' INT TERM

while :; do
  if ! kill -0 "${api_pid}" 2>/dev/null; then
    echo "API process exited; stopping container"
    shutdown
    exit 1
  fi

  if ! kill -0 "${ui_pid}" 2>/dev/null; then
    echo "UI process exited; stopping container"
    shutdown
    exit 1
  fi

  sleep 1
done
