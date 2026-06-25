#!/bin/bash
# =============================================================================
# ステム分離（UVR / audio-separator）のセットアップ
#
# 「ボーカルとインストに分離して」等を AI チャットから使うための分離エンジンを
# 専用 venv (~/.audacity-mcp-companion/sep-venv) に導入します。約 1.1GB の
# ダウンロードがあり、初回のみ実行すれば OK です（チャット本体は無しでも動作し、
# 分離を頼まれたときだけこのエンジンを使います）。
# =============================================================================

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
VENV="$HOME/.audacity-mcp-companion/sep-venv"

say()   { printf '%s\n' "$*"; }
pause() { echo; read -r -n 1 -s -p "Enter キーで閉じます… "; echo; }

say "=== 音声AI（ステム分離・文字起こし）セットアップ ==="

if [ "$(uname -m)" != "arm64" ]; then
  say "✖ Apple Silicon (arm64) 専用です（現在: $(uname -m)）。"; pause; exit 1
fi

# Python 3.10+ を探す
PY=""
for p in python3.12 python3.11 python3.10; do
  if command -v "$p" >/dev/null 2>&1; then PY="$p"; break; fi
done
if [ -z "$PY" ]; then
  say "Python 3.10 以上が必要です。Homebrew で導入してください:"
  say "    brew install python@3.11"
  pause; exit 1
fi
say "使用する Python: $($PY --version 2>&1) ($PY)"

if [ -x "$VENV/bin/audio-separator" ] && [ -x "$VENV/bin/mlx_whisper" ]; then
  say "既に導入済みです（ステム分離・文字起こしの両方）。"
  say "再導入する場合はフォルダを削除してください: rm -rf \"$VENV\""
  pause; exit 0
fi

say "音声AIエンジンを $VENV に導入します（約 1.5GB・数分かかります）…"
say "  - audio-separator（UVR ステム分離）"
say "  - mlx-whisper（ローカル文字起こし・Apple GPU）"
[ -d "$VENV/bin" ] || "$PY" -m venv "$VENV" || { say "✖ venv の作成に失敗しました。"; pause; exit 1; }
"$VENV/bin/python" -m pip install --upgrade pip -q
if "$VENV/bin/pip" install "audio-separator[cpu]" mlx-whisper; then
  say ""
  say "✓ 完了。AI チャットで次のように頼めます:"
  say "  ・「この音声をボーカルとインストに分離して」"
  say "  ・「この音声を文字起こしして」"
  say "（初回の実行時に各モデルが自動ダウンロードされます。）"
else
  say "✖ 導入に失敗しました。ネットワークをご確認のうえ再実行してください。"
fi
pause
