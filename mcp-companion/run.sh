#!/usr/bin/env bash
# Audacity MCP Companion — convenience launcher (Codex app-server edition)

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

# ── Check Codex CLI ─────────────────────────────────────────────────────────
CODEX_BIN="${CODEX_BIN:-codex}"
if ! command -v "$CODEX_BIN" &>/dev/null; then
  echo "ERROR: '$CODEX_BIN' (Codex CLI) not found in PATH."
  echo "  Install Codex CLI, then log in once with:  codex login"
  exit 1
fi

# ── Friendly reminder about Audacity ────────────────────────────────────────
MCP_URL="${MCP_URL:-http://127.0.0.1:4830/mcp}"
if ! curl -s --max-time 2 -X POST "$MCP_URL" \
     -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' >/dev/null 2>&1; then
  echo "NOTE: Audacity MCP server not reachable at ${MCP_URL}."
  echo "  Launch the patched Audacity (mod-mcp-server enabled) first."
  echo "  The chat will still load; the Audacity chip turns green once it's up."
  echo ""
fi

# ── Start server ─────────────────────────────────────────────────────────────
echo "Starting Audacity MCP Companion on ${URL} ..."
echo "  Backend  : Codex app-server (ChatGPT login, persistent threads)"
echo "  MCP URL  : ${MCP_URL}"
echo "  Model    : ${CODEX_MODEL:-<account default>}"
echo ""
echo "Press Ctrl+C to stop."
echo ""

cd "$SCRIPT_DIR"
python3 server.py &
SERVER_PID=$!

sleep 1.0
if command -v open &>/dev/null; then
  open "$URL"
elif command -v xdg-open &>/dev/null; then
  xdg-open "$URL"
fi

wait $SERVER_PID
