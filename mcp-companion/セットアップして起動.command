#!/bin/bash
# =============================================================================
# Audacity ＋ AI チャット（MCP Companion）を一緒に起動（Codex app-server 版）
#
# ダブルクリックすると、Audacity（MCP サーバ内蔵）と AI チャットの両方を
# 立ち上げます。
#
#   1. Python 3 を確認（無ければ開発者ツールの導入を案内）
#   2. Audacity を起動（未起動なら）— DMG 内 / Applications / dev ビルドから自動検出
#   3. Codex CLI を解決（PATH / Codex.app / キャッシュ）— 無ければ公式の
#      署名済みバイナリを ~/.audacity-mcp-companion/bin に自動ダウンロード
#   4. companion サーバを起動し、ブラウザで http://127.0.0.1:8765 を開く
#
# Codex CLI を手動で入れる場合: brew install --cask codex
# =============================================================================

cd "$(dirname "$0")" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin:$PATH"

SUPPORT="$HOME/.audacity-mcp-companion"
BIN_DIR="$SUPPORT/bin"
MCP_URL="${MCP_URL:-http://127.0.0.1:4830/mcp}"
PORT="${COMPANION_PORT:-8765}"

say()    { printf '%s\n' "$*"; }
pause()  { echo; read -r -n 1 -s -p "Enter キーで閉じます… "; echo; }
fail()   { say ""; say "✖ $*"; pause; exit 1; }
mcp_up() { curl -fsS --max-time 2 -X POST "$MCP_URL" \
             -d '{"jsonrpc":"2.0","id":1,"method":"ping"}' >/dev/null 2>&1; }

say "=== Audacity ＋ AI チャットを起動 ==="

# --- 0. アーキテクチャ ------------------------------------------------------
[ "$(uname -m)" = "arm64" ] || \
  fail "このビルドは Apple Silicon (arm64) 専用です（現在: $(uname -m)）。"

# --- 1. Python 3 ------------------------------------------------------------
if ! command -v python3 >/dev/null 2>&1; then
  say "Python 3 が見つかりません。コマンドラインデベロッパツールを導入します…"
  xcode-select --install 2>/dev/null || true
  fail "導入完了後、もう一度この起動ファイルを実行してください。"
fi

# --- 2. Audacity を起動（先に立ち上げてプラグインスキャンを並行させる）-----
if mcp_up; then
  say "Audacity は既に起動しています。"
else
  # Otis.app 探索順: DMG 同梱の隣 → インストール済み → 開発ビルド → 旧名(Audacity.app)
  # への後方互換。バックスラッシュ継続行の末尾には絶対にコメントを置かない
  # (bash がコメントを継続行より優先するため、for ループが途中で切れる)。
  AUD=""
  for cand in \
    "../Otis.app" \
    "/Applications/Otis.app" \
    "$HOME/Applications/Otis.app" \
    "../build/RelWithDebInfo/Otis.app" \
    "../Audacity.app" \
    "/Applications/Audacity.app" \
    "../build/RelWithDebInfo/Audacity.app"; do
    if [ -d "$cand" ]; then AUD="$cand"; break; fi
  done
  if [ -n "$AUD" ]; then
    say "Otis を起動します: $AUD"
    open "$AUD" 2>/dev/null || open -a "Otis" 2>/dev/null || open -a "Audacity" 2>/dev/null \
      || say "  起動に失敗しました。手動で Otis を開いてください。"
  else
    open -a "Otis" 2>/dev/null || open -a "Audacity" 2>/dev/null \
      || say "  Otis.app が見つかりません。Applications に入れて開いてください。"
  fi
fi

# --- 3. Codex CLI -----------------------------------------------------------
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

# --- 4. companion 起動 ------------------------------------------------------
# 専用ウィンドウで開く: Chrome の --app モード（タブ・アドレスバー無しの単独
# ウィンドウ）。Chrome が無ければ既定ブラウザにフォールバック。
URL="http://127.0.0.1:${PORT}"
open_window() {
  local chrome="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
  if [ -x "$chrome" ]; then
    "$chrome" --app="$1" --new-window >/dev/null 2>&1 &
  else
    open "$1" >/dev/null 2>&1
  fi
}
say ""
say "AI チャットを専用ウィンドウで開きます（$URL）。"
say "・Audacity 側で『ようこそ』画面が出たら閉じてください。初回はプラグイン"
say "  スキャン中はチャットの接続表示が赤、完了すると緑になります。"
say "・チャット右上の『ChatGPT でログイン』でサインイン（API キー不要）。"
say "・停止するにはこのウィンドウで Ctrl+C。"
say ""
( sleep 2; open_window "$URL" ) &
python3 server.py
pause
