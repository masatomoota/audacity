# MCP_SESSION4_HANDOFF

このドキュメントは、別の LLM が `mcp-llm` ブランチの現在地点から Otis 開発を継続できるよう、
セッション 4 の成果・実証済み挙動・既知の地雷を引き継ぐためのものです。マーケティング文章は
含めず、ファイル参照とコマンドを厳密に書きます。

- ブランチ: `mcp-llm`
- 基底コミット: `5ad592b3c` (BUILD_HANDOFF baseline)
- 直近 HEAD: `6eeca817f`
- 対象ホスト: macOS Apple Silicon, AU(GPLv2) ビルドのみ

## 関連ドキュメント (重複させず、参照のみ)

- `MCP_LLM_CONTROL_HANDOFF.md` — Phase 0 設計理論
- `MCP_PHASE0_IMPLEMENTATION_HANDOFF.md` — Phase 0 実装
- `MCP_SESSION2_HANDOFF.md` — UI/Codex companion/stems/DMG
- `MCP_SESSION3_HANDOFF.md` — Audacity-core バグ監査 (32 source files / 30+ fixes)
- `BUILD_HANDOFF.md` — ビルド手順の正本 (clean build はこちらを参照)
- `OTIS_REBRAND_HANDOFF.md` — display-name アプローチと延期項目
- `OTIS_UI_COLOR_HANDOFF.md` — カラーパレットトークン

## 本セッションのコミット (`git log --oneline 5ad592b3c..HEAD`)

```
6eeca817f fix: IME-aware send, Chrome --app menu, stem/transcribe export validation, About credits, Japanese README
272136e76 ui: add "チャットを開く" (Open Chat) to the Tools menu
78ababe5c brand: Otis About logo, sidebar mark, no startup What's New panel
e36a256f9 ui: default to the flat modern Dark theme (was Light)
5627be18c brand: Otis cleanup — engine error strings, disable updates, drop Audacity4 promo
828a8bdd2 brand: apply Otis rebrand to the app + companion (display-name approach)
616ab50eb brand: name the app "Otis" — icon set, wordmark, rebrand/UI-color handoffs
```

## コミットごとの要点

### 616ab50eb — Otis 命名 + アイコン/ロックアップ生成

- `assets/otis/` 配下に `Otis.icns` / favicon セット / lockup SVG / wordmark / マーク
- 生成スクリプト: `assets/otis/generate_assets.py`, `assets/otis/build_assets.sh`
- リブランドとカラーの方針を `OTIS_REBRAND_HANDOFF.md` / `OTIS_UI_COLOR_HANDOFF.md` に記録

### 828a8bdd2 — display-name 方式でアプリと companion をリブランド

CMake の `TARGET="Audacity"` を改名するのはリスクが高すぎる (複数の hardcoded path と、
wx/conan の framework deploy が TARGET 名にキーされている — 詳細は
`OTIS_REBRAND_HANDOFF.md` の "Implementation status")。代替案として:

- `cmake-proxies/cmake-modules/MacOSXBundleInfo.plist.in`: `CFBundleName` と
  `CFBundleDisplayName` を "Otis" に
- `mac/Resources/Otis.icns` を bundle に同梱
- `src/AudacityApp.cpp` で `SetAppDisplayName("Otis")`
- アプリ内文字列を一括で "Otis" に置換 (`libraries/lib-files/FileException.cpp`,
  `libraries/lib-import-export/Import.cpp`, `libraries/lib-module-manager/ModuleManager.cpp`,
  `libraries/lib-project-file-io/ProjectFileIO.cpp`, `src/AudacityFileConfig.cpp`,
  `src/AudacityMirProject.cpp`, `src/AutoRecoveryDialog.cpp`, `src/Benchmark.cpp`,
  `src/CrashReport.cpp`, `src/LangChoice.cpp`, `src/MixerBoard.cpp`,
  `src/ProjectManager.cpp`, `src/ProjectWindowBase.cpp`, `src/TimerRecordDialog.cpp`,
  `src/menus/FileMenus.cpp`, `src/menus/HelpMenus.cpp`,
  `src/prefs/ApplicationPrefs.cpp`, `src/prefs/ImportExportPrefs.cpp`,
  `src/prefs/PrefsDialog.cpp`, `src/toolbars/ToolBar.cpp`,
  `src/update/CustomNotificationRegistry.cpp`)

ディスク上の bundle は `Audacity.app` のまま、表示名のみ "Otis"。

### 5627be18c — Otis cleanup

- エンジンエラー文字列の "Audacity" → "Otis" 置換
- `UpdateManager::Start` を無効化 (起動時にアップデートチェックさせない)
- `Audacity40PromoDialog` を撤去 (関連表示は `src/AudacityApp.cpp` から削除)

### e36a256f9 — Dark をデフォルトテーマに

- `libraries/lib-theme/Theme.cpp:1312` `defaultTheme = 2` (dark)。以前は 1 (light)。
- "Windows 95" 風の灰色ベベル付きツールバーが消える

### 78ababe5c — About ロゴ + サイドバーマーク + 起動時 What's New 抑止

- `images/OtisLogoWithName.xpm` を `assets/otis/otis-lockup-dark.svg` から生成
  (500x134、ダーク背景でページ bg と馴染ませる)
- `src/AboutDialog.cpp` で新 XPM を include、`AudacityLogoWithName_xpm` を差し替え
- `src/AudacityApp.cpp` で起動時の `WhatsNewDialog::Show` 呼び出しを抑制
- `mcp-companion/index.html` のサイドバー `<h1>` 内に Otis SVG マークをインライン

### 272136e76 — ツールメニューに "チャットを開く"

- `src/menus/PluginMenus.cpp` に `OnOpenChat` を追加
- メニューラベルは日本語 "チャットを開く"

### 6eeca817f — 本セッションの修正パック

特に重要な変更:

- `mcp-companion/index.html:450` 付近: `if (e.isComposing || e.keyCode === 229) return;`
  を Enter ハンドラに追加。日本語 IME 変換確定の Enter で送信されてしまう問題を抑止
- `src/menus/PluginMenus.cpp` `OnOpenChat`: Chrome **バイナリを直接** `--app=URL` で起動
  (`open -na "Google Chrome" --args ...` は既存 Chrome プロセスに統合され `--app` が落ちる)
- `mcp-companion/server.py` の `DEV_INSTRUCTIONS`: "Audacity" → "Otis"、
  stem/transcribe 両ワークフローで `Export2` の **前に必ず `SelectAll:`** を入れる手順を明文化
- `mcp-companion/stem_mcp_server.py` と `mcp-companion/transcribe_mcp_server.py`:
  `_validate_input_audio()` を追加。ファイルサイズ < 4KB か WAV(RIFF) / AIFF(FORM..AIFF) の
  magic に一致しない場合、日本語で「Otis が空のヘッダーだけを出力したので
  `SelectAll:` を実行してから再試行してください」というリトライヒントを返す
- `src/AboutDialog.cpp`: 個人クレジット約 107 行を削除し、"Built on Audacity(R)" の
  attribution を 1 行だけ残す
- `README.md`: macOS Apple Silicon に絞った日本語版に書き直し

## 実証済みのランタイム挙動 (次の LLM の再実証は不要)

### A. チャット経由のエンドツーエンドのステム分離

新スレッドで動作中の companion に対し、次のメッセージを送信:

> "今開いているオーディオのボーカルを分離して。返答は日本語で。"

SSE トレース (代表値):

```
+0s    meta {newThread:true}
+4.8s  assistant: "現在のプロジェクト全体を書き出してから...生成されたステムを Otis に読み込みます。"
+5.0s  audacity.run_command SelectAll:
+7.2s  audacity.run_command Export2: …/aud_stem_src.wav
+10.2s stem_separator.separate_stems
+68.3s stem_separator returns (model UVR-MDX-NET-Inst_HQ_3, ML 推論 58s)
+72.8s assistant: "分離が終わりました。伴奏とボーカルの2トラックを Otis に追加します。"
+72.8s audacity.run_command Import2: …/Instrumental.wav
+73.0s audacity.run_command Import2: …/Vocals.wav
+75.7s done
```

Otis トラック数: BEFORE = 1 (test_happiness) → AFTER = 3 (test_happiness,
`...(Instrumental)...`, `...(Vocals)...`)。エージェントの応答文中に "Audacity" の自己参照は
発生せず、すべて "Otis" として振る舞った。

### B. SelectAll 修正の効果

- 修正前: Selection が Start=0 End=0 で `Export2` が 410 byte の AIFF (ヘッダーのみ、
  "audio bytes: 0") を生成
- 修正後: Selection が Start=0 End=309.943 で `Export2` が 54MB AIFF (309.94 秒全長) を生成

テスト用ファイル (ディスク上に保持):

- `/tmp/aud_stem_src.wav` — 410 byte の壊れ検体
- `/tmp/otis_test1.wav` — 54MB の正常検体

### C. ステム入力バリデータ

```
_validate_input_audio('/tmp/aud_stem_src.wav') → 日本語の "Otis exported a header… run SelectAll:" メッセージ
_validate_input_audio('/tmp/otis_test1.wav')   → None (pass)
```

`.wav` 拡張子でも Audacity の `Export2` は AIFF を書くため、RIFF/WAVE と FORM/AIFF の
両 magic を受理する設計。

### D. Chrome `--app` メニュー

ツール → チャットを開く で、タブストリップ / URL バー / ブックマークなしの枠なし Chrome
ウィンドウが起動することを確認。

- 旧: `open -na "Google Chrome" --args --app=URL` (既存 Chrome に merge され `--app` が drop)
- 新: `/Applications/Google Chrome.app/Contents/MacOS/Google Chrome` をパスごとクオートで
  直接起動し `--app=URL` を渡す (wxExecute がスペースを 1 引数として保つ)

### E. IME ガード

`mcp-companion/index.html:450` のソースとライブで配信される HTML の両方で
`e.isComposing || e.keyCode === 229` のガードを確認済み。

## 未解決 / 要検討 (約束はしない)

- **ステムトラック名が長い・醜い**: 例 `aud_stem_src_(Vocals)_UVR-MDX-NET-Inst_HQ_3`。
  対応案 (1) `Import2` の後に Audacity scripting でリネーム
  (2) `stem_separator` の出力ファイル名を綺麗に
- **古いチャットスレッドが "Audacity..." を残す**: CODEX_HOME 配下に旧 `DEV_INSTRUCTIONS`
  のコンテキストが永続化されているため。リブランドが効くのは **新規スレッドのみ**。
  対策案: thread-prompt のバージョンを bump して stale prompt を無効化。ユーザー向けに
  ドキュメント化が必要。
- **1024–1680px ラップトップ幅でのレスポンシブ検証**: 本セッションでは未実施
  (エンドツーエンドのステムテストを優先したため中断)。companion は固定 260px サイドバー +
  flex メイン。spot-check では破綻なし、ただし系統的監査は open work。
- **真の Otis.app バンドル化** (display name でなくバンドル名そのもの) は未着手。
  `OTIS_REBRAND_HANDOFF.md` "Implementation status" 参照。CMake TARGET 改名 + 多数の
  hardcoded path 修正 + wx/conan framework deploy 連動が必要。
- **AU4 (Qt/QML) のアクセントカラー変更**: `OTIS_UI_COLOR_HANDOFF.md` §B。この Mac では
  AU4 はビルドされていない。
- **mcp-companion サイドバーに古いスレッド履歴**: クレジット削除を前段で実施したが、
  Codex thread storage に残る古いチャット履歴内の "Audacity" 自己参照は scrub されない。
  新規スレッドはクリーン。

## 再現手順

### 1. インクリメンタルビルド (macOS Apple Silicon, AU-only / GPLv2)

```
cmake --build /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build -j
```

出力: `/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build/RelWithDebInfo/Audacity.app`
(display name は "Otis")。クリーンビルドは `BUILD_HANDOFF.md` §3 を参照。

### 2. Otis 起動

```
open /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build/RelWithDebInfo/Audacity.app
```

MCP サーバーは `http://127.0.0.1:4830/mcp` で自動 listen。クラッシュリカバリダイアログが
起動を阻害する場合:

```
killall Audacity
mv ~/Library/Application\ Support/audacity/SessionData ~/Library/Application\ Support/audacity/SessionData.bak
```

### 3. companion 起動

```
cd /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/mcp-companion && ./run.sh
```

ポート 8765、`/api/chat` は SSE。

Otis + companion を一緒に立ち上げる (Chrome `--app`):

```
/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/mcp-companion/セットアップして起動.command
```

または Otis のメニュー: ツール → チャットを開く (companion が 8765 で稼働している前提)。

### 4. エンドツーエンドのステム動作確認

1. `/tmp/test_happiness.mp3` (ASCII のみの path) を Otis にドロップ。
   **注意**: スペースや非 ASCII を含む path で `Import2` してはいけない (後述の地雷参照)
2. チャットに送信: "今開いているオーディオのボーカルを分離して"
3. 期待挙動: `SelectAll` → `Export2` → `separate_stems` (約 1 分) → `Import2` × 2 → 計 3 トラック

### 5. MCP スモークテスト (チャットなし)

```
curl -s http://127.0.0.1:4830/mcp -H 'content-type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}'
```

`run_command` と `get_info` の 2 ツールのみ返る。`stem_separator` と `transcribe` は別の
stdio MCP サーバー (CODEX_HOME 内 Codex 設定で登録) でエージェントだけが見える層に存在し、
このレイヤーには **意図的に** 出していない。

## 既知の地雷

- **Audacity scripting の `Import2`/`Export2` macro パーサーは Filename= の値を空白で
  分割する**。スペースを含む path や (ロケールによっては) 非 ASCII を含む path を渡すと
  切り捨てられた path をフォルダとして再帰インポートし、暴走する事象がこのセッションで
  発生した。対策: 必ず ASCII-only 名で `/tmp` にコピーしてから渡す。
- **`Export2` は `.wav` 拡張子でも AIFF を書く**。stem/transcribe 側のバリデータが
  RIFF/WAVE と FORM/AIFF の両方を受理するのはこのため。
- **`Export2` は TIME SELECTION をエクスポートする**。Selection が空 (Start==End) だと
  410 byte のヘッダーのみのファイルが出る。範囲指定の意図がない限り、**必ず `Export2` の
  前に `SelectAll:`** を実行する。
- **macOS のクラッシュリカバリダイアログは legacy wx ビルドだと脆い**。クリックは +12px
  Y オフセットが必要なことがあり、`S` キーショートカットでは閉じない。回避: Otis を kill
  → SessionData を退避 → 再起動 (再現手順 2 参照)。
- **`open -na "Google Chrome" --args --app=URL` は Chrome が既に起動中だと `--app` を渡せ
  ない**。バイナリパス直接起動を使う。
- **`src/AboutDialog.cpp:485` のページ背景色はロゴ (1,1) ピクセルからサンプリングして
  いる**。ロゴを差し替えるなら canvas を暗色にしないと白枠が出る。

## ベースライン (5ad592b3c) からの差分一覧

`git diff --stat 5ad592b3c..HEAD` の出力:

```
 OTIS_REBRAND_HANDOFF.md                            | 160 +++++++++++
 OTIS_UI_COLOR_HANDOFF.md                           | 106 +++++++
 README.md                                          | 319 +++++++++++++++++++--
 assets/otis/Otis.icns                              | Bin 0 -> 181698 bytes
 assets/otis/apple-touch-icon.png                   | Bin 0 -> 6575 bytes
 assets/otis/build_assets.sh                        |  25 ++
 assets/otis/favicon-16.png                         | Bin 0 -> 536 bytes
 assets/otis/favicon-32.png                         | Bin 0 -> 1161 bytes
 assets/otis/favicon-48.png                         | Bin 0 -> 1719 bytes
 assets/otis/favicon-64.png                         | Bin 0 -> 2289 bytes
 assets/otis/favicon.ico                            | Bin 0 -> 32038 bytes
 assets/otis/favicon.svg                            |  16 ++
 assets/otis/generate_assets.py                     | 137 +++++++++
 assets/otis/icon-1024.png                          | Bin 0 -> 55512 bytes
 assets/otis/icon-192.png                           | Bin 0 -> 7076 bytes
 assets/otis/icon-512.png                           | Bin 0 -> 19276 bytes
 assets/otis/otis-icon-rounded.svg                  |  17 ++
 assets/otis/otis-icon-tile.svg                     |  17 ++
 assets/otis/otis-lockup-dark.svg                   |  17 ++
 assets/otis/otis-lockup-light.svg                  |  17 ++
 assets/otis/otis-mark.svg                          |  16 ++
 assets/otis/otis-wordmark-dark.svg                 |   3 +
 assets/otis/otis-wordmark-light.svg                |   3 +
 assets/otis/site.webmanifest                       |  12 +
 au4/src/appshell/internal/applicationactioncontroller.cpp |   4 +-
 au4/src/appshell/qml/AboutDialog.qml               |   2 +-
 au4/src/appshell/view/mainwindowtitleprovider.cpp  |   2 +-
 au4/src/project/internal/projectuiactions.cpp      |   4 +-
 cmake-proxies/cmake-modules/MacOSXBundleInfo.plist.in     |  12 +-
 images/OtisLogoWithName.xpm                        | 272 ++++++++++++++++++
 libraries/lib-files/FileException.cpp              |   8 +-
 libraries/lib-import-export/Import.cpp             |   2 +-
 libraries/lib-module-manager/ModuleManager.cpp     |   2 +-
 libraries/lib-project-file-io/ProjectFileIO.cpp    |   6 +-
 libraries/lib-theme/Theme.cpp                      |  17 +-
 mac/Install.txt                                    |   8 +-
 mac/Resources/Otis.icns                            | Bin 0 -> 181698 bytes
 mcp-companion/README.md                            |  28 +-
 mcp-companion/index.html                           |  26 +-
 mcp-companion/server.py                            |  34 ++-
 mcp-companion/stem_mcp_server.py                   |  34 +++
 mcp-companion/transcribe_mcp_server.py             |  28 ++
 "mcp-companion/セットアップして起動.command"        |  21 +-
 scripts/make-dmg.sh                                |  22 +-
 src/AboutDialog.cpp                                | 158 +---------
 src/AudacityApp.cpp                                |  41 +--
 src/AudacityFileConfig.cpp                         |   6 +-
 src/AudacityMirProject.cpp                         |   4 +-
 src/AutoRecoveryDialog.cpp                         |   4 +-
 src/Benchmark.cpp                                  |   2 +-
 src/CMakeLists.txt                                 |   2 +-
 src/CrashReport.cpp                                |   8 +-
 src/LangChoice.cpp                                 |   4 +-
 src/MixerBoard.cpp                                 |   2 +-
 src/ProjectManager.cpp                             |   2 +-
 src/ProjectWindowBase.cpp                          |   2 +-
 src/TimerRecordDialog.cpp                          |  10 +-
 src/WhatsNewDialog.cpp                             |   2 +-
 src/menus/FileMenus.cpp                            |   2 +-
 src/menus/HelpMenus.cpp                            |   4 +-
 src/menus/PluginMenus.cpp                          |  52 +++-
 src/prefs/ApplicationPrefs.cpp                     |   2 +-
 src/prefs/ImportExportPrefs.cpp                    |   4 +-
 src/prefs/PrefsDialog.cpp                          |   2 +-
 src/toolbars/ToolBar.cpp                           |   2 +-
 src/update/CustomNotificationRegistry.cpp          |  17 +-
 win/Inno_Setup_Wizard/audacity.iss.in              |   4 +-
 win/audacity.rc                                    |   8 +-
 68 files changed, 1416 insertions(+), 325 deletions(-)
```

## 現時点での作業ツリー状態

`git status --short` はクリーン (本セッション末時点で uncommitted な変更なし。
`6eeca817f` までがコミット済み)。

## 次の LLM への推奨優先順位 (約束ではなく提案)

1. ステム/トランスクライブのトラック命名整理 (Audacity scripting `SetTrackInfo` で
   `Import2` 後にリネームするのが最小侵襲)
2. 古いチャットスレッド問題: `DEV_INSTRUCTIONS` のバージョントークンを埋め込み、
   バージョンが古いスレッドを新規 thread に誘導する UI フラグを `mcp-companion/index.html`
   に追加
3. レスポンシブ監査: 1024 / 1280 / 1440 / 1680 px で companion を spot-check し、
   サイドバーが折り畳めるか、入力欄が下に押し出されないかを確認
4. 真の Otis.app バンドル化は `OTIS_REBRAND_HANDOFF.md` の "Implementation status" を
   完全に読み、CMake TARGET 改名と framework deploy の整合性を整えてから着手
5. AU4 アクセントカラー (`OTIS_UI_COLOR_HANDOFF.md` §B) — ただし AU4 のビルドが必要

## 参考: コミット間で意味的に重要なファイル位置

- `libraries/lib-theme/Theme.cpp:1312` — defaultTheme = 2 (dark)
- `src/AboutDialog.cpp:485` — page bg を logo 1,1 から sample
- `mcp-companion/index.html:450` 付近 — IME ガード
- `src/menus/PluginMenus.cpp` — `OnOpenChat` (Chrome バイナリ直接起動)
- `mcp-companion/server.py` — `DEV_INSTRUCTIONS` (SelectAll を前置するワークフロー)
- `mcp-companion/stem_mcp_server.py` / `transcribe_mcp_server.py` — `_validate_input_audio()`
- `src/AudacityApp.cpp` — `SetAppDisplayName("Otis")`、WhatsNewDialog 起動抑止
- `cmake-proxies/cmake-modules/MacOSXBundleInfo.plist.in` — CFBundleName/CFBundleDisplayName

以上。
