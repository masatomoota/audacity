# Audacity MCP — Phase 0 実装ハンドオフ（**完了・実機検証済み**）

> **ABSTRACT (English, for any LLM picking this up cold):** Phase 0 of the "MCP server inside Audacity" project is **DONE and end-to-end verified**. A new built-in module `mod-mcp-server` exposes an MCP (JSON-RPC 2.0) server over localhost HTTP (`POST http://127.0.0.1:4830/mcp`) and drives Audacity by reusing the existing `ScriptCommandRelay` (unchanged) — so all command execution is marshalled onto the GUI/main thread exactly like `mod-script-pipe`. It builds cleanly against **standalone Audacity 3.7.7** and a live external HTTP client successfully ran the full **Select → Generate → Effect → Export2** pipeline. This document is self-contained: it carries the exact repo layout, build commands, toolchain workarounds, runtime-enable procedure, the live verification transcript, known gotchas, and the Phase 1 plan. Prose is Japanese; all paths/identifiers/commands/code are English with `file:line` citations. The companion planning doc is `MCP_LLM_CONTROL_HANDOFF.md` (same directory). Start at §1.

---

## 1. 現状サマリ（一目で）

- **対象**: Audacity **専用**プロジェクト（Ardour は無関係・不可侵）。
- **達成**: handoff の **Phase 0 受入基準（外部 LLM が Select→Effect→Export2 を端から端まで実行）を実機で達成**。
- **土台**: standalone Audacity **3.7.7**（`audacity3` ブランチ起点）。4.0-alpha の dark-build 問題を回避済み（`au4/` は root CMake の `add_subdirectory` に含まれず＝ビルド対象外を確認）。
- **新規モジュール**: `modules/scripting/mod-mcp-server`（`mod-script-pipe` の完全ミラー配置）。**既存コードの改変は CMake 登録 1 行のみ。** `ScriptCommandRelay` は無改変再利用。
- **ビルド**: クリーン成功（モジュール固有の警告ゼロ）。`mod-mcp-server.so` (795 KB) がアプリバンドルの `Contents/modules/` に配置される。

---

## 2. リポジトリ／ブランチ／worktree レイアウト

| 場所 | ブランチ | 内容 |
|---|---|---|
| `/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-3x` (**作業 worktree**) | `mcp-llm`（`origin/audacity3` 起点, HEAD `2cbd4c41c`） | **本 Phase 0 実装の本体**。新モジュール＋両ハンドオフ。 |
| `/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity` (元 checkout) | `mcp-llm-handoff`（4.0-alpha `7ad9b3818`） | 当初の計画ハンドオフのみ（4.0 ツリー上）。 |
| build dir（out-of-tree） | — | `/Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-3x-build`（worktree を汚さない） |

- worktree は `git worktree add -b mcp-llm ../audacity-3x origin/audacity3` で作成。
- **git remotes**: `origin` = `github.com/audacity/audacity`（公式・push しない）、`fork` = `github.com/masatomoota/audacity`（**同期先**）。
- `mcp-llm` は `audacity3` 起点のクリーンな feature ブランチ（差分＝新 dir ＋ CMake 1 行）＝安全にマージ可能。

---

## 3. ビルド手順（再現可能・検証済み）

### 3.1 ツールチェーン（このホストの実績値）
- macOS arm64, **Xcode 26.3 (apple-clang 17)**, **CMake 4.3.4** (homebrew), **Ninja 1.13.2** (`brew install ninja`), **Conan 2.29.1**（`python3 -m pip install --user conan` → `conan profile detect`）, python3 `/usr/bin/python3`。
- **PATH（毎回必要）**: `export PATH="/Users/masatomo/Library/Python/3.9/bin:/opt/homebrew/bin:$PATH"`

### 3.2 configure ＋ build
```bash
export PATH="/Users/masatomo/Library/Python/3.9/bin:/opt/homebrew/bin:$PATH"
cmake -G Ninja \
  -S /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-3x \
  -B /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-3x-build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-3x-build -j
```

### 3.3 ツールチェーン・ワークアラウンド（重要）
- **`-DCMAKE_POLICY_VERSION_MINIMUM=3.5`** が必須。CMake 4.x は `cmake_minimum_required(VERSION <3.5)` を拒否するため（VST3 SDK 等の vendored cmake が該当）。
- **`-Werror` は不要**。プロジェクトは警告をエラー化しない。ソース改変ゼロで vanilla がビルド可能。
- 初回 configure は Conan が全 27 依存を **source ビルド**（apple-clang 17 のプリビルドが無い）で約 9 分、compile 約 25–30 分。2 回目以降は Conan キャッシュで高速。
- 成果物: `…/audacity-3x-build/RelWithDebInfo/Audacity.app`（arm64, 約 17MB バイナリ）。

---

## 4. 実装したもの：`modules/scripting/mod-mcp-server`

### 4.1 設計（核心）
`mod-script-pipe` を複製し、**transport だけ**を「名前付きパイプ」→「localhost HTTP/JSON-RPC」に置換。コマンド実行は `ScriptCommandRelay` を**無改変**で再利用するため、**HTTP worker スレッド → `AppCommandEvent` ＋ `wxSemaphore` → メインスレッド実行**というスレッド整流をそのまま継承する（worker からプロジェクト状態を直接触らない＝安全）。

実行パスの核心（`src/commands/ScriptCommandRelay.cpp:33-92`）:
```cpp
// ExecCommand(pIn, pOut, fromMain=false):
if (auto pProject = ::GetActiveProject().lock()) {     // ← アクティブ project が無いと空返し（§6 の罠）
   CommandBuilder builder(*pProject, *pIn);
   if (builder.WasValid()) { AppCommandEvent ev; ev.SetCommand(builder.GetCommand());
                              wxTheApp->AddPendingEvent(ev); }   // メインスレッドへ
   *pOut = builder.GetResponse();                       // semaphore でブロックし応答取得
} else *pOut = wxString{};
// StartScriptServer は detached thread で while(true){ function(ExecFromWorker); }
```
本モジュールの `ModuleInitialize` は `ScriptCommandRelay::StartScriptServer(RegMcpServerFunc)` を呼び、`RegMcpServerFunc(ExecFromWorker)` が `ExecFromWorker` ポインタを保存して HTTP listen ループ（`gMcpServer.Start(4830)`）に入る。これは `mod-script-pipe` の `RegScriptServerFunc → PipeServer()` と同型。

### 4.2 ファイル構成
| ファイル | 役割 |
|---|---|
| `MCPServerCallback.cpp` | モジュール ABI（`GetVersionString` via `DEFINE_VERSION_CHECK` ＋ `ModuleDispatch`）。`ModuleInitialize` で StartScriptServer、`AppQuiting`/`ModuleTerminate` で `gMcpServer.Stop()`。`RegMcpServerFunc` が relay と HTTP を橋渡し。 |
| `MCPHttpServer.h/.cpp` | cpp-httplib サーバ。`127.0.0.1:4830` の `POST /mcp`。JSON-RPC 2.0 dispatch（`initialize`/`notifications/initialized`/`ping`/`tools/list`/`tools/call`）。`ExecCommand` が relay を `std::mutex` で直列化して呼ぶ。 |
| `CMakeLists.txt` | `audacity_module(mod-mcp-server "${SOURCES}" "${LIBRARIES}" "${DEFINES}" "")`（`Audacity` 本体に PRIVATE リンク、`mod-script-pipe` と同型）＋ vendored ヘッダの include dir 追加。`DEFINES = BUILDING_MCP_SERVER, wxDEBUG_LEVEL=0`。 |
| `README.md` | モジュール概要。 |
| `lib/cpp-httplib/httplib.h` | vendored **cpp-httplib 0.18.3**（MIT, header-only）。 |
| `lib/nlohmann-json/json.hpp` | vendored **nlohmann/json 3.11.3**（MIT, header-only）。 |

登録（唯一の既存ファイル改変）: `modules/scripting/CMakeLists.txt` の `set(MODULES …)` に `mod-mcp-server` を 1 行追加。

### 4.3 MCP プロトコル / ツール（Phase 0）
- `initialize` → `{protocolVersion:"2025-03-26", capabilities:{tools:{listChanged:false}}, serverInfo:{name:"audacity-mcp", version:"0.1.0"}}`
- `ping` → `{}` / `notifications/initialized` → 202（応答なし）
- `tools/list` → **静的 2 ツール**:
  - `run_command` `{command: string}` — 任意の Audacity コマンド文字列を実行（例 `"Select: Start=0 End=10"`）。
  - `get_info` `{type?: string="Commands", format?: string="JSON"}` — `GetInfo: Type=… Format=…` を実行。
- `tools/call` → 上記をコマンド文字列へ整形し relay 実行。応答を `content[].text` に格納、`"Failed!"` 含有時は `isError:true`。

---

## 5. 実機での有効化・起動・検証（**再現手順**）

### 5.1 モジュール有効化（GUI 不要）
新規モジュールは既定で `kModuleNew`＝**ロードされない**（`libraries/lib-module-manager/ModuleSettings.cpp:95-142`, enum: Disabled=0/Enabled=1/Ask=2/Failed=3/New=4）。`mod-script-pipe` 同様 `autoEnabledModules()` に**含めない**（ネットワークを開くので手動有効化が正しい）。有効化は preference 3 点：
- `/Module/mod-mcp-server = 1`、`/ModulePath/mod-mcp-server = <…/Contents/modules/mod-mcp-server.so>`、`/ModuleDateTime/mod-mcp-server = <.so の mtime を FormatISOCombined>`。
- **datetime が .so の実 mtime と一致しないと `kModuleNew` にリセットされロードされない**（`ModuleSettings.cpp:114-126`）。

ユーザーの実プロファイルを汚さない隔離は **Portable Settings**（`libraries/lib-files/FileNames.cpp:287-303`）: `Audacity.app/Contents/Portable Settings/audacity.cfg` を置けばそこが config dir になる。検証で使った確実な手法 = `.so` の mtime を固定値に `touch` し、cfg に同値を書く：
```bash
APP=…/audacity-3x-build/RelWithDebInfo/Audacity.app
MODSO="$APP/Contents/modules/mod-mcp-server.so"
touch -t 202606251200.00 "$MODSO"
mkdir -p "$APP/Contents/Portable Settings"
cat > "$APP/Contents/Portable Settings/audacity.cfg" <<EOF
[Module]
mod-mcp-server=1
[ModulePath]
mod-mcp-server=$MODSO
[ModuleDateTime]
mod-mcp-server=2026-06-25T12:00:00
EOF
"$APP/Contents/MacOS/Audacity" >/tmp/aud.log 2>&1 &   # GUI 起動
```

### 5.2 検証 curl（実際に通った）
```bash
MCP=http://127.0.0.1:4830/mcp
# 起動待ち（foreground sleep 不要）:
curl -s --retry-connrefused --retry 40 --retry-delay 1 --max-time 6 -X POST "$MCP" \
  -d '{"jsonrpc":"2.0","id":1,"method":"initialize"}'
# → serverInfo audacity-mcp 0.1.0 が返れば server UP
curl -s -X POST "$MCP" -d '{"jsonrpc":"2.0","id":2,"method":"tools/list"}'   # run_command + get_info
# tools/call ヘルパ: name, argsjson
# e2e（成功実績）:
#  get_info Commands  → 65,925 bytes のコマンドカタログ JSON（Phase 1 の土台）
#  run_command NewMonoTrack                         → BatchCommand finished: OK
#  run_command "Select: Start=0 End=3"              → OK
#  run_command "Tone: Frequency=440 Amplitude=0.8 Waveform=Sine" → OK
#  get_info Tracks → {"name":"オーディオ 1","kind":"wave","start":0,"end":3,"channels":1}
#  run_command "Amplify: Ratio=0.5"                 → OK
#  run_command 'Export2: Filename="/tmp/mcp_phase0.wav" NumChannels=1' → Exported …: OK（265KB 実オーディオ生成）
```
tools/call 形式: `{"jsonrpc":"2.0","id":N,"method":"tools/call","params":{"name":"run_command","arguments":{"command":"…"}}}`

---

## 6. 既知の罠・注意点（次の人が必ず踏むもの）

1. **初回起動のプラグインスキャンが致命的に紛らわしい**：fresh profile での初回起動は、ホストにインストール済みの全 VST/VST3/AU をスキャンし、「プラグインを検索中」モーダルが OnInit をブロックする。**この間はアクティブ project が無く、全 `tools/call` が即・空文字列を返す**（ハングではない＝`GetActiveProject()` が null の経路）。スキャン完了後（registry が Portable Settings にキャッシュされ次回以降スキップ）に project がアクティブになり、コマンドが正しく動く。**「空応答＝バグ」ではない**。osascript（Accessibility 許可済み）でウィンドウ名 "新規プラグイン"/"Audacity へようこそ!"/"Audacity" を確認でき、`key code 53`(Esc) で welcome 系を閉じられる。
2. **Export2 のフォーマット**：`Filename="…wav"` でも実ファイルが AIFF になるケースを観測（`file` → "AIFF audio"）。Export は成功し実オーディオは出る。Audacity 側 Export2 の format 解決の癖で、本モジュールのコードとは無関係。必要なら `Export2` の format 指定を精査。
3. **サーバは現状モジュールロード時に自動起動**（port 4830）。handoff §5 の「既定 OFF・トークン認証・破壊的操作ゲート」は **Phase 2/4 の TODO**（現状は localhost バインドのみ）。
4. **並行リクエスト**：cpp-httplib は多スレッド。relay は単一コマンド契約なので `MCPHttpServer::ExecCommand` を `std::mutex mExecMutex` で直列化済み。
5. **shutdown busy-spin 回避**：`StartScriptServer` の `while(true)` 対策として、`RegMcpServerFunc` の shutdown ガードに 100ms sleep を入れてある。
6. **vendored ヘッダの typedef**：`tpExecScriptServerFunc`(relay) と `tpMcpExecFunc`(本モジュール) は同一シグネチャ。`SetExecFunc` 呼び出しで `reinterpret_cast<tpMcpExecFunc>` を入れてある（防御的）。

---

## 7. 次のステップ（Phase 1 以降）

- **Phase 1 — `tools/list` 自動生成（最大の差別化）**：`get_info Type=Commands Format=JSON` が既に **65KB の完全カタログ（id/name/params[key,type,default,enum]）** を返すことを実証済み。これを JSON-Schema にマップして約 250+ ツールを手書きゼロで生成する。型対応表（`double`→number, `bool`→boolean, `string`→string, `enum`→string+enum）を 1 つ用意。`AddBool` の真偽値クォート癖（原典 handoff §3.5）を吸収。
- **Phase 2 — 知覚・安全・Undo**：`get_info` の Tracks/Clips/Labels/Selection を MCP resources 化（Tracks は既に動作確認済み）。メータ読み出し（原典 §6-1）。複数編集の 1-Undo トランザクション（§6-3）。**localhost 限定＋トークン認証＋破壊操作ゲート＋サーバ起動オプトイン**（§5）。
- **コード入口（3.x, `au3/` 接頭辞なし）**：transport 複製元 `modules/scripting/mod-script-pipe/`、relay `src/commands/ScriptCommandRelay.cpp`、自己記述 JSON `src/commands/GetInfoCommand.cpp`、結果契約 `src/commands/Command.cpp`、カタログ `src/BatchCommands.cpp`、モジュール ABI `libraries/lib-utility/ModuleConstants.h`、有効化 `libraries/lib-module-manager/ModuleSettings.cpp`。

---

## 8. 検証メタデータ

- 起点: `origin/audacity3` = Audacity **3.7.7**（worktree HEAD `2cbd4c41c`）。
- ビルド成功: 1497/1497 targets、モジュール固有警告 0。`mod-mcp-server.so` 795KB を `Contents/modules/` に確認。
- e2e: 外部 HTTP クライアント（curl）から `initialize`/`tools/list`/`ping`/`tools/call`（get_info, NewMonoTrack, Select, Tone, Amplify, Export2）全成功、`/tmp/mcp_phase0.wav` 265KB 生成。
- vendored: cpp-httplib 0.18.3, nlohmann/json 3.11.3（ともに MIT, GPLv2 互換）。
- 同期先 fork: `github.com/masatomoota/audacity`（公式 origin には push しない）。

---

## 9. アプリ内チャット = コンパニオンアプリ（in-process モジュールは断念）

**結論**: 「言葉で Audacity を操作するチャット」は Audacity 内蔵 UI ではなく **`mcp-companion/`（localhost Web チャット＝MCP クライアント）** で提供する。実機で OpenAI gpt-4o から `NewMonoTrack`→`Select`→`Tone`→`Amplify`→`Export2` を完通し `/tmp/companion_test.wav`（265KB 実オーディオ）生成を確認済み。

### 9.1 なぜ in-process モジュール（mod-ai-assistant）を断念したか
- 試作した内蔵モジュールは **ビルドは通るが、dlopen でロードするだけで Audacity の project ウィンドウ起動を破壊**した（メインスレッドのイベントループは生きるが active project が作られず、プラグインスキャンも走らない＝ログ空）。A/B テストで「メニュー登録でも `AttachedWindows::RegisteredFactory` でもなく、モジュール .o の**静的初期化（ロード時）**」まで絞り込んだが根治に至らず時間超過。`lib-network-manager` は本体内蔵で専用 dylib を持たず（symbols は `-bundle_loader` 経由）、可視性/重複初期化が絡む難所。
- **AU/VST プラグインも不可**: プラグインはオーディオ処理スロットで、ホスト（Audacity）を操作する API を持たない（トラック作成/エクスポート/コマンド実行ができず核が成立しない）。Audacity の VST は「選択範囲へのエフェクト」扱いで更に制約大。
- → **MCP クライアントのコンパニオン**が最も堅牢: Audacity 起動を一切壊さず、既存の動く `mod-mcp-server` を再利用、Claude Desktop/Codex と同じ正攻法。撤去後フォークは **mod-mcp-server のみの clean 状態**（`autoEnabledModules()` も mod-mcp-server のみ）。

### 9.2 コンパニオン構成（`mcp-companion/`, Python 3 stdlib のみ・pip 不要）
- `server.py` — `127.0.0.1:8765` の Web サーバ＋**サーバ側エージェントループ**（Anthropic Messages / OpenAI chat-completions 両対応、ツール= `run_command`/`get_info`、最大12ラウンド）。`.env` 自動読込（`$ENV_FILE` → スクリプト dir → CWD、既存 env は上書きしない）。ルート: `GET /`, `GET /api/config`, `GET /api/mcp_status`, `POST /api/chat`。
- `index.html` — チャット UI（会話バブル・ツール呼出の折りたたみ表示・MCP 接続インジケータ）。
- `run.sh` / `README.md` / `.gitignore`（`.env` 除外）。
- 設定(env): `AI_PROVIDER`(anthropic|openai), `AI_MODEL`(既定 claude-sonnet-4-6 / gpt-4o), `ANTHROPIC_API_KEY`/`OPENAI_API_KEY`, `OPENAI_BASE_URL`(Ollama/LM Studio 等), `MCP_URL`(既定 `http://127.0.0.1:4830/mcp`)。**キーは `.env`（.gitignore 済み・コミットしない）か env で渡す。**

### 9.3 使い方
1. Audacity（mod-mcp-server 有効）を起動（初回はプラグインスキャン完了まで待つ）。
2. `cd mcp-companion && ./run.sh`（または `python3 server.py`）→ ブラウザで `http://127.0.0.1:8765`。
3. チャットで「440Hzのトーンを3秒作って半分の音量にして書き出して」等と指示すると、LLM が `run_command`/`get_info` 経由で Audacity を駆動。
- 既知の小欠点: 非ストリーミング(v1)。`Export2` は `.wav` 指定でも AIFF を書くことがある（Audacity 側 Export2 の癖、コンパニオン無関係）。

---

*End of Phase 0 implementation handoff. 次の LLM へ：§5 の手順でビルド→有効化→検証を再現でき、§7 の Phase 1（`get_info`→`tools/list` 自動生成）から続行できる。アプリ内チャットは §9 のコンパニオン方式で完成済み。*
