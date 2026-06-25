#!/bin/bash
# =============================================================================
# make-dmg.sh — package the patched Audacity.app + MCP Companion into a
# distributable (unsigned / ad-hoc) .dmg.
#
#   ./scripts/make-dmg.sh
#
# Output: build/Audacity-MCP.dmg
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
DMG="$REPO/build/Audacity-MCP.dmg"
VOL="Audacity MCP"

[ -d "$APP" ] || { echo "ERROR: $APP not found — build Audacity first."; exit 1; }

echo "Staging in $STAGE …"
rm -rf "$STAGE" "$DMG"
mkdir -p "$STAGE"

# 1. App (ditto preserves the bundle metadata + ad-hoc signature)
ditto "$APP" "$STAGE/Audacity.app"

# 1a. Drop the dev "Portable Settings" config: it holds machine-specific module
# paths and personal state (macros, plugin registry), forces portable mode, and
# contains a non-ASCII filename that breaks the code seal on the HFS+ DMG.
# mod-mcp-server is auto-enabled in code (autoEnabledModules()), so the MCP
# server still starts on a fresh per-user profile.
rm -rf "$STAGE/Audacity.app/Contents/Portable Settings"

# 1a-2. AU-only build (configured with -Daudacity_has_vst3=Off -Daudacity_use_vst=Off):
# remove any stale VST host dylibs an incremental build may have left behind, so
# no VST3 SDK (GPLv3) code ships. The app binary does not link these.
rm -f "$STAGE/Audacity.app/Contents/Frameworks/"lib-vst*.dylib

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
for f in server.py codex_bridge.py index.html run.sh README.md "セットアップして起動.command"; do
  cp "$COMP/$f" "$DEST/$f"
done
chmod +x "$DEST/セットアップして起動.command" "$DEST/run.sh"

# 3b. One-click launcher at the DMG root that starts BOTH Audacity and the chat
# (it just delegates to the companion launcher, which now launches Audacity too).
LAUNCHER="$STAGE/Audacity と AI チャットを起動.command"
cat > "$LAUNCHER" <<'LAUNCH'
#!/bin/bash
# Audacity（MCP サーバ内蔵）と AI チャット(MCP Companion) を一緒に起動します。
exec "$(dirname "$0")/MCP Companion/セットアップして起動.command"
LAUNCH
chmod +x "$LAUNCHER"

# 4. Top-level read-me (Japanese)
cat > "$STAGE/はじめにお読みください.txt" <<'TXT'
Audacity MCP — 同梱物と使い方
================================================================

このディスクには次が入っています。
  • Audacity.app                         … MCP サーバ内蔵の Audacity（このフォーク）
  • MCP Companion フォルダ                … 言葉で Audacity を操作するチャット（Codex 版）
  • 「Audacity と AI チャットを起動.command」… 両方をまとめて起動（おすすめ）

================================================================
かんたん起動（おすすめ）
================================================================
  ・先に Audacity.app を Applications にドラッグし、初回だけ下のコマンドで
    Gatekeeper を解除しておきます:
        xattr -dr com.apple.quarantine /Applications/Audacity.app
  ・あとは「Audacity と AI チャットを起動.command」を右クリック →「開く」。
    Audacity と AI チャットが一緒に立ち上がります（初回は Codex CLI を自動
    取得・約 249MB／ブラウザの「Log in with ChatGPT」でサインイン）。

  以下は個別の手順です。

----------------------------------------------------------------
1. Audacity をインストール
----------------------------------------------------------------
  1) Audacity.app を、左の「Applications」エイリアスにドラッグします。
  2) 初回起動: このアプリは未署名のため、Gatekeeper にブロックされます。
     ターミナルで次の 1 行を実行してから起動してください（確実な方法）:
         xattr -dr com.apple.quarantine /Applications/Audacity.app
     （「右クリック → 開く」でも開ける場合がありますが、「壊れているため
       開けません」と表示されるときは上のコマンドを使ってください）
  3) 初回起動時はインストール済みプラグインのスキャンと、ようこそ画面が
     表示されます。ようこそ画面を閉じ、スキャン完了までお待ちください。
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
  3) 自動でブラウザが http://127.0.0.1:8765 を開きます。
  4) 画面右上の「Log in with ChatGPT」で ChatGPT アカウントにサインイン。
     （API キーは不要。会話は端末内に保存され、次回も続きから使えます）
  5)「440Hz のトーンを 3 秒作って半分の音量にして書き出して」のように
     話しかけると、Audacity が動きます。

----------------------------------------------------------------
動作要件・注意
----------------------------------------------------------------
  • Apple Silicon (arm64) の Mac 専用です。
  • このディスクは未署名（Apple の公証なし）です。配布元を信頼できる
    場合のみ、上記の方法で開いてください。
  • "Audacity" は Muse Group の登録商標です。広く再配布する場合は名称・
    アイコンのリブランドが必要です。
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
