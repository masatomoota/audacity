#!/bin/bash
# =============================================================================
# make-dmg.sh — package the patched Otis.app + MCP Companion into a
# distributable (unsigned / ad-hoc) .dmg.
#
#   ./scripts/make-dmg.sh
#
# Output: build/Otis-MCP.dmg
#
# This packages an AU-only / GPLv2 build. Configure Audacity beforehand with VST
# disabled so no VST3 SDK (GPLv3) ships:
#   cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#     -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
#     -Daudacity_has_vst3=Off -Daudacity_use_vst=Off -Daudacity_bundle_gplv3=Off
#   cmake --build build -j
# (Audio Unit support stays ON by default on macOS; it uses only the system
# AudioUnit framework, no third-party SDK.)
#
# Note: there is no Apple Developer ID on this machine, so the result is NOT
# notarized. Recipients must clear Gatekeeper quarantine on first launch
# (right-click -> Open, or `xattr -dr com.apple.quarantine`). See the in-DMG
# README. The companion folder excludes secrets (.env) and per-user state
# (.codex/, __pycache__, legacy/).
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$REPO/build/RelWithDebInfo/Audacity.app"
COMP="$REPO/mcp-companion"
STAGE="$REPO/build/dmg-stage"
DMG="$REPO/build/Otis-MCP.dmg"
VOL="Otis MCP"

[ -d "$APP" ] || { echo "ERROR: $APP not found — build Audacity first."; exit 1; }

echo "Staging in $STAGE …"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"

# 1. App (ditto preserves the bundle metadata + ad-hoc signature)
ditto "$APP" "$STAGE/Otis.app"

# 1a. Drop the dev "Portable Settings" config: it holds machine-specific module
# paths and personal state (macros, plugin registry), forces portable mode, and
# contains a non-ASCII filename that breaks the code seal on the HFS+ DMG.
# mod-mcp-server is auto-enabled in code (autoEnabledModules()), so the MCP
# server still starts on a fresh per-user profile.
rm -rf "$STAGE/Otis.app/Contents/Portable Settings"

# 1a-2. AU-only build (configured with -Daudacity_has_vst3=Off -Daudacity_use_vst=Off):
# remove any stale VST host dylibs an incremental build may have left behind, so
# no VST3 SDK (GPLv3) code ships. The app binary does not link these.
rm -f "$STAGE/Otis.app/Contents/Frameworks/"lib-vst*.dylib

# NOTE: do NOT `codesign --force --deep` the bundle. It produces a "valid"
# signature but re-signs the bundled plugin-scanner helper inconsistently, which
# makes the first launch on a fresh profile hang at 0% CPU (the scanner never
# returns). The build's own linker-signed ad-hoc signature runs correctly; since
# there is no Developer ID anyway, recipients clear Gatekeeper quarantine on
# first launch (`xattr -dr com.apple.quarantine`, see the in-DMG README).

# 2. Drag-to-install alias
ln -s /Applications "$STAGE/Applications"

# 3. MCP Companion (only the files needed to run; NO secrets / state)
DEST="$STAGE/MCP Companion"
mkdir -p "$DEST"
for f in server.py codex_bridge.py stem_mcp_server.py transcribe_mcp_server.py \
         index.html run.sh README.md \
         "セットアップして起動.command" "ステム分離セットアップ.command"; do
  cp "$COMP/$f" "$DEST/$f"
done
chmod +x "$DEST/セットアップして起動.command" "$DEST/ステム分離セットアップ.command" "$DEST/run.sh"

# 3b. One-click launcher at the DMG root that starts BOTH Audacity and the chat
# (it just delegates to the companion launcher, which now launches Audacity too).
LAUNCHER="$STAGE/Otis と AI チャットを起動.command"
cat > "$LAUNCHER" <<'LAUNCH'
#!/bin/bash
# Otis（MCP サーバ内蔵）と AI チャット(MCP Companion) を一緒に起動します。
exec "$(dirname "$0")/MCP Companion/セットアップして起動.command"
LAUNCH
chmod +x "$LAUNCHER"

# 4. Top-level read-me (Japanese)
cat > "$STAGE/はじめにお読みください.txt" <<'TXT'
Otis — 同梱物と使い方
================================================================

このディスクには次が入っています。
  • Otis.app                          … MCP サーバ内蔵の音声エディタ（Audacity 3.7 ベース）
  • MCP Companion フォルダ                … 言葉で Otis を操作する AI チャット（Codex 版）
  • 「Otis と AI チャットを起動.command」    … 両方をまとめて起動（おすすめ）

================================================================
かんたん起動（おすすめ）
================================================================
  ・先に Otis.app を Applications にドラッグし、初回だけ下のコマンドで
    Gatekeeper を解除しておきます:
        xattr -dr com.apple.quarantine /Applications/Otis.app
  ・あとは「Otis と AI チャットを起動.command」を右クリック →「開く」。
    Otis と AI チャットが一緒に立ち上がります（初回は Codex CLI を自動
    取得・約 249MB／ブラウザの「Log in with ChatGPT」でサインイン）。

  以下は個別の手順です。

----------------------------------------------------------------
1. Otis をインストール
----------------------------------------------------------------
  1) Otis.app を、左の「Applications」エイリアスにドラッグします。
  2) 初回起動: このアプリは未署名のため、Gatekeeper にブロックされます。
     ターミナルで次の 1 行を実行してから起動してください（確実な方法）:
         xattr -dr com.apple.quarantine /Applications/Otis.app
     （「右クリック → 開く」でも開ける場合がありますが、「壊れているため
       開けません」と表示されるときは上のコマンドを使ってください）
  3) 初回起動時はインストール済みプラグインのスキャンを行います。
     完了までしばらくお待ちください。
  4) 起動すると内蔵の MCP サーバが 127.0.0.1:4830 で自動的に立ち上がります。

----------------------------------------------------------------
2. チャット（MCP Companion）を使う
----------------------------------------------------------------
  1)「MCP Companion」フォルダを書き込み可能な場所（例: アプリケーション/
     ホーム）にコピーします（DMG から直接でも動きます）。
  2)「セットアップして起動.command」を「右クリック → 開く → 開く」で実行。
     ・初回は Codex CLI（OpenAI 製・署名済み）を自動ダウンロードします
       （約 249MB／2 回目以降は不要）。手動で入れる場合:
         brew install --cask codex
     ・Python 3 が無い場合は開発者ツールの導入を案内します。
  3) Chrome の --app モード（タブ・アドレスバー無しの専用ウィンドウ）で
     http://127.0.0.1:8765 が自動的に開きます。
  4) 画面右上の「Log in with ChatGPT」で ChatGPT アカウントにサインイン。
     （API キーは不要。会話は端末内に保存され、次回も続きから使えます）
  5)「440Hz のトーンを 3 秒作って半分の音量にして書き出して」のように
     話しかけると、Otis が動きます。

----------------------------------------------------------------
3. ステム分離・文字起こし（音声AI・任意）
----------------------------------------------------------------
  「ボーカルを抜いて」「この音声を文字起こしして」等を使うには、MCP Companion
  フォルダ内の「ステム分離セットアップ.command」を一度だけ実行して音声AIエンジン
  （UVR/audio-separator ＋ ローカル Whisper/mlx-whisper・約 1.5GB）を導入して
  ください。導入後はチャットで:
  ・「ボーカルとインストに分離して」→ 書き出し→分離→トラック取り込みまで自動
    （既定 Vocals/Instrumental、4 ステムは htdemucs を指定）。
  ・「この音声を文字起こしして」→ 書き出し→Whisper→全文＋タイムスタンプを返す
    （端末内で処理・APIキー不要）。

----------------------------------------------------------------
4. 対応フォーマット
----------------------------------------------------------------
  ・ffmpeg 不要で対応: WAV / AIFF / MP3 / FLAC / OGG Vorbis / Opus
  ・M4A / AAC / WMA / AC3 を扱いたい場合は Homebrew で ffmpeg を追加:
        brew install ffmpeg
    （ライセンス・配布サイズの観点から、ffmpeg は DMG に同梱していません）

----------------------------------------------------------------
動作要件・注意
----------------------------------------------------------------
  • Apple Silicon (arm64) の Mac 専用です。
  • このディスクは未署名（Apple の公証なし）です。配布元を信頼できる
    場合のみ、上記の方法で開いてください。
  • Otis は Audacity 3.7 のフォークです。"Audacity" は Muse Group の
    登録商標であり、Otis は Audacity プロジェクトと提携・公認の関係に
    はありません。
  • プラグイン対応: AU（Audio Unit）のみ。VST/VST3 は無効化しています。
    VST3 SDK（GPLv3）を含まないため、本ビルドは GPLv2-or-later で配布可能です
    （同梱「LICENSE (GPLv2).txt」）。GPL の義務に基づきソースは元リポジトリで
    提供されます。
TXT

# 4b. GPLv2 license text (AU-only build excludes the VST3 SDK → no GPLv3-only
# components → distributable under GPLv2-or-later).
GPLV2="$REPO/build/LICENSE-GPLv2.txt"
if [ ! -s "$GPLV2" ]; then
  curl -fsSL https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt -o "$GPLV2" \
    || echo "WARNING: could not fetch GPLv2 license text"
fi
[ -s "$GPLV2" ] && cp "$GPLV2" "$STAGE/LICENSE (GPLv2).txt"

# 5. Build the compressed DMG
echo "Creating $DMG …"
hdiutil create -volname "$VOL" -srcfolder "$STAGE" -fs HFS+ \
  -format UDZO -ov "$DMG" >/dev/null

SIZE="$(du -h "$DMG" | cut -f1)"
echo "Done: $DMG ($SIZE)"
