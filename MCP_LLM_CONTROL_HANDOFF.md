# Audacity MCP / LLM 制御層 — 実装ハンドオフ（Audacity 3.x ベース）

> **ABSTRACT (English, for any LLM picking this up cold):** This document is a complete, self-contained handoff for building an **MCP (Model Context Protocol) server inside Audacity** so that an LLM can drive Audacity by natural language — the audio-app analogue of Codex/Cursor for code. It targets the **stable Audacity 3.x codebase (`audacity3` branch)**, *not* the 4.0-alpha you are currently checked out on, for reasons explained in §1 (the "dark-build" problem). You do **not** need the originating chat; everything required is below. Prose is Japanese, but all file paths, identifiers, commands, and code are English — every claim carries a `file:line` citation you can verify. Start at §0, then §1, then the roadmap in §7.

> **See also:** project-wide master handoff at https://github.com/masatomoota/llm-daw-handoff (chronological narrative, decision tree, prioritized roadmap to 100%, auto-start protocol).

---

## 0. このドキュメントの使い方・前提

- **目的**：Audacity に「言葉で指示して操作する」LLM 制御層（MCP サーバ）を実装するための、ゼロ知識から着手できる実装ハンドオフ。並行する Ardour 版（§9）と対をなす。
- **読者**：このスレッドを知らない別の LLM / エンジニア。**この文書だけで 100% 着手・継続できる**ことを意図している。
- **分析の出自**：本ハンドオフは、Audacity リポジトリ（解析時点 `Audacity-4.0.0-alpha-1-2702-gcaa9b9fdc`）と Ardour リポジトリの一次ソース精読（複数の専門サブエージェント調査＋検証）に基づく。補足の詳細レポート（PDF）は作業者ホストの `/Volumes/work-ssd-4TB-USB4/_Git_Repository/llm-daw-report/` に存在するが、**本文書は単体で完結**しており、それらに依存しない。
- **パス表記の規約（重要）**：以下の `file:line` 引用は、解析した **4.0-alpha のレイアウト**（レガシーコードが `au3/` 配下）で示す。**`audacity3` ブランチでは `au3/` 接頭辞を外す**（例：`au3/src/commands/...` → `src/commands/...`、`au3/modules/scripting/mod-script-pipe/...` → `modules/mod-script-pipe/...`、`au3/libraries/...` → `libraries/...`）。コードの中身・構造は両者で実質同一。

---

## 1. 最重要の意思決定：ターゲットは Audacity **3.x**（`audacity3` ブランチ）

### 1.1 なぜ 4.0-alpha ではないのか — "dark-build" 問題（一次検証済み）
Audacity 4.0 は **au3（レガシー Audacity-3 の音声/コマンド/スクリプトcoア）＋ Muse フレームワーク（MuseScore-4 由来の Qt/QML app shell）** のハイブリッド移行期にある。検証の結果、**LLM 制御に最も価値のある基盤（コマンド系と `GetInfo` JSON と `mod-script-pipe`）は 4.0 のアクティブビルドにコンパイルされていない**ことが判明した：

- 4.0 のビルドは `src/au3wrap/CMakeLists.txt:96` で `add_subdirectory(${AU3_LIBRARIES} ...)` を行い、`AU3_LIBRARIES = ${AUDACITY_ROOT}/libraries`（`src/au3wrap/au3wrapDefs.cmake:22`）—— すなわち **`au3/libraries/` のみ**を取り込む。
- `au3/src/commands`（`GetInfoCommand` 等）と `au3/modules/scripting/mod-script-pipe` を **4.0 の `src/` ビルドが参照する箇所はゼロ**（root + `src/` を grep 確定。例外は import-export モジュールのみ）。

→ 4.0 で実装を始めると、最初の作業が「死んでいるコードの再結線（wx 密結合・alpha churn と戦う）」になる。

### 1.2 3.x なら全てが「ライブ＝コンパイル済み」
**`audacity3` ブランチ（および `release-3.2.x` タグ群）は完全なスタンドアロン Audacity 3.x** であり、コマンド系・`mod-script-pipe`・`GetInfo` JSON・`Screenshot` コマンドまで**全て実際にビルドされ動作する**。dark-build 問題は **4.0 固有**。したがって：

> **決定：`audacity3` ブランチ（または最新の `release-3.2.x`）をチェックアウトして実装する。** ここでは MCP 化に必要な基盤がすべて生きており、「復活作業」ゼロで着手できる。アーキも安定（wxWidgets 単一ツリー、alpha churn なし）。

確認済みブランチ（`git branch -r`）：`origin/audacity3`, `origin/release-3.0.3` 〜 `origin/release-3.2.5`（以降も）。

```bash
# 着手の最初の一歩
cd <audacity-repo>
git fetch origin
git checkout -b mcp-llm audacity3        # または release-3.2.5 等の安定タグ起点
```

### 1.3 4.0/Muse への将来移植（視野）
3.x で設計を完成・実証したのち、Audacity 4 のアーキが固まった段階で **Muse のアクションバス（`muse::actions::IActionsDispatcher::dispatch()` / `actionList()`）**へ移植できる（4.0 の正規拡張点）。ただし Muse 側は `GetInfo` 相当の機械可読スキーマ/状態 read-back を欠き、`action://` 名が不安定なので、**まず 3.x で証明 → 後で移植**が手堅い。本ハンドオフは 3.x 実装に集中する。

---

## 2. ゴールと全体像

**ゴール**：外部（または将来アプリ内）の LLM が、自然言語の意図を構造化ツール呼び出しに変換して Audacity を駆動する。「このトラックにノイズ除去かけて、-3dB にして、MP3 で書き出して」が通る。

**戦略の核心**：Audacity 3.x は**機能的に `tools/list` と `tools/call` に相当するものを既に内蔵**している。欠けているのは **1 層だけ — MCP/HTTP サーバ**。Ardour が約 95 ツールを手書きしたのに対し、**Audacity はツールカタログを `GetInfo` JSON から自動生成できる**（最大の差別化）。

```
[LLM] --MCP/JSON-RPC over HTTP--> [新規 mod-mcp-server]
          tools/list  <= GetInfo: Type=Commands Format=JSON（自動生成）
          tools/call  => "CmdName: Param=Value" 文字列
                      => ScriptCommandRelay::ExecFromWorker（無改変・既存）
                      => CommandBuilder => BatchCommand
                      => MacroCommands::ApplyCommand
                      => CommandDispatch::HandleTextualCommand（メニュー）
                         / DoAudacityCommand（エフェクト/コマンド）
                      => 結果 "Name finished: OK|Failed!" を MCP result へ
```

---

## 3. 既存基盤（3.x で「生きている」もの）— 実装はこの上に乗せる

すべて `file:line` 付き。4.0 レイアウト表記（§0 の規約で 3.x に読み替え）。

### 3.1 トランスポート：`mod-script-pipe`（差し替え対象）
- 行指向パイプを開く：Windows 名前付きパイプ `\\.\pipe\ToSrvPipe` / `FromSrvPipe`（`au3/modules/scripting/mod-script-pipe/PipeServer.cpp:18,33`）、POSIX FIFO `/tmp/audacity_script_pipe.{to,from}.<uid>`（`PipeServer.cpp:116`）。1 行＝1 コマンド。
- モジュール ABI：必須エントリは 2 つだけ — `GetVersionString()` と `ModuleDispatch(ModuleDispatchTypes)`（`au3/libraries/au3-utility/ModuleConstants.h:26-40`）。`wxDynamicLibrary` でロード（`au3/libraries/au3-module-manager/ModuleManager.cpp:143-151`）。
- `mod-script-pipe` の `ModuleDispatch`（`ScripterCallback.cpp:44-48`）は `ModuleInitialize` で `ScriptCommandRelay::StartScriptServer(RegScriptServerFunc)` を呼ぶだけ。**transport とコマンド実行は既に分離**している。

### 3.2 コマンド実行リレー（無改変で再利用する核心）
- `ScriptCommandRelay::ExecFromWorker`（`au3/src/commands/ScriptCommandRelay.cpp:63` 付近、本体は `ExecCommand` `:33`）が受信文字列を処理する：
```cpp
CommandBuilder builder(*pProject, *pIn);     // ScriptCommandRelay.cpp:36
AppCommandEvent ev; ev.SetCommand(cmd);
wxTheApp->AddPendingEvent(ev);               // GUI(メイン)スレッドへ投函
*pOut = builder.GetResponse();               // wxSemaphore でブロックし応答を待つ
```
- **この関数を一切変えずに**、新しい HTTP/MCP サーバから呼ぶのが実装の肝。

### 3.3 ディスパッチ連鎖（単一チョークポイント）
`CommandBuilder` は全コマンドを `BatchCommand` に流す（`CommandBuilder.cpp:154-161`）→ `MacroCommands::ApplyCommand`（`au3/src/BatchCommands.cpp:585`）→ `CommandDispatch::HandleTextualCommand`（`au3/src/commands/CommandDispatch.cpp:28`、メニューコマンド）または `DoAudacityCommand`（エフェクト/`AudacityCommand`）。**1 本の文字列であらゆるメニュー・エフェクト・コマンドに到達できる**。

### 3.4 自己記述 JSON（`tools/list` の自動生成元 — 最大の資産）
`GetInfoCommand`（`au3/src/commands/GetInfoCommand.cpp`）：
- `GetInfo: Type=Commands Format=JSON` が**全コマンドの定義（id / name / params[type, default, enum] / tip）を JSON で吐く**（`ShuttleGuiGetDefinition` 系が `id/prompt/type/default/enum` を出力、`GetInfoCommand.cpp:220-394` 付近、`SendCommands` `:409-432`）。**MCP `tools/list` の inputSchema へほぼ 1:1 変換できる。**
- 他に `Type=` で `Menus / Tracks / Clips / Labels / Envelopes / Selection / Preferences / Boxes` を JSON 出力（`GetInfoCommand.cpp` 各 `Send*`）。これは**部分的な知覚層**でもある（LLM が状態を読める）。

### 3.5 型付きパラメータと結果契約
- 各 `AudacityCommand` は `VisitSettings()`（`ShuttleAutomation`）で型・既定・列挙を宣言（例 `au3/src/commands/SelectCommand.cpp:71-80`）。MCP の inputSchema 生成に必要なメタデータが揃っている。
- 結果は `"<Name> finished: OK"` / `"...finished: Failed!"`（`au3/src/commands/Command.cpp:154-158`）。**パース可能なステータストークン**。**留意**：`AddBool` が真偽値をクォートする癖がある（`au3/.../CommandTargets.cpp`、Phase 1 で要是正）。

### 3.6 コマンドカタログの広さ（Ardour 約95ツールに匹敵）
- **約 29 個のパラメータ付き組み込み `AudacityCommand`**（`Registration<>` 自己登録、`au3/src/commands/*.cpp`）：`Select`/`SelectTime`/`SelectFrequencies`/`SelectTracks`、`SetTrack(Status/Audio/Visuals)`/`GetTrackInfo`、`SetClip`/`SetLabel`/`SetEnvelope`、`SetProject`/`Open`/`Save`/`SaveCopy`、`Import2`/`Export2`、`Get/SetPreference`、`GetInfo`、`Message`/`Help`/`CompareAudio`/`Drag`、そして **3.x では `Screenshot` も生きている**（4.0 では `au3/src/commands/CommandDirectory.cpp:54` でコメントアウト）。
- **約 212 個のメニューコマンド ID**（`au3/src/menus/*.cpp` の `Command(...)`）：トランスポート完全（`Play`/`Stop`/`Record1stChoice`/`PlayLooped`…）、編集（`Cut`/`Split`/`Join`）、書き出し（`ExportWav`/`ExportMp3`/`ExportFLAC`…）。`MacroCommands::GetAllCommandNames`（`BatchCommands.cpp:333`）で列挙。
- **35 個の組み込みエフェクト** ＋ Nyquist/VST/VST3/AudioUnit/LADSPA/Vamp が同一 `PluginTypeEffect` 名前空間に入り**自動的にスクリプタブル**。`MacroCommandsCatalog`（`BatchCommands.cpp:303`）が全層を統合。

### 3.7 スレッドモデル（Ardour より構造的に安全 — 無償で継承できる）
スクリプトワーカーは専用デタッチドスレッドだが、**プロジェクト状態を直接変異させない**。受信文字列を `AppCommandEvent` に包み `wxTheApp->AddPendingEvent`（`ScriptCommandRelay.cpp:43-50`）でメインスレッドへ送り、`wxSemaphore`（`ResponseTarget`、`au3/.../CommandTargets.h:217-226`）で応答をブロッキング待機する。実際の `cmd->Apply()` は **GUI(メイン)スレッドで実行**される。新 MCP サーバが `ExecFromWorker` を再利用すれば**このスレッド整流を無償で継承**でき、Ardour の「ワーカースレッドからの直接変異」リスク（別文書 §9）を設計段階で回避できる。

---

## 4. 実装するもの：新規モジュール `mod-mcp-server`

`mod-script-pipe` を**複製**し、パイプを最小 HTTP/MCP サーバに置換、`tools/list` を `GetInfo` JSON から生成する。**既存コードの改変はビルド登録のみ。**

### 4.1 MCP プロトコル（JSON-RPC 2.0 over HTTP）
最低限実装するメソッド：
- `initialize` → `{ "protocolVersion":"2025-03-26", "capabilities":{"tools":{"listChanged":false}}, "serverInfo":{"name":"audacity-mcp","version":"0.1.0"} }`
- `notifications/initialized`（応答なし）、`ping` → `{}`
- `tools/list` → §4.3 で生成したツール配列
- `tools/call` → §4.4 のブリッジ

参考実装：Ardour 版 `mcp_http_server.cc::dispatch_jsonrpc`（別リポ `ardour/libs/surfaces/mcp_http/`）が**意味論の正しいリファレンス**になる（プロトコル枠・structuredContent+text フォールバック・エラーコード）。コードはコピーせず**設計を参照**する。

### 4.2 HTTP トランスポートの選択
wxWidgets 同梱の `wxWebRequest` は**クライアント専用**でサーバ不可。推奨は **`cpp-httplib`（ヘッダオンリー・MIT、GPLv3/GPLv2 互換）**を `lib-src/` か `modules/mod-mcp-server/` に同梱、`POST /mcp` の単一エンドポイントを立てる。代替：素の BSD socket、または libwebsockets（Ardour と揃えたい場合）。MIT のヘッダオンリーが最小摩擦。

### 4.3 `tools/list` の自動生成（このプロジェクトの目玉）
1. 起動後 1 回、内部で `GetInfo: Type=Commands Format=JSON` を `ExecFromWorker` 経由で発行。
2. 返る JSON 配列の各コマンド（`id`, `params[{key,type,default,enum}]`）を **MCP ツール記述子**へ変換：
   - `name` ← コマンド ID（スペースを `_` 等に正規化、双方向マップを保持）。
   - `inputSchema` ← `params` を JSON-Schema（`type`/`default`/`enum`/`additionalProperties:false`）へ写像。`type` の対応表（`double`→number, `bool`→boolean, `string`→string, `enum`→string+enum）を 1 つ用意。
3. メニューコマンド（パラメータなし、約 212）も `Type=Menus` か `GetAllCommandNames` から引数なしツールとして加える（任意。まずは AudacityCommand 群だけでも実用）。
4. 結果をキャッシュ。**約 250+ ツールが手書きゼロで得られる。**

### 4.4 `tools/call` のブリッジ
1. MCP の `params.name` + `params.arguments`（JSON）を受ける。
2. `"<CmdName>: key1=val1 key2=\"val with space\" ..."` の**コマンド文字列に整形**（Audacity のコマンド構文。真偽値・文字列のクォート規約に注意、§3.5 の `AddBool` 癖を吸収）。
3. その文字列を `ExecFromWorker`（無改変）へ渡す（→ §3.7 で GUI スレッド実行）。
4. 戻り（`GetInfo` 系なら JSON、その他は `"...finished: OK|Failed!"`）を解析し、MCP `result`（`content[].text` ＋可能なら `structuredContent`）に包む。`Failed!` は MCP のツールエラーへマップ。

---

## 5. スレッド安全性とセキュリティ（必須・正しさの核心）

- **整流を再利用、ワーカーから状態を触らない**：HTTP リクエストは worker スレッドで受け、コマンド実行は必ず `AppCommandEvent` + `wxSemaphore`（§3.7）でメインスレッドへ。新サーバが独自に session を触らないこと。
- **localhost バインド固定**：listen を `127.0.0.1` に限定（外部到達不可）。
- **トークン認証**：起動時に乱数トークンを生成、`Authorization: Bearer <token>` を必須化、トークンをログ/設定 UI に表示。
- **既定 OFF・明示オプトイン**：モジュールはビルトインだが、サーバ起動は設定で明示有効化。
- **破壊的操作のゲート**：`Export*`/`Save*`/ファイル I/O、後述 Nyquist のファイル系は確認/許可リストを通す（無人運用時）。
- **Nyquist サンドボックス**（§8 の生成 DSP を使う場合）：同梱 libnyquist は `xsystem`（シェル実行）を無効化済み（`thirdparty/libnyquist/.../nyx.c:1329-1338`）だが、`xopen`（ファイル I/O）・`chdir`・`getenv` は健在。無人で LLM 生成 Nyquist を流すなら**ファイル系プリミティブの追加ゲート**が要る。

---

## 6. カバレッジと埋めるべきギャップ

**強い（3.x で今すぐ EXISTS）**：トランスポート、トラック管理（mute/solo/gain/pan/色/高さ）、選択（時間・周波数・トラックを相対指定込み）、クリップ/ラベル編集、エフェクト/生成/解析（35＋Nyquist＋サードパーティ）、I/O（`Import2`/`Export2`）、設定（`Get/Set Preference`）、イントロスペクション（`GetInfo` JSON）、`Screenshot`（3.x）。

**ギャップ（要追加コード、grep 確定）**：
1. **リアルタイム・メータ読み出しが皆無**（peak/RMS/レベル取得コマンドなし）。LLM が「今クリップしているか」を判断できない → 新コマンド/ツールを追加（`WaveTrack`/メータ系 API から）。
2. **波形/スペクトルのサンプル値読み出し不可**（`GetInfo` はメタデータのみ、`CompareAudio` は差分判定どまり）。
3. **Undo グルーピング不在**：バッチは `kSkipState`（`BatchCommands.cpp:571`）で個別 undo を積まない。複数編集を 1 トランザクションに束ねる仕組みを足す（エージェント編集の原子的ロールバック）。
4. **構造化エラー/進捗/キャンセルが貧弱**（パイプ越し文字列）。MCP 化に伴い structured error を整備。

---

## 7. フェーズ別ロードマップ（着手順）

### Phase 0 — `mod-mcp-server` 試作（最初の動くデモ）
- `audacity3` ブランチで**素の Audacity 3.x をビルド**（§ビルド）。
- `modules/mod-script-pipe` を `modules/mod-mcp-server` に複製。`ModuleDispatch` で `ScriptCommandRelay::StartScriptServer(...)` を流用（`ExecFromWorker` は無改変）。
- パイプを最小 localhost HTTP/JSON-RPC（`cpp-httplib`）に置換。**2 つの MCP 動詞をハードコード**（1 つは `GetInfo` 直叩き、1 つは任意コマンド文字列のプロキシ）。
- **受入基準**：外部 LLM（Codex CLI / Claude Desktop 等を `http://127.0.0.1:<port>/mcp` に接続）から `Select` → 任意 Effect → `Export2` を端から端まで実行できる。
- **リスク**：HTTP ライブラリ統合と CMake 登録。コマンド文字列のクォート規約。

### Phase 1 — ツールカタログ自動生成
- §4.3 を実装。`GetInfo Type=Commands Format=JSON` → JSON-Schema 変換。`AddBool` のクォートを是正し型付き引数を往復させる。
- **受入基準**：約 250+ ツールが手書きゼロで `tools/list` に出る。任意の AudacityCommand を typed-args で呼べる。

### Phase 2 — 知覚・安全・Undo
- `GetInfo` の Tracks/Clips/Labels/Selection を MCP **resources**（または専用 read ツール）として公開。
- **メータ読み出しツールを追加**（§6-1）。
- 複数ステップを 1 Undo トランザクションに束ねる（§6-3）。
- localhost バインド＋トークン認証＋破壊的操作の確認ゲート（§5）。
- **受入基準**：知覚し、行動し、安全に取り消せるエージェント。

### Phase 3（任意）— Nyquist 生成 DSP
- 「LLM が Nyquist コードを生成 → Nyquist Prompt 経由で実行」。Nyquist は 3.x で結線済み・`xsystem` 無効。ファイル I/O ゲートを付ける（§5）。デモ映え大。

### Phase 4 — 製品化
- 構造化エラー、長時間処理の進捗ストリーム、ツール別権限ポリシー、パッケージング/署名、既定 OFF 出荷、ドキュメント。

---

## 8. ビルド統合（Audacity 3.x）

- 3.x は **CMake + conan**（依存管理は conan。`audacity3` ブランチの root `CMakeLists.txt` と `cmake-proxies/` 参照）。Mac/Win 両対応の公式 `BUILDING.md` あり。
- `mod-script-pipe` は `audacity_module(mod-script-pipe ...)` マクロで宣言され（`modules/mod-script-pipe/CMakeLists.txt`）、`modules/CMakeLists.txt` の `FOLDERS`/`MODULES` リストで束ねられる。新規 `mod-mcp-server` も**同じマクロ 1 ブロック＋リストに 1 行**で追加できる（侵襲度：極小）。
- HTTP ライブラリ（`cpp-httplib`）はヘッダオンリーなので `modules/mod-mcp-server/` 直下に置いて `#include` するだけ。conan を汚さない。
- **macOS dev ビルド**：conan が依存を解決。Apple Silicon（arm64）/ Intel 両対応。`cmake -G Ninja` 推奨。

---

## 9. 並行する Ardour トラック（文脈と転用できる教訓）

別途、**Ardour の "fresh"（クリーンルーム）MCP コントロールサーフェス**（macOS、GPLv2-or-later）を構築中。Ardour には既に実験的 `mcp_http` サーフェス（約 95 ツール、libwebsockets、`POST /mcp`）が存在するが、**スレッド未整流（lws サービススレッドから直接 Session 変更 → Undo 破壊・debug ビルドで assert クラッシュ）**という欠陥を持つ。そこから得た、Audacity 実装にも効く教訓：

1. **スレッド整流は最初から**：Audacity は §3.7 で既に正しい（メインスレッドへ marshal）。この優位を崩さない。
2. **ターン制ロック・モデル**：LLM と人間の編集を排他するターン制にすると、双方向の知覚ループ（push 通知）が不要化し、Undo を「1 ターン＝1 トランザクション」で扱える。「ライブ表示」は捨てず「同時編集」だけ捨てるのが正解（Codex のターン制と一致）。Audacity の §6-3 Undo グルーピングはこのロック境界＝バッチ境界として実装するのが綺麗。
3. **ツールは自己記述から自動生成**：Ardour は手書きだったが、Audacity は `GetInfo` JSON で自動生成できる（本プロジェクトの優位）。
4. **安全性**：localhost バインド・トークン・既定 OFF・破壊的操作ゲートは両者共通。

> Ardour 版の整流パッチ設計（`call_slot` + condvar マーシャリング）とターン制ロック状態機械の詳細は、作業ホストの `llm-daw-report/fix_plan.pdf`（改訂 v2）にある。Audacity では §3.7 の既存機構で同等の安全性が無償で得られる点が重要。

---

## 10. 主要ファイル・マップ（3.x の `audacity3` ブランチ起点）

| 役割 | パス（4.0 表記 → 3.x は `au3/` を外す） | 何をするか |
|---|---|---|
| transport（複製元）| `au3/modules/scripting/mod-script-pipe/{PipeServer,ScripterCallback}.cpp` | 名前付きパイプサーバ。`ModuleDispatch` でリレー起動 |
| コマンド実行リレー（無改変再利用）| `au3/src/commands/ScriptCommandRelay.cpp:33,63` | 文字列→`AppCommandEvent`→メインスレッド実行→応答 |
| コマンド構築 | `au3/src/commands/CommandBuilder.cpp:154-161` | 全コマンドを `BatchCommand` へ |
| ディスパッチ | `au3/src/commands/CommandDispatch.cpp:28` / `au3/src/BatchCommands.cpp:585` | メニュー/エフェクトへ振り分け |
| 自己記述 JSON | `au3/src/commands/GetInfoCommand.cpp` | `tools/list` の自動生成元 |
| 型付き引数 | `au3/src/commands/SelectCommand.cpp:71-80`（`VisitSettings` 例）| inputSchema の型情報 |
| 結果契約 | `au3/src/commands/Command.cpp:154-158` | `finished: OK/Failed!` |
| カタログ統合 | `au3/src/BatchCommands.cpp:303,333` | 29+212+35 を統合 |
| スレッド整流 | `au3/.../CommandTargets.h:217-226`（`ResponseTarget`/`wxSemaphore`）| メインスレッド同期 |
| モジュール ABI | `au3/libraries/au3-utility/ModuleConstants.h:26-40` | `GetVersionString`+`ModuleDispatch` |
| モジュールロード | `au3/libraries/au3-module-manager/ModuleManager.cpp:143-151` | `wxDynamicLibrary` |
| Nyquist（任意 DSP）| `au3/libraries/au3-nyquist-effects/NyquistBase.cpp` ＋ Nyquist Prompt UI | LLM 生成 DSP の実行口 |

---

## 11. ライセンス（フォーク公開する場合の要点）

- **Audacity 3.x コアは GPLv2-or-later**（4.0 の Muse フレームワークが課す GPLv3-only ロックや Qt LGPL 再リンク義務が**無い分、4.0 より軽い**）。VST3 を含むビルドは VST3 SDK の GPLv3 オプションにより実効 GPLv3 になり得る。
- **GPL は「無料」でなく「自由」**：有料配布は GPL §4 で明示的に許可（売ってよい）。あなたへの義務は「課金の可否」ではなく、(1) 渡した相手への**完全ソース提供**、(2) 相手の自由を**制限しない**、(3) **表示の保持**、(4) 該当時の特許グラント。**全世界公開ではなく受領者への提供**で足りる。
- **商標リブランド必須**：`"Audacity"` 名・ロゴは Muse Group の登録商標（GPL とは独立）。配布バイナリは改名・アイコン差し替えが必要（Tenacity/Audacium 前例）。
- **CLA はフォークを妨げない**：CLA は上流貢献用。スタンドアロンのフォーク公開＋PR 非提出なら署名不要。`*-CLA-applies` マーカーは剥がさず保持。
- **テレメトリ**：3.x の audio.com/更新チェック系は無効化・自前プライバシーポリシーを推奨。
- 詳細は作業ホストの `llm-daw-report/license_report.pdf`（ライセンス・コンプライアンス）に網羅。

---

## 12. 実装者への最初の具体ステップ

```bash
# 1) 3.x をチェックアウト（dark-build を回避）
cd <audacity-repo>
git fetch origin
git checkout -b mcp-llm audacity3      # もしくは release-3.2.5

# 2) 素の 3.x をビルドして「動く土台」を確認（BUILDING.md 参照、CMake+conan）
#    例: cmake --preset <platform> && cmake --build ...

# 3) Phase 0: mod-script-pipe を複製
cp -r modules/mod-script-pipe modules/mod-mcp-server
#    - ScripterCallback を ModuleDispatch ごと流用（ExecFromWorker は無改変）
#    - PipeServer を cpp-httplib の POST /mcp に置換
#    - modules/CMakeLists.txt の FOLDERS/MODULES に mod-mcp-server を1行追加
#    - dispatch_jsonrpc 相当を実装（initialize/tools/list/tools/call）

# 4) tools/list は GetInfo: Type=Commands Format=JSON を内部発行して自動生成（Phase 1）
```

外部 LLM クライアント設定例（Phase 0 検証用）：
```bash
codex mcp add audacity --url http://127.0.0.1:<port>/mcp
# Claude Desktop なら mcp-remote 経由で http://127.0.0.1:<port>/mcp
```

---

## 13. 未決事項（実装者が決める）

1. **正確な 3.x 起点**：`audacity3`（最新 3.x 開発）か `release-3.2.5`（最も安定）か。安定優先なら後者。
2. **HTTP ライブラリ**：`cpp-httplib`（推奨・MIT・ヘッダオンリー）／素 socket／libwebsockets（Ardour と統一したい場合）。
3. **メニューコマンド（約212）を tools に含めるか**：まずは型付き `AudacityCommand`（29）だけで実用。後で拡張。
4. **将来の Muse 移植時期**：3.x で実証後、4.0 のアーキ安定を待って `IActionsDispatcher` へ。

---

## 14. 来歴・検証メタデータ

- 解析対象コミット：Audacity `caa9b9fdc`（4.0.0-alpha、au3 サブツリーを 3.x の代理として精読）／Ardour `9.7-88-gb25a63c74a`。
- 主要な定量値は `grep`/`wc` と一次読解で検証。dark-build（§1.1）は `src/au3wrap` の CMake を直接確認して決着。
- 補足レポート（作業ホスト `/Volumes/work-ssd-4TB-USB4/_Git_Repository/llm-daw-report/`、本リポ外）：`Audacity_..._改造可能性レビュー.pdf`、`license_report.pdf`、`Ardour_..._改造可能性調査.pdf`、`fix_plan.pdf`（Ardour 整流＋ターン制ロック）。**本ハンドオフはこれらに依存せず単体で完結**。
- 本ハンドオフはエンジニアリング分析であり、ライセンス §11 は**法的助言ではない**（公開前に弁護士確認を推奨）。

---

*End of handoff. 次に着手する LLM へ：§12 から始め、Phase 0 の「動くデモ」を最優先で通すこと。最大の差別化（§4.3 の `GetInfo`→`tools/list` 自動生成）が効いた瞬間に、このプロジェクトの価値が立ち上がる。*
