#!/usr/bin/env bash
# Audacity MCP Companion — convenience launcher

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORT="${COMPANION_PORT:-8765}"
URL="http://127.0.0.1:${PORT}"

# ── Check Python 3 ──────────────────────────────────────────────────────────
if ! command -v python3 &>/dev/null; then
  echo "ERROR: python3 not found."
  echo "  macOS: install Xcode Command Line Tools with: xcode-select --install"
  exit 1
fi

PY_VER=$(python3 -c "import sys; print(sys.version_info.major * 10 + sys.version_info.minor)")
if [ "$PY_VER" -lt 37 ]; then
  echo "ERROR: Python 3.7 or later is required (ThreadingHTTPServer)."
  echo "  Found: $(python3 --version)"
  exit 1
fi

# ── Check API key ────────────────────────────────────────────────────────────
PROVIDER="${AI_PROVIDER:-anthropic}"

if [ "$PROVIDER" = "anthropic" ]; then
  if [ -z "${ANTHROPIC_API_KEY:-}" ]; then
    echo "WARNING: ANTHROPIC_API_KEY is not set."
    echo "  The chat will show an error until you set it:"
    echo "    export ANTHROPIC_API_KEY=sk-ant-..."
    echo ""
  fi
elif [ "$PROVIDER" = "openai" ]; then
  if [ -z "${OPENAI_API_KEY:-}" ]; then
    echo "WARNING: OPENAI_API_KEY is not set."
    echo "  The chat will show an error until you set it:"
    echo "    export OPENAI_API_KEY=sk-..."
    echo ""
  fi
fi

# ── Start server ─────────────────────────────────────────────────────────────
echo "Starting Audacity MCP Companion on ${URL} ..."
echo "  Provider : ${PROVIDER}"
echo "  Model    : ${AI_MODEL:-<default>}"
echo "  MCP URL  : ${MCP_URL:-http://127.0.0.1:4830/mcp}"
echo ""
echo "Press Ctrl+C to stop."
echo ""

# Launch server in background, open browser, then wait for server
cd "$SCRIPT_DIR"
python3 server.py &
SERVER_PID=$!

# Give the server a moment to start
sleep 0.8

# Open browser (macOS)
if command -v open &>/dev/null; then
  open "$URL"
elif command -v xdg-open &>/dev/null; then
  xdg-open "$URL"
fi

# Wait for the server process (Ctrl+C will kill it)
wait $SERVER_PID
