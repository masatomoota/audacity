#!/bin/bash
# =============================================================================
# Audacity MCP Companion — セットアップ & 起動（Codex app-server 版）
#
# ダブルクリックで実行します。初回は必要なら Codex CLI を自動取得し、
# ブラウザのチャット UI から ChatGPT でログインして使い始められます。
#
#   1. Python 3 を確認（無ければ開発者ツールの導入を案内）
#   2. Codex CLI を解決（PATH / Codex.app / キャッシュ）— 無ければ公式の
#      署名済みバイナリを ~/.audacity-mcp-companion/bin に自動ダウンロード
#   3. Audacity（mod-mcp-server 入り）が起動していなければ起動を試みる
#   4. companion サーバを起動し、ブラウザで http://127.0.0.1:8765 を開く
#
# 別途インストール不要にしたい場合は、Codex CLI をあらかじめ
#   brew install --cask codex
# で入れておくこともできます（その場合 2. のダウンロードは省略）。
# =============================================================================

cd "$(dirname "$0")" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin:$PATH"

SUPPORT="$HOME/.audacity-mcp-companion"
BIN_DIR="$SUPPORT/bin"
MCP_URL="${MCP_URL:-http://127.0.0.1:4830/mcp}"
PORT="${COMPANION_PORT:-8765}"

say()   { printf '%s\n' "$*"; }
pause() { echo; read -r -n 1 -s -p "Enter キーで閉じます… "; echo; }
fail()  { say ""; say "✖ $*"; pause; exit 1; }

say "=== Audacity MCP Companion セットアップ ==="

# --- 0. アーキテクチャ ------------------------------------------------------
if [ "$(uname -m)" != "arm64" ]; then
  fail "このビルドは Apple Silicon (arm64) 専用です（現在: $(uname -m)）。"
fi

# --- 1. Python 3 ------------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
  say "Python 3 が見つかりません。コマンドラインデベロッパツールを導入します…"
  xcode-select --install 2>/dev/null || true
  fail "導入完了後、もう一度この『セットアップして起動』を実行してください。"
fi

# --- 2. Codex CLI -----------------------------------------------------------
CODEX_BIN=""
for c in "${CODEX_BIN:-}" "$BIN_DIR/codex" \
         "/Applications/Codex.app/Contents/Resources/codex" \
         "$(command -v codex 2>/dev/null)"; do
  if [ -n "$c" ] && [ -x "$c" ]; then CODEX_BIN="$c"; break; fi
done

if [ -z "$CODEX_BIN" ]; then
  say "Codex CLI が見つかりません。公式バイナリを取得します（約 249MB・初回のみ）…"
  mkdir -p "$BIN_DIR" || fail "$BIN_DIR を作成できませんでした。"
  TAG="$(curl -fsSL https://api.github.com/repos/openai/codex/releases/latest \
        | python3 -c 'import sys,json; print(json.load(sys.stdin).get("tag_name",""))' 2>/dev/null)"
  [ -n "$TAG" ] || TAG="rust-v0.142.0"   # フォールバック
  URL="https://github.com/openai/codex/releases/download/${TAG}/codex-aarch64-apple-darwin.tar.gz"
  say "  ダウンロード元: $URL"
  if curl -fL --progress-bar "$URL" -o "$BIN_DIR/codex.tgz"; then
    tar -xzf "$BIN_DIR/codex.tgz" -C "$BIN_DIR" || fail "展開に失敗しました。"
    SRC="$(find "$BIN_DIR" -maxdepth 2 -name 'codex-aarch64-apple-darwin' -type f | head -1)"
    [ -n "$SRC" ] || fail "ダウンロードした Codex バイナリが見つかりません。"
    mv -f "$SRC" "$BIN_DIR/codex" && chmod +x "$BIN_DIR/codex"
    rm -f "$BIN_DIR/codex.tgz"
    CODEX_BIN="$BIN_DIR/codex"
  fi
  [ -x "$CODEX_BIN" ] || fail "Codex CLI の取得に失敗しました。ネットワークをご確認ください。"
fi
say "Codex CLI: $CODEX_BIN"
export CODEX_BIN

# --- 3. Audacity（MCP サーバ）を確認・起動 ---------------------------------
if ! curl -fsS --max-time 2 -X POST "$MCP_URL" \
        -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' >/dev/null 2>&1; then
  say "Audacity の MCP サーバ ($MCP_URL) に接続できません。Audacity を起動します…"
  open -a "Audacity" >/dev/null 2>&1 \
    || say "  （Audacity.app を Applications に入れて手動で起動してください）"
fi

# --- 4. companion 起動 ------------------------------------------------------
say ""
say "companion を起動します。ブラウザで http://127.0.0.1:${PORT} を開きます。"
say "初回はチャット画面右上の『Log in with ChatGPT』でサインインしてください。"
say "停止するにはこのウィンドウで Ctrl+C を押します。"
say ""
( sleep 2; open "http://127.0.0.1:${PORT}" >/dev/null 2>&1 ) &
python3 server.py
pause
