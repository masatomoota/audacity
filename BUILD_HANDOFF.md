# Audacity MCP — ビルド・ハンドオフ（別スレッド／別 LLM 向け・完全再現用）

> 目的: **このリポジトリを別スレッド（別 LLM）が最初から完全にビルド・パッケージできる**ようにするための単一ドキュメント。
> このチャット（およびサブスレッド）で行った実装すべてを、ビルド再現に必要な粒度で記載する。
> コンパニオン内部の細かい挙動は [`MCP_SESSION2_HANDOFF.md`](MCP_SESSION2_HANDOFF.md) に詳しいので、本書はそれを参照しつつ**ビルド手順を主**とする。

最終更新: 2026-06-26 / 対象ブランチ: `mcp-llm`

---

## 0. 30 秒サマリ（まずこれだけ読めばビルドできる）

```bash
# 0) 場所とブランチ
cd /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp
git checkout mcp-llm            # 作業ブランチ

# 1) Audacity をビルド（AU-only / GPLv2 / arm64 / Ninja）
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -Daudacity_has_vst3=Off -Daudacity_use_vst=Off -Daudacity_bundle_gplv3=Off
cmake --build build -j
# → 成果物: build/RelWithDebInfo/Audacity.app（内蔵 MCP サーバ込み）

# 2) 配布 DMG を作成（任意）
./scripts/make-dmg.sh
# → build/Audacity-MCP.dmg

# 3) AI チャット（MCP コンパニオン）を起動（任意・別プロセス）
cd mcp-companion && ./run.sh     # → http://127.0.0.1:8765
```

これだけで「内蔵 MCP サーバ付き Audacity」+「言葉で操作する AI チャット」が動く。詳細は以下。

---

## 1. リポジトリと Git の絶対ルール（重要）

| 項目 | 値 |
|---|---|
| 作業ディレクトリ | `/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp` |
| 作業ブランチ | **`mcp-llm`** |
| `fork` リモート | `https://github.com/masatomoota/audacity.git` … **push 可（ここだけ）** |
| `origin` リモート | `https://github.com/audacity/audacity.git` … **公式。絶対に push しない** |

- **push 先は必ず `fork` のみ。`origin`（公式 Audacity）へは何があっても push しない。**
- **秘密情報は絶対にコミットしない**: `mcp-companion/.env`、`~/.audacity-mcp-companion/.codex/`（`auth.json` 等）、`__pycache__`、`legacy/` は配布・コミット対象外（`make-dmg.sh` も除外済み）。
- 商標: "Audacity" は Muse Group の登録商標。広く再配布するならアプリ名・アイコンのリブランドが必要（本ビルドは個人/検証用）。

### 1.1 現在のコミット状態（2026-06-26 時点）

```
358eae154 core: bug-hunt wave 1–5 — 30+ verified fixes across import/export, project I/O, RT effects  ← 別セッションの成果（後述 §6）
018b9f529 mcp-companion: interactive shell-approval UI (allow/deny), MCP auto-allowed                 ← fork/mcp-llm の現在地
c062fcf83 mcp-companion: Whisper transcription tool (local mlx-whisper), AI-chat driven
83de3fec6 mcp-companion: harden agent against shell/destructive requests + runaway loops
5c9c755bb handoff: MCP_SESSION2_HANDOFF.md — UI fixes, Codex companion, stems, DMG
b37d5f1b1 dist: bundle stem separation in the DMG companion + setup command
976c718b7 assets: companion icon / favicon concept set (WIP)
e6da02308 mcp-companion: UVR stem separation + full Japanese UI + dedicated window
898449816 dist: one launcher that starts Audacity + the AI chat together
dd3452591 dist: AU-only / GPLv2 DMG (drop VST3 SDK, bundle GPLv2 license)
a5d4fb8f6 dist: package Audacity + MCP companion into a distributable DMG
2f8a69c05 ui: optically center the clip title in the affordance bar
0f7b64eb7 ui: optically center the track title text in the title bar
b77092cca ui: crisp HiDPI track text + language-independent colon timecode
5c52b27ee mcp-companion: rebuild the chat client on the Codex app server
```

- **ローカル HEAD = `358eae154`**、**`fork/mcp-llm` = `018b9f529`**。
  → ローカルは `358eae154`（別セッションの C++ バグ修正）＋本ハンドオフのコミットの分だけ fork より進んでいる。
- 作業ツリーはクリーン（未コミットの変更なし）。
- **別スレッドが別マシン/別クローンでビルドする場合**は、まず fork を最新化する必要がある:
  ```bash
  git push fork mcp-llm        # ローカルの 358eae154 + 本ハンドオフを fork へ反映（origin には push しない）
  # 別マシン側:
  git clone https://github.com/masatomoota/audacity.git && cd audacity && git checkout mcp-llm
  ```

---

## 2. ビルド環境・前提ツール

| ツール | このマシンでの実績値 | 備考 |
|---|---|---|
| OS / Arch | macOS (Darwin) / **arm64 (Apple Silicon)** | `CMAKE_OSX_ARCHITECTURES=arm64` |
| Deployment target | 10.13 | CMake が自動設定 |
| CMake | **4.3.4** | 4.x のため **`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` が必須**（古い CMake 記述を許容させる） |
| Ninja | 1.13.2 | ジェネレータ |
| Conan | 必須（`audacity_conan_enabled=On`） | 依存パッケージを自動取得。プリビルドバイナリ許可（`audacity_conan_allow_prebuilt_binaries=On`） |
| Python | システム 3.9.6（ビルド用） | コンパニオンは別途 3.10+ を使う（§5） |
| Xcode CLT | 必要 | clang / システム AudioUnit フレームワーク |

- 依存（wxWidgets 3.1.3 等）は Conan が初回 configure 時に解決・取得する。ネットワーク必須。
- ベースは Audacity 3.7 系。

---

## 3. Audacity のビルド（AU-only / GPLv2）

### 3.1 なぜ AU-only / GPLv2 か
- VST3 SDK は **GPLv3**。これを含めると配布物全体が GPLv3 になる。
- 本ビルドは **VST/VST3 を無効化**し、プラグインは **AU（Audio Unit）のみ**（macOS のシステム AudioUnit フレームワークのみ使用、サードパーティ SDK 不要）。
- これにより GPLv3-only コンポーネントを含まず、**GPLv2-or-later で配布可能**。

### 3.2 Configure（このリポジトリで実際に使われた設定）

```bash
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -Daudacity_has_vst3=Off \
  -Daudacity_use_vst=Off \
  -Daudacity_bundle_gplv3=Off
```

CMakeCache での確認済み値:
```
CMAKE_BUILD_TYPE        = RelWithDebInfo
CMAKE_GENERATOR         = Ninja
CMAKE_OSX_ARCHITECTURES = arm64
audacity_has_vst3       = Off
audacity_use_vst        = Off
audacity_bundle_gplv3   = Off
audacity_conan_enabled  = On
```

### 3.3 Build

```bash
cmake --build build -j
```

- 成果物: **`build/RelWithDebInfo/Audacity.app`**（約 77MB）。
- ビルドは Conan が自動でビルドする AudioUnit ホスト等を含む。
- インクリメンタルビルドで `lib-vst*.dylib` が残ることがあるが、`make-dmg.sh` が配布時に削除する（§4）。

### 3.4 内蔵 MCP サーバ（`mod-mcp-server`）
- **コード内 `autoEnabledModules()` で自動有効化**されるため、別途の有効化操作は不要。
- 起動すると **`http://127.0.0.1:4830/mcp`** で JSON-RPC（MCP）を待ち受ける。
- 疎通確認:
  ```bash
  curl -s http://127.0.0.1:4830/mcp -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{}}'
  # → {"jsonrpc":"2.0","id":1,"result":{}}
  curl -s http://127.0.0.1:4830/mcp -H 'content-type: application/json' \
    -d '{"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}}'
  # → run_command / get_info などのツールが返る
  ```
- 注意: MCP 経由の操作はアクティブプロジェクトが必要。空応答になる場合は Audacity を最前面にしてから再試行。

---

## 4. 配布 DMG の作成

```bash
./scripts/make-dmg.sh          # 事前に §3 のビルドが完了していること
# → build/Audacity-MCP.dmg
```

`scripts/make-dmg.sh` の要点（全文は同ファイル参照）:
- `build/RelWithDebInfo/Audacity.app` を `ditto` でステージ（ad-hoc 署名を保持）。
- **`Contents/Portable Settings` を削除**（マシン固有設定・非 ASCII ファイル名で DMG のコードシールが壊れるため）。`mod-mcp-server` はコードで自動有効化なので新規プロファイルでも MCP は起動する。
- **`lib-vst*.dylib` を削除**（AU-only。GPLv3 の VST コードを混入させない）。
- `MCP Companion` フォルダに必要ファイルのみ同梱: `server.py codex_bridge.py stem_mcp_server.py transcribe_mcp_server.py index.html run.sh README.md` ＋ 2 つの `.command`。**秘密情報/状態（.env, .codex/, __pycache__, legacy/）は含めない**。
- ルートに「Audacity と AI チャットを起動.command」（両方まとめて起動）、`はじめにお読みください.txt`（日本語）、`LICENSE (GPLv2).txt` を配置。
- DMG は `hdiutil create -format UDZO`（圧縮）。

### 4.1 コード署名の落とし穴（厳守）
- **`codesign --force --deep` をしてはいけない。** 一見「正常な署名」になるが、同梱の plugin-scanner ヘルパが不整合に再署名され、**新規プロファイルでの初回起動が 0% CPU でハングする**。
- このマシンには Apple Developer ID が無く、配布物は**未署名（公証なし）**。受領者は初回のみ Gatekeeper 解除が必要:
  ```bash
  xattr -dr com.apple.quarantine /Applications/Audacity.app
  # または Finder で右クリック →「開く」
  ```

---

## 5. MCP コンパニオン（AI チャット）— Codex app-server ベース

言葉で Audacity を操作するチャット UI。**OpenAI Codex app-server**（`codex app-server`、stdio JSON-RPC）を使い、ChatGPT Web ログイン＋会話記憶を持つ。

### 5.1 構成

```
ブラウザ (index.html, 日本語UI)
   ⇅  HTTP/SSE  (http://127.0.0.1:8765)
server.py  (SSE ブリッジ / 設定生成 / /api/approval)
   ⇅  stdio JSON-RPC
codex app-server  (CODEX_HOME = ~/.audacity-mcp-companion で隔離)
   ⇅  MCP
 ├─ audacity        … Audacity 内蔵 MCP (http://127.0.0.1:4830/mcp)
 ├─ stem_separator  … stem_mcp_server.py (UVR ステム分離)
 └─ transcribe      … transcribe_mcp_server.py (Whisper 文字起こし)
```

### 5.2 ファイル（`mcp-companion/`）
| ファイル | 役割 |
|---|---|
| `server.py` | SSE ブリッジ。設定（approval_policy / sandbox_mode / `[mcp_servers.*]`）を生成、`/api/chat`・`/api/approval` を提供、turn ストリームを SSE 中継 |
| `codex_bridge.py` | codex app-server への stdio JSON-RPC クライアント。承認要求のルーティング（§5.4）、スレッド start/resume |
| `index.html` | 全面日本語 UI。承認カード（許可/拒否ボタン）を入力欄下に表示 |
| `stem_mcp_server.py` | stdio MCP。`separate_stems` / `list_stem_models`（`audio-separator` を呼ぶ） |
| `transcribe_mcp_server.py` | stdio MCP。`transcribe_audio` / `list_transcribe_models`（`mlx_whisper` を呼ぶ。`TRANSCRIBE_ENGINE=openai` で OpenAI API 経由も可） |
| `run.sh` | コンパニオン単体起動（`http://127.0.0.1:8765`） |
| `セットアップして起動.command` | Audacity（`../Audacity.app`→`/Applications`→dev build の順に解決）→ コンパニオンを Chrome `--app` 専用ウィンドウで起動 |
| `ステム分離セットアップ.command` | 音声 AI venv 作成（`audio-separator[cpu]` ＋ `mlx-whisper`） |

### 5.3 起動方法

```bash
# 単体（開発時）
cd mcp-companion && ./run.sh        # → http://127.0.0.1:8765
# 初回のみ: codex CLI 取得（brew install --cask codex でも可）
# 初回のみ: ChatGPT ログイン（UI の「ChatGPT でログイン」または codex login）

# Audacity と一緒に（配布同等）
cd mcp-companion && ./セットアップして起動.command
```

### 5.4 承認 UI（このチャットで最後に実装した機能・検証済み）
- **シェル/exec/ファイル操作の承認要求**は、入力欄の下に**承認カード**（実コマンド＋作業ディレクトリ＋ **[許可][拒否]**）として表示。Claude Code の権限プロンプトと同じ発想。
- **MCP ツール**（audacity / stem / transcribe）は**承認不要で自動許可**。
- 仕組み: codex の承認要求（`item/commandExecution/requestApproval` 等）を**リーダースレッドをブロックせず** SSE で UI に中継 → ユーザー決定を `POST /api/approval` → `codex_bridge.resolve_approval()` で codex に返答。MCP 呼び出しは非 `never` ポリシー下で `mcpServer/elicitation/request` として来るので **自動 accept**（`{"action":"accept","content":{}}`）。
- 既定設定: `approval_policy="untrusted"` ＋ `sandbox_mode="danger-full-access"`。
  → **sandbox は danger-full-access のまま維持**（read-only/workspace-write だと codex の localhost MCP 通信がブロックされることを確認済み。ネットワーク必須）。
- 検証済み: 拒否でコマンド不実行（sentinel 生存）、許可で実行（証明ファイル生成）、MCP は緑✓でプロンプト無し。

### 5.5 音声 AI（ステム分離・文字起こし）セットアップ（任意）
```bash
cd mcp-companion && ./ステム分離セットアップ.command   # 初回のみ・約1.5GB
# venv: ~/.audacity-mcp-companion/sep-venv  に
#   audio-separator[cpu] （UVR/MDX/Demucs。既定 UVR-MDX-NET-Inst_HQ_3.onnx）
#   mlx-whisper          （Apple GPU。既定 mlx-community/whisper-large-v3-turbo）
```
- チャットで「ボーカルとインストに分離して」「この音声を文字起こしして」と頼むと、書き出し→分離/文字起こし→結果取り込みまで自動。
- 詳細は [`MCP_SESSION2_HANDOFF.md`](MCP_SESSION2_HANDOFF.md) §4（ステム）と本リポジトリの `transcribe_mcp_server.py`。

### 5.6 環境変数（主要）
| 変数 | 既定 | 用途 |
|---|---|---|
| `CODEX_HOME` | `~/.audacity-mcp-companion` | codex 状態の隔離先（auth.json 等） |
| `CODEX_APPROVAL` | `untrusted` | approval_policy |
| `CODEX_SANDBOX` | `danger-full-access` | sandbox_mode（変更非推奨・§5.4） |
| `TRANSCRIBE_ENGINE` | （未設定=ローカル） | `openai` で OpenAI API 経由の文字起こし |
- 完全版は [`MCP_SESSION2_HANDOFF.md`](MCP_SESSION2_HANDOFF.md) §10。

---

## 6. このチャットで行った実装の全体マップ（コミット別）

### 6.1 Audacity コア C++（UI 修正）— **このチャットの成果**
| コミット | 内容 | 主なファイル |
|---|---|---|
| `b77092cca` | **HiDPI（Retina）トラックテキストのクッキリ化**＋**言語独立コロン形式タイムコード** | `src/widgets/BackedPanel.cpp`（裏バッファをデバイス解像度で確保: `CreateScaled(w,h,24,scale)` / blit は `GetScaledSize()`）、`libraries/lib-numeric-formats/formatters/ParsedNumericConverterFormatter.cpp`（`時間/分/秒` → `0100:060:060` 等のコロン書式） |
| `0f7b64eb7` | **トラックタイトルの垂直光学センタリング** | `src/tracks/ui/CommonTrackInfo.cpp`（`DrawText(..., bev.y + (bev.height - metrics.ascent)/2)`） |
| `2f8a69c05` | **クリップタイトルの垂直光学センタリング** | `src/TrackArt.cpp`（`DrawLabel(CENTER_VERTICAL)` → 手動 `DrawText` でフォントメトリクス基準のセンタリング） |

### 6.2 MCP コンパニオン — **このチャットの成果**
| コミット | 内容 |
|---|---|
| `5c52b27ee` | チャットを **Codex app-server ベースに再構築**（ChatGPT ログイン＋会話記憶） |
| `e6da02308` | **UVR ステム分離**＋**完全日本語 UI**＋**専用ウィンドウ** |
| `898449816` | Audacity と AI チャットを**まとめて起動**するランチャー |
| `b37d5f1b1` | DMG にステム分離を同梱＋セットアップ command |
| `83de3fec6` | エージェントの**安全強化**（シェル/破壊的要求・暴走ループ対策） |
| `c062fcf83` | **Whisper 文字起こし**ツール（ローカル mlx-whisper・チャット駆動） |
| `018b9f529` | **対話的シェル承認 UI**（許可/拒否、MCP は自動許可）← §5.4 |

### 6.3 配布
| コミット | 内容 |
|---|---|
| `a5d4fb8f6` | Audacity + コンパニオンを DMG にパッケージ |
| `dd3452591` | **AU-only / GPLv2 DMG**（VST3 SDK を外し GPLv2 ライセンス同梱） |
| `976c718b7` | コンパニオンのアイコン/favicon 案（WIP） |

### 6.4 ドキュメント
| コミット | 内容 |
|---|---|
| `5c9c755bb` | `MCP_SESSION2_HANDOFF.md`（UI / コンパニオン / ステム / DMG の詳細・465 行） |
| （本コミット） | 本書 `BUILD_HANDOFF.md` |

### 6.5 別セッションの成果（**このチャットの作業ではない**・参考）
| コミット | 内容 |
|---|---|
| `358eae154` | `core: bug-hunt wave 1–5 — 30+ verified fixes`（import/export・project I/O・RT effects のスレッド安全化/データ競合修正など。例: `AudioIO.h` の `mSeek` を `std::atomic<double>` 化）。**並行して走っていた別 Claude セッションが mcp-llm にコミットしたもの。** ブランチに含まれるためビルド対象に入る。問題があればこのコミット単体を `git revert 358eae154` で外せる。 |

> 注意: このマシンでは **複数の Claude/Codex セッションが同時に同じ repo / 同じ `mcp-llm` ブランチで作業している**ことがある。別スレッドで作業する際は、コミット前に `git status` と `git log` を確認し、自分の変更だけを `git add <path>` で明示的にステージすること（このチャットでも `git add mcp-companion/...` のように明示してきた）。

---

## 7. 検証チェックリスト（別スレッドでビルド後に確認すること）

1. ビルド成功: `build/RelWithDebInfo/Audacity.app` が生成される。
2. 起動: `open build/RelWithDebInfo/Audacity.app`（初回はプラグインスキャン・ようこそ画面）。
3. 内蔵 MCP 疎通: §3.4 の `curl`（initialize → `{}`、tools/list → ツール一覧）。
4. UI 目視: トラック文字が Retina でクッキリ／タイトルが上下センター／タイムコードがコロン表記。
5. DMG: `./scripts/make-dmg.sh` → `build/Audacity-MCP.dmg`（約 28MB）。
6. コンパニオン: `cd mcp-companion && ./run.sh` → `http://127.0.0.1:8765` で UI 表示、ChatGPT ログイン後にチャット応答。
7. 承認 UI: チャットでシェル実行を伴う依頼 → 入力欄下に承認カード → 拒否で不実行・許可で実行／MCP ツールはプロンプト無し。

---

## 8. 既知の落とし穴（再掲・重要）

- **CMake 4.x** → `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` を付けないと configure が失敗する。
- **`codesign --force --deep` 禁止**（初回起動ハング）。未署名＋`xattr -dr com.apple.quarantine` で配布。
- **sandbox は `danger-full-access` 維持**（MCP の localhost 通信に必要）。
- **HTML 属性は ASCII クォートのみ**（過去にカーリークォート `”` 混入で UI が真っ白になった。`index.html` 編集時は注意）。
- **push は `fork` のみ・`origin` 禁止**。秘密情報（.env/.codex/auth.json）はコミットしない。
- 複数セッション同時作業の可能性 → コミット前に必ず `git status`/`git log` 確認・自分の変更のみ `git add`。

---

## 9. 参照ドキュメント
- [`MCP_SESSION2_HANDOFF.md`](MCP_SESSION2_HANDOFF.md) — UI 修正 / Codex コンパニオン / ステム分離 / DMG の**詳細実装**（コード断片・行番号付き、465 行）。本書と併読推奨。
- `scripts/make-dmg.sh` — DMG パッケージングの全手順（コメント充実）。
- `mcp-companion/README.md` — コンパニオンの使い方。
- OpenAI Codex app-server: https://developers.openai.com/codex/app-server
