# Audacity MCP — Session 2 ハンドオフ（**UI 修正 / コンパニオン再構築 / ステム分離 / 配布 DMG**）

> **ABSTRACT (English, for any LLM picking this up cold):**
> This document covers the work done **after** `MCP_PHASE0_IMPLEMENTATION_HANDOFF.md` (§11 "次の LLM へ") and `MCP_LLM_CONTROL_HANDOFF.md`.  Those two files describe Phase 0 (the `mod-mcp-server` module, perception commands, and the original API-key companion).  This document picks up from commit `9dae50214` (true-peak / ST-M loudness, the last commit documented in the Phase 0 handoff) and covers everything up to the HEAD of `mcp-llm` at time of writing.  Prose is Japanese; all paths, identifiers, commands, and code are English with `file:line` citations verified against the live repo.  Start at §1.

---

## 0. 前のハンドオフとの関係

| ファイル | 何をカバーするか |
|---|---|
| `MCP_LLM_CONTROL_HANDOFF.md` | 設計意思決定・Audacity 3.x 選定の根拠・ロードマップ（Phase 0 以前） |
| `MCP_PHASE0_IMPLEMENTATION_HANDOFF.md` | Phase 0 実装完了（mod-mcp-server・知覚コマンド・Codex app-server 版コンパニオン）まで |
| **本ドキュメント（MCP_SESSION2_HANDOFF.md）** | commit `b77092cca` 以降の全作業（UI 修正 3 件・コンパニオン再構築・ステム分離・DMG 配布・専用ウィンドウ） |

---

## 1. コミット一覧（新しい順）

| コミット | 日時 | 内容 |
|---|---|---|
| `976c718b7` | 2026-06-26 | assets: コンパニオン用アイコン / favicon 概念セット（WIP） |
| `e6da02308` | 2026-06-26 | mcp-companion: UVR ステム分離 + 完全日本語 UI + 専用ウィンドウ |
| `898449816` | 2026-06-26 | dist: Audacity + AI チャットをワンクリックで起動するランチャー |
| `dd3452591` | 2026-06-26 | dist: AU-only / GPLv2 DMG（VST3 SDK 除去・GPLv2 ライセンス同梱） |
| `a5d4fb8f6` | 2026-06-25 | dist: Audacity + MCP Companion を配布可能な DMG にパッケージ |
| `2f8a69c05` | 2026-06-25 | ui: クリップタイトルをアフォーダンスバーで光学的に垂直センタリング |
| `0f7b64eb7` | 2026-06-25 | ui: トラックタイトルテキストをタイトルバーで光学的に垂直センタリング |
| `fd1eee50c` | 2026-06-25 | handoff: Codex app-server コンパニオンを Phase 0 ハンドオフの §9 に記録 |
| `5c52b27ee` | 2026-06-25 | mcp-companion: Codex app server へ再構築 |
| `b77092cca` | 2026-06-25 | ui: クリア HiDPI トラックテキスト + 言語独立コロン形式タイムコード |

---

## 2. UI 修正（Audacity コア、コミット `b77092cca`・`0f7b64eb7`・`2f8a69c05`）

### 2.1 HiDPI（Retina）トラックテキストのクリア化

**問題**: TrackPanel 全体が **1x の `wxBitmap` バックバッファ**に描画され、それが Retina の 2x バッキングストアへ拡大されていた。DrawText で描いたトラック名・クリップタイトルが 2x 表示でぼやける。

**修正ファイル**: `src/widgets/BackedPanel.cpp`

- `ResizeBacking()`（line 48–63）: `GetContentScaleFactor()` を取得し、`mBacking->CreateScaled(w, h, 24, scale)` でデバイス解像度のビットマップを作成する。コメント（line 59–62）に「1x バッキングが Retina バッキングストアに引き伸ばされてテキストがぼやける」問題の説明がある。
- `DisplayBitmap()`（line 71–80）: `mBacking->GetScaledSize()` で論理サイズを取得し、それで `RepairBitmap` を呼ぶ（1:1 コピー。物理→論理のマッピングは DC が担う）。

```cpp
// BackedPanel.cpp:62
mBacking->CreateScaled(std::max(sz.x,1), std::max(sz.y,1), 24, scale);
// BackedPanel.cpp:78-79
const wxSize logical = mBacking->GetScaledSize();
RepairBitmap(dc, 0, 0, logical.GetWidth(), logical.GetHeight());
```

**Windows への影響**: `GetContentScaleFactor()` は Windows で通常 1.0 を返す（DPI-aware マニフェストが無ければ）。よって Windows ビルドでは実質 no-op — 動作変更なし。将来 Windows DPI-aware にする場合は `#if __WXMSW__` ガードを検討すべき。

### 2.2 言語独立コロン形式タイムコード

**問題**: `hh:mm:ss` 系の時刻表示フォーマット文字列が翻訳可能な 'h' / 'm' / 's' デリミタを使っており、日本語ロケールで `00時間00分00秒` のように表示されていた（他の DAW と見た目が異なる）。

**修正ファイル**: `libraries/lib-numeric-formats/formatters/ParsedNumericConverterFormatter.cpp`

`TimeConverterFormats_[]`（line 712）の `hh:mm:ss` 系エントリを DAW スタイルのコロン区切り書式文字列に変更した：

| フォーマット | 変更後の書式文字列 | 表示例 |
|---|---|---|
| `hh:mm:ss` | `XO("0100:060:060")` | `00:00:00` |
| `hh:mm:ss + hundredths` | `{ XO("0100:060:060>0100"), XO("centiseconds") }` | `00:00:00.00` |
| `hh:mm:ss + milliseconds` | `{ XO("0100:060:060>01000"), XO("milliseconds") }` | `00:00:00.000` |
| `dd:hh:mm:ss` | `XO("0100:024:060:060")` | `00:00:00:00` |

`:` はパーサで「区切り文字」として扱われ翻訳対象にならないため、どの言語でも同じ表示になる。`>` は小数点（`.` として描画）。内部フォーマット名（`NumericConverterFormats::HoursMinsSecondsFormat()` 等）は変更していないため、保存済みプリファレンスは互換性を保つ。

### 2.3 トラックタイトルの垂直光学センタリング（コミット `0f7b64eb7`）

**問題**: トラック名を 16px のタイトル行に描画する際、フルのエムボックス（アセント＋ディセント）で垂直中央計算していた。典型的なトラック名（CJK 含む）はディセンダーがないため、ディセント分（約 3px）だけ上に浮いて見える。HiDPI 化でテキストがシャープになったことでより目立つようになった。

**修正ファイル**: `src/tracks/ui/CommonTrackInfo.cpp`

`CloseTitleDrawFunction()`（line 219–254）を修正し、`DrawText` の Y 座標を `bev.y + (bev.height - metrics.ascent) / 2` で計算（アセントだけを使い、空のディセント部分を除外）。

```cpp
// CommonTrackInfo.cpp:254
dc->DrawText(titleStr, bev.x + 2, bev.y + (bev.height - metrics.ascent) / 2);
```

コメント（line 249–253）に「ディセントは典型的なトラック名では空であり、エムボックス全体を使うと字形が上に座って見える」旨の説明がある。

### 2.4 クリップタイトルの垂直光学センタリング（コミット `2f8a69c05`）

**問題**: `DrawClipTitle()` が `DrawLabel(wxALIGN_CENTER_VERTICAL)` でアフォーダンスバー（18px）の中央に描いていたが、同様に空のディセントで上に浮いて見えた。

**修正ファイル**: `src/TrackArt.cpp`

`DrawClipTitle()`（line 289–311）を修正。`DrawLabel` を廃止して `dc.DrawText` を使い、Y 座標を `titleRect.GetTop() + (titleRect.GetHeight() - metrics.ascent) / 2` で計算する。`titleRect` は既に水平位置が確定しているため、水平アライメントは維持される。

```cpp
// TrackArt.cpp:308-310
dc.DrawText(
   truncatedTitle, titleRect.GetLeft(),
   titleRect.GetTop() + (titleRect.GetHeight() - metrics.ascent) / 2);
```

---

## 3. コンパニオン再構築（Codex app-server ベース）（コミット `5c52b27ee`）

### 3.1 アーキテクチャの変更点

旧実装（`mcp-companion/legacy/` に温存）は OpenAI / Anthropic API キー直叩きのエージェントループだった。新実装は **Codex CLI の `app-server` モード**を使う：

```
Browser(index.html) ⇄ HTTP/SSE ⇄ server.py ⇄ JSON-RPC/stdio ⇄ codex app-server
                                                 ⇄ MCP(streamable HTTP) ⇄ Audacity mod-mcp-server(:4830)
```

これにより (1) **ChatGPT Web ログイン（API キー不要）** と (2) **会話メモリの永続化**（Codex スレッドとして disk 保存・再開可能）が得られる。

### 3.2 `codex_bridge.py`

`mcp-companion/codex_bridge.py` — `codex app-server` サブプロセスを 1 本所有する JSON-RPC 2.0（stdio, NDJSON）クライアント。stdlib のみ（pip 不要）。

主要クラス `CodexAppServer`：
- `start()`: `CODEX_HOME` 環境変数付きで `codex app-server` を `subprocess.Popen` し、`initialize` ハンドシェイクを行う（line 91–125）。
- `request(method, params, timeout=120)`: 同期リクエスト（`threading.Event` でブロック）（line 156–180）。
- `notify(method, params)`: 応答不要の通知送信（line 182–186）。
- `subscribe(thread_id) / unsubscribe(thread_id, q)`: SSE チャットストリームのスレッド別購読キュー（line 200–220）。
- `_handle_server_request()`: サーバ→クライアントの承認リクエストに自動応答（`decision: acceptForSession`）（line 304–317）。認可ポリシーが `never` なので通常は発火しないが、デッドロック防止のため防御的に実装。
- `_handle_notification()`: `account/login/completed` を捕捉し `_login_events` に記録（line 319–342）。

### 3.3 `server.py` の主要設計

`mcp-companion/server.py` — `127.0.0.1:8765` の HTTP/SSE サーバ（`http.server.BaseHTTPRequestHandler` ベース、stdlib のみ）。

**隔離 CODEX_HOME**: 既定 `~/.audacity-mcp-companion`（`CODEX_HOME_COMPANION` 環境変数で上書き可）。ユーザのグローバル MCP サーバが混入しない。

```python
# server.py:86-87
CODEX_HOME = Path(os.environ.get(
    "CODEX_HOME_COMPANION", str(Path.home() / ".audacity-mcp-companion"))).resolve()
```

**`setup_codex_home()`**（line 164–218）: 起動時に `config.toml` を生成。生成内容：
```toml
approval_policy = "never"
sandbox_mode = "danger-full-access"

[mcp_servers.audacity]
url = "http://127.0.0.1:4830/mcp"

[mcp_servers.stem_separator]
command = "<sys.executable>"
args = ["<path>/stem_mcp_server.py"]
```

**重要な落とし穴（sandbox）**: `sandbox_mode = "danger-full-access"` が必須。Codex の制限サンドボックス（`read-only` / `workspace-write`）では、エージェントの MCP-over-HTTP 呼び出し（`localhost:4830`）がブロックされてツールが空 / "rejected" になる（line 185–190 のコメント参照）。この companion はローカルホスト専用・ユーザ駆動・开发者指示でシェル/ファイル操作を禁止しているため full-access を許容。`CODEX_SANDBOX` 環境変数で変更可。

**グローバル認証の継承**（line 169–181）: `~/.codex/auth.json` が存在すれば `CODEX_HOME/auth.json` にコピーし、再ログイン不要にする。

**API ルート**:

| メソッド | パス | 機能 |
|---|---|---|
| GET | `/api/status` | Codex bridge / Audacity MCP / アカウント状態 |
| GET | `/api/threads` | Codex スレッド一覧 |
| GET | `/api/thread?id=...` | 特定スレッドの全メッセージ取得 |
| GET | `/api/login/wait` | ログイン完了待ち（ポーリング代替） |
| POST | `/api/chat` | SSE チャットストリーム（`delta`/`tool_start`/`tool_end`/`message`/`done`/`error` イベント） |
| POST | `/api/login` | ChatGPT ログイン開始 |
| POST | `/api/logout` | ログアウト |
| POST | `/api/interrupt` | 実行中ターンの中断 |

**DEV_INSTRUCTIONS**（line 96–128）: 完全日本語。エージェントに「返答は日本語で」「コードやシェルを実行しない」「破壊的操作前は確認する」「解析コマンドで数値を測ってから判断する」を指示。ステム分離ワークフロー（Export2 → separate_stems → Import2）も記載（`separate_stems` ツールは stem_mcp_server.py が提供）。

---

## 4. ステム分離（UVR レベル）、AI チャット駆動（コミット `e6da02308`）

### 4.1 エンジン概要

- ライブラリ: **audio-separator**（pip）。MPS（Apple Silicon GPU）＋ ONNX ランタイムに対応。
- 分離専用 venv: `~/.audacity-mcp-companion/sep-venv`（Python 3.10+。`ステム分離セットアップ.command` で作成）。Audacity 本体・companion の Python 環境とは完全に分離。
- デフォルトモデル: `UVR-MDX-NET-Inst_HQ_3.onnx`（Vocals / Instrumental の 2 ステム、高品質 MDX-Net）。4 ステム（vocals/drums/bass/other）は `htdemucs.yaml`（Demucs、torch 必要）。

### 4.2 `stem_mcp_server.py`

`mcp-companion/stem_mcp_server.py` — stdio の MCP サーバ。Codex エージェントに `separate_stems` と `list_stem_models` ツールを提供。

定数（line 26–34）:
```python
SUPPORT  = ~/.audacity-mcp-companion   # AUDACITY_COMPANION_HOME で上書き可
VENV     = SUPPORT / "sep-venv"         # SEP_VENV で上書き可
AUDIO_SEP = VENV / "bin" / "audio-separator"
DEFAULT_MODEL = "UVR-MDX-NET-Inst_HQ_3.onnx"  # SEP_DEFAULT_MODEL で上書き可
```

`do_separate(args)`（line 90–133）:
1. `input_path` の存在を確認。
2. `AUDIO_SEP` が無ければ「venv 未構築」のセットアップヒントをエラーとして返す（line 96–98）。Codex ターンはクラッシュしない。
3. `audio-separator <input> --model_filename <model> --output_dir <outdir> --output_format WAV` を実行（timeout 3600s）（line 104–111）。
4. 生成されたステムファイルパスを収集し、`{"content": [...], "structuredContent": {"model": ..., "stems": [...]}}` で返す（line 128–133）。

`do_list_models()`（line 136–139）: `RECOMMENDED` リストのテキスト説明を返す。

`main()`（line 162–191）: stdin から NDJSON を読み、`handle()` で MCP メソッドを dispatch し、stdout に結果を書く標準的な stdio MCP ループ。

### 4.3 `server.py` への登録

`setup_codex_home()` が `config.toml` に `[mcp_servers.stem_separator]` を追記する（line 210–217）。`stem_mcp_server.py` が存在しない場合はこのセクションを省略（graceful degradation）。

```python
# server.py:213-216
lines += [
    "[mcp_servers.stem_separator]",
    f'command = "{sys.executable}"',
    f'args = ["{stem_server}"]',
```

### 4.4 エンド・ツー・エンド検証済みワークフロー

チャットで「このトラックをボーカルとインストに分けて」のように指示すると、Codex エージェントは以下を実行する（DEV_INSTRUCTIONS に記載）:
1. `run_command "Export2: Filename=/tmp/aud_stem_src.wav"` でプロジェクトを WAV 書き出し。
2. `separate_stems(input_path="/tmp/aud_stem_src.wav")` でステム分離。
3. 返ってきた各ステムパスを `run_command "Import2: Filename=..."` で Audacity に取り込み。
4. 結果（3 トラック構成など）を日本語で報告。

実機で e2e 検証済み（Export2 → separate_stems → Import2 × 2 → Audacity に 3 トラック）。

### 4.5 ステム分離 venv のセットアップ方法

```bash
# DMG 同梱の "ステム分離セットアップ.command" をダブルクリック、または手動:
python3.11 -m venv ~/.audacity-mcp-companion/sep-venv
~/.audacity-mcp-companion/sep-venv/bin/pip install "audio-separator[cpu]"
```

初回分離時にモデルが自動ダウンロードされる（`~/.audacity-mcp-companion/models/`）。

---

## 5. 完全日本語 UI とバグ修正（コミット `e6da02308`）

### 5.1 全面日本語化

`mcp-companion/index.html` の全ユーザ向け文字列を日本語に翻訳。エージェントの返答言語も日本語（DEV_INSTRUCTIONS で指定）。

### 5.2 カーリークォートバグ（重大な落とし穴）

日本語翻訳の際に、`textarea` / `button` 要素の HTML 属性にカーリークォート（U+201C/U+201D、いわゆる "typographic quotes"）が混入した。

```html
<!-- 誤り（バグ） — id属性の値がパーサに認識されない -->
<textarea id="input" ...>  ← ここでカーリークォートが使われた場合
```

`document.querySelector("#input")` が `null` を返し、JS 初期化時に例外が発生してページが真っ白になった。修正: HTML 属性には必ず ASCII ストレートクォート（`"` U+0022）を使う。

**今後の編集における注意**: `index.html` を編集する際は属性値に `"` (U+0022) のみを使うこと。日本語テキストをエディタや AI が補完する際にカーリークォートに自動変換されることがある。

---

## 6. 専用ウィンドウランチャー（コミット `e6da02308`・`898449816`）

`mcp-companion/セットアップして起動.command` がシェルスクリプトのダブルクリックランチャー。

### 6.1 起動順序

1. **アーキテクチャ確認**: `uname -m` が `arm64` でなければ終了（Apple Silicon 専用ビルド）。
2. **Python 3 確認**: なければ `xcode-select --install` を促して終了。
3. **Audacity 起動**: MCP が `curl` で既に応答していれば skip。なければ以下の順で `Audacity.app` を検索し `open` する：
   - `../Audacity.app`（DMG 内の隣）
   - `/Applications/Audacity.app`
   - `~/Applications/Audacity.app`
   - `../build/RelWithDebInfo/Audacity.app`（開発ビルド）
4. **Codex CLI 解決**: `PATH` → `~/.audacity-mcp-companion/bin/codex` → `/Applications/Codex.app` の順で探す。無ければ GitHub Releases から `codex-aarch64-apple-darwin.tar.gz` を自動ダウンロードして `~/.audacity-mcp-companion/bin/codex` に配置。
5. **companion 起動**: `python3 server.py` を実行。
6. **専用ウィンドウ**: `sleep 2` 後に Google Chrome の `--app` モード（`--app="http://127.0.0.1:8765" --new-window`）でクロムレスウィンドウを開く。Chrome がなければ `open URL` にフォールバック（既定ブラウザ）。

---

## 7. AU-only / GPLv2 ビルドと DMG 配布（コミット `a5d4fb8f6`・`dd3452591`・`898449816`）

### 7.1 AU-only ビルド設定

VST3 SDK（Steinberg 独自ライセンス / GPLv3 オプション）を除外し、バイナリを GPLv2-or-later で配布可能にする。Audio Unit は macOS システムの AudioUnit フレームワークを使うため、追加 SDK なし。

CMake フラグ:
```bash
cmake -G Ninja \
  -S /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp \
  -B /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -Daudacity_has_vst3=Off \
  -Daudacity_use_vst=Off \
  -Daudacity_bundle_gplv3=Off
cmake --build /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build -j
```

### 7.2 `scripts/make-dmg.sh`

`scripts/make-dmg.sh` — 配布 DMG を生成するスクリプト。出力先: `build/Audacity-MCP.dmg`。

ステージング手順（概略）:
1. `ditto "$APP" "$STAGE/Audacity.app"` でバンドルをコピー（メタデータ・ad-hoc 署名を保持）。
2. **Portable Settings を削除**（`rm -rf "$STAGE/Audacity.app/Contents/Portable Settings"`）: 開発機固有のモジュールパスやプリファレンスを除外。`mod-mcp-server` は `autoEnabledModules()` で自動有効化されるため問題なし。
3. **残存 VST dylib を削除**（`rm -f "$STAGE/Audacity.app/Contents/Frameworks/"lib-vst*.dylib`）: インクリメンタルビルドで残るかもしれない lib-vst*.dylib を防衛的に除去。
4. `/Applications` へのシンボリックリンクを追加（ドラッグインストール用）。
5. companion ファイルを `"$STAGE/MCP Companion/"` にコピー（secrets/state を除外: `.env`, `.codex/`, `__pycache__/`, `legacy/`）。
6. DMG ルートにワンクリックランチャー `"Audacity と AI チャットを起動.command"` を配置（companion の `セットアップして起動.command` に委譲するだけのシェルスクリプト）。
7. GPLv2 ライセンステキストを `https://www.gnu.org/licenses/old-licenses/gpl-2.0.txt` から取得し `"LICENSE (GPLv2).txt"` として同梱。
8. `hdiutil create` で圧縮 DMG を生成。

### 7.3 コード署名の注意点（重要な落とし穴）

**`codesign --force --deep` を実行してはいけない**。実行すると「有効な」署名が生成されるが、プラグインスキャナーヘルパーが不整合な形で再署名されるため、**新規プロファイルでの初回起動が 0% CPU でハング**（スキャナーが返らない）する。ビルダーのリンカーが付与した ad-hoc 署名のまま配布することが正解（`make-dmg.sh` の line 55–59 コメント参照）。

Apple Developer ID がないため公証なし。受取人は初回起動前に Gatekeeper クォランティンを解除する必要がある:
```bash
xattr -dr com.apple.quarantine /Applications/Audacity.app
# または: ファインダーで右クリック → 「開く」
```

Apple Silicon 専用（aarch64 バイナリ）。

---

## 8. 現在の状態・実行方法・検証

### 8.1 リポジトリ状態

- 作業リポ: `/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp`
- ブランチ: `mcp-llm`
- git remotes:
  - `fork` = `https://github.com/masatomoota/audacity.git`（**push 先**）
  - `origin` = `https://github.com/audacity/audacity.git`（公式・**絶対に push しない**）

### 8.2 ビルド方法（AU-only / GPLv2）

```bash
export PATH="/Users/masatomo/Library/Python/3.9/bin:/opt/homebrew/bin:$PATH"
cmake -G Ninja \
  -S /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp \
  -B /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -Daudacity_has_vst3=Off -Daudacity_use_vst=Off -Daudacity_bundle_gplv3=Off
cmake --build /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build -j
```

`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` は CMake 4.x で vendored cmake ファイルが `cmake_minimum_required(VERSION <3.5)` を使っているため必須。

### 8.3 mod-mcp-server の自動有効化

`mod-mcp-server` は `libraries/lib-module-manager/ModuleSettings.cpp` の `autoEnabledModules()` セット（line 69–95）に追加済み。新規プロファイルで起動するだけで MCP サーバが `127.0.0.1:4830` に立ち上がる（Portable Settings の手動設定は不要）。

**注意**: Audacity にアクティブプロジェクトがないと `run_command` は空文字列を返す（`ScriptCommandRelay.cpp` の `GetActiveProject()` が null の経路）。チャット経由で操作する前にプロジェクトを開いておくか、`NewMonoTrack` 等で作成する。

### 8.4 コンパニオンの実行

```bash
# 初回のみ: Codex CLI の取得 (または brew install --cask codex)
# 初回のみ: ChatGPT ログイン
#   codex login   # または companion UI の「ChatGPT でログイン」

cd /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/mcp-companion
python3 server.py
# → http://127.0.0.1:8765 をブラウザで開く
```

あるいは `セットアップして起動.command` をダブルクリックすれば Audacity の起動・Codex CLI の解決・companion 起動・専用ウィンドウ表示を一括で行う。

### 8.5 MCP サーバの疎通確認

```bash
MCP=http://127.0.0.1:4830/mcp
curl -s -X POST "$MCP" -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
# → {"jsonrpc":"2.0","id":1,"result":{}}

curl -s -X POST "$MCP" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'
# → run_command と get_info の 2 ツールが返る
```

### 8.6 ステム分離のセットアップ（オプション）

```bash
# DMG 同梱の「ステム分離セットアップ.command」をダブルクリック、または:
python3.11 -m venv ~/.audacity-mcp-companion/sep-venv
~/.audacity-mcp-companion/sep-venv/bin/pip install "audio-separator[cpu]"
# 初回分離時にモデルが自動 DL される (~/.audacity-mcp-companion/models/)
```

---

## 9. 開いている課題と次のステップ

### 9.1 ステム分離 venv のバンドル（最優先）

現状、ステム分離 venv は開発機にのみ存在する。DMG に含まれていないため、受取人は `ステム分離セットアップ.command`（または手動 pip）を実行する必要がある。配布パッケージに venv or 初回起動時の自動セットアップスクリプトを含めるのが理想。

オプション:
- (a) `make-dmg.sh` が `ステム分離セットアップ.command` を実行してステージング環境に venv を作り、DMG にバンドル（容量 ~1GB 増）。
- (b) companion 起動時に「分離機能が未設定です。セットアップしますか？」ダイアログを表示し自動インストール。
- (c) 現行どおり手動セットアップ（`ステム分離セットアップ.command` をドキュメントに明示する）。

### 9.2 Windows 移植（中難度）

コア変更（`mod-mcp-server`・知覚コマンド）は Windows で原則そのままビルドできる（wxWidgets も CMake も Windows 対応）。

差分対応:
- `BackedPanel.cpp` の HiDPI 修正は Windows では no-op（`GetContentScaleFactor()` = 1.0）。将来 DPI-aware マニフェストを追加する場合は `#if __WXMSW__` ガードを検討。
- コンパニオンランチャーは `.sh` / `.command` → `.bat` または PowerShell スクリプトに変換が必要。
- Codex CLI の Windows 版（`codex-x86_64-pc-windows-msvc.zip`）を自動取得するロジックを追加。

### 9.3 商標リブランド（公開配布必須）

`"Audacity"` 名称と Audacity ロゴは Muse Group の登録商標。公開配布する場合は改名（例: Tenacity, Audacium 等の前例あり）とアイコン差し替えが必要。`assets/` にアイコン候補セットが WIP で入っている（コミット `976c718b7`）。

### 9.4 Phase 1 実装（`tools/list` 自動生成）

Phase 0 ハンドオフ §11.2 で詳述済み。`MCPHttpServer.cpp` の `HandleToolsList` で `GetInfo: Type=Commands Format=JSON` を内部発行し、JSON-Schema に変換して約 250+ ツールを手書きゼロで生成する。現状は静的 2 ツール（`run_command` / `get_info`）のみ。

### 9.5 Electron アプリ化（任意）

companion の `index.html` + `server.py` を Electron でラップすれば、ブラウザ不要のネイティブ app になる。Node.js / Python ランタイムのバンドル方法の選択が主な課題。

---

## 10. 環境変数リファレンス

| 変数 | 既定値 | 説明 |
|---|---|---|
| `MCP_URL` | `http://127.0.0.1:4830/mcp` | Audacity MCP サーバの URL |
| `COMPANION_PORT` | `8765` | companion サーバのポート |
| `CODEX_BIN` | `codex` | Codex CLI のパス |
| `CODEX_HOME_COMPANION` | `~/.audacity-mcp-companion` | 隔離 CODEX_HOME（スレッド・config 保存先） |
| `CODEX_WORKDIR` | `CODEX_HOME/workdir` | Codex の作業ディレクトリ |
| `CODEX_MODEL` | （アカウント既定） | モデル override（例: `gpt-4o`） |
| `GLOBAL_CODEX_HOME` | `~/.codex` | グローバル認証の継承元 |
| `CODEX_SANDBOX` | `danger-full-access` | sandbox_mode 設定（変更は慎重に） |
| `AUDACITY_COMPANION_HOME` | `~/.audacity-mcp-companion` | stem_mcp_server の SUPPORT ディレクトリ |
| `SEP_VENV` | `SUPPORT/sep-venv` | 分離エンジン venv のパス |
| `SEP_DEFAULT_MODEL` | `UVR-MDX-NET-Inst_HQ_3.onnx` | デフォルト分離モデル |

---

## 11. 主要ファイルマップ（Session 2 の変更対象）

| ファイル | 変更内容 |
|---|---|
| `src/widgets/BackedPanel.cpp` | HiDPI バックバッファ修正（ResizeBacking line 48–63, DisplayBitmap line 71–80） |
| `libraries/lib-numeric-formats/formatters/ParsedNumericConverterFormatter.cpp` | TimeConverterFormats_[] のコロン形式化（line 712–） |
| `src/tracks/ui/CommonTrackInfo.cpp` | CloseTitleDrawFunction のアセントセンタリング（line 219–254） |
| `src/TrackArt.cpp` | DrawClipTitle のアセントセンタリング（line 289–311） |
| `libraries/lib-module-manager/ModuleSettings.cpp` | autoEnabledModules() に mod-mcp-server 追加（line 69–95） |
| `mcp-companion/codex_bridge.py` | Codex app-server JSON-RPC クライアント（全面新規） |
| `mcp-companion/server.py` | SSE bridge・config.toml 生成・API ルート（全面再構築） |
| `mcp-companion/stem_mcp_server.py` | UVR ステム分離 MCP サーバ（新規） |
| `mcp-companion/index.html` | チャット UI（完全日本語化・カーリークォートバグ修正） |
| `mcp-companion/セットアップして起動.command` | ワンクリックランチャー（Audacity + Codex + companion + 専用ウィンドウ） |
| `mcp-companion/ステム分離セットアップ.command` | ステム分離 venv セットアップスクリプト（新規） |
| `scripts/make-dmg.sh` | AU-only DMG パッケージングスクリプト（全面新規） |

---

*End of Session 2 handoff. 次の LLM へ：§8 でビルド・起動・疎通確認を行い、§9 の課題から着手すること。最も高インパクトな残課題は §9.1（ステム分離 venv のバンドル）と §9.4（Phase 1: tools/list 自動生成）。*
