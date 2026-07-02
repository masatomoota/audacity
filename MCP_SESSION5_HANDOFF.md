# MCP_SESSION5_HANDOFF

このドキュメントは、別の LLM が `mcp-llm` ブランチの現在地点から Otis 開発を
完全に再開できるよう、セッション 5 の成果・検証済み挙動・設計判断・残作業を
引き継ぐためのものです。ファイル参照とコマンドを厳密に書きます。

- ブランチ: `mcp-llm` (リモート: `fork` = https://github.com/masatomoota/audacity.git)
- 基底コミット: `8d987a0cf` (SESSION4 終了時点)
- 本セッションのコミット: `445eb2fb2` → `bd41821ef` → `934fe7bda` (+ 本 handoff のコミット)
- 対象ホスト: macOS Apple Silicon, AU(GPLv2) ビルドのみ
- デプロイ成果物: `build/Otis-MCP.dmg` (28MB, 本セッションで再生成・マウント検証済み)

## 関連ドキュメント (参照のみ、重複させない)

- `MCP_SESSION4_HANDOFF.md` — 前セッション。**注意: §既知の地雷のうち 3 件は本セッションで根治済み** (下記参照)
- `MCP_SESSION3_HANDOFF.md` / `MCP_SESSION3_BUG_REVIEW.md` — core バグ監査
- `BUILD_HANDOFF.md` — クリーンビルド手順の正本
- `OTIS_REBRAND_HANDOFF.md` / `OTIS_UI_COLOR_HANDOFF.md` — リブランド/カラー

## 本セッションのテーマ: 安定化 wave (監査 → 修正 → 検証 → デプロイ)

「とりあえず動く」レベルだった LLM 操作の安定性を、多段 wave の
マルチエージェント作業で底上げした。監査は 4 並列 (C++ MCP モジュール /
スクリプティング・ディスパッチ層 / companion Python / frontend)、修正は
ファイル非衝突の 3 並列 + スクリプティング層 1 本、検証は実機スモークテスト
18 項目 (18/18 PASS、`scripts/mcp-smoke-test.py` として保存)。

## コミットごとの要点

### 445eb2fb2 — mcp: harden mod-mcp-server

対象: `modules/scripting/mod-mcp-server/` のみ。

1. **relay タイムアウト (最重要)**: `MCPHttpServer::ExecCommand`
   (`MCPHttpServer.cpp` ~line 132) を再設計。relay 呼び出し (`mExecFn`) を
   detached `std::thread` に移し、そのスレッドが `std::timed_mutex` の lock を
   move capture で保持。HTTP スレッドは `future.wait_for(300s)` で待つ。
   - モーダルダイアログ等でコマンドが返らない場合: 300s で当該リクエストは
     タイムアウトエラーになり、後続リクエストは `try_lock_for(5s)` の失敗で
     即座に "Previous command still executing — Otis may be showing a modal
     dialog..." を受け取る (永久 wedge の廃止)
   - ダイアログが人間により閉じられると relay スレッドが完走して mutex を
     解放し、**再起動なしで全自動復旧する** (実機検証済み、下記 §検証 E)
   - `RelayJob` は `shared_ptr` 保持なので、タイムアウト後に relay が
     書き込んでも use-after-free しない
2. **失敗検出の拡張**: `ResponseIsFailed()` が空レスポンス (trim 後) と
   "Syntax error" / "Unrecognized parameter" / "Parameter string is missing" /
   "Invalid value for parameter" も failure と判定。空レスポンス時は LLM に
   "Empty response from Otis — is a project window open?" を返す。
   従来は構文エラーやプロジェクト無しが isError:false (成功) で返っていた
3. **id エスケープ**: `IdToString()` を `j["id"].dump()` に統一
   (引用符入り文字列 id で不正 JSON になるバグの修正)
4. **非オブジェクト args**: `tools/call` の arguments が object でなければ
   -32602 を返す (従来は素の HTTP 500)
5. **shutdown レース + ポート衝突**: `mStopRequested` (atomic) を listen 直前に
   確認。listen 失敗時は 3 秒 sleep してから return (StartScriptServer の
   `while(true)` 再入による bind busy-spin 防止)
6. `run_command` の tools/list description に quoting ルールと Export2 の
   全体フォールバックを明文化

### bd41821ef — scripting: 根治修正 3 件 (SESSION4 の地雷 1〜3 が根治)

1. **Filename 空白分割の根治** (`libraries/lib-components/EffectAutomationParameters.h` ~line 295):
   - 真因: `CommandParameters::SetParameters` が
     `wxCmdLineParser::ConvertStringToArgs(parms)` をデフォルトの
     `wxCMD_LINE_SPLIT_DOS` で呼んでいた (単引用符非対応)。さらに分割で
     生じた '=' なしトークンを黙って捨てるため、
     `Filename=/a b/x.wav` → `Filename=/a` に**静かに**切り詰められていた
   - 修正: `wxCMD_LINE_SPLIT_UNIX` (単/二重引用符の両対応) + '=' なし
     トークン検出で `SetParameters` が false を返す。コンストラクタが
     結果を `mWasWellFormed` に記録し `WasWellFormed()` で公開
   - 伝播: `AudacityCommand::LoadSettingsFromString`
     (`src/commands/AudacityCommand.cpp` ~line 136) と
     `Effect::LoadSettingsFromString` (`libraries/lib-effects/Effect.cpp`
     ~line 260) が `WasWellFormed()` を確認し、**非モーダルの `wxLogError`**
     を出して失敗を返す → LLM には "BatchCommand finished: Failed!" が届く
   - **設計判断 (重要)**: 修正エージェントは当初モーダル MessageBox を
     実装したが、マネージャーレビューで差し戻した。このパスの到達源は
     ほぼスクリプティングであり、モーダルは人間がダイアログを閉じるまで
     アプリを wedge させる (= 今回直した問題の再発源) ため
   - 注: wxCMD_LINE_SPLIT_UNIX ではバックスラッシュがエスケープ文字になる。
     本ビルドは macOS 専用なので Windows path の後方互換は問題にならない
   - 反証情報: SESSION4 の「切り捨て path をディレクトリとして再帰
     インポートし暴走」は監査で**コード上の裏付けなし**と判定
     (`Importer::Import` 経路に directory-walk は存在せず、wxFile::Open が
     ディレクトリで false を返してクリーンに失敗する)
2. **Export2 空セレクション根治** (`src/commands/ImportExportCommands.cpp`
   `ExportCommand::Apply` ~line 114): `t1 <= t0` なら全プロジェクト
   (t0=0, t1=`TrackList::GetEndTime()`, selectedOnly=false) にフォール
   バック。プロジェクトも空なら `context.Error(...)` で可視の失敗。
   → **プロンプトでの「Export2 の前に必ず SelectAll:」ワークアラウンドは不要になった**
3. **.wav なのに AIFF が書かれる問題の根治** (同ファイル ~line 156):
   `FindFormat(extension)` で解決済みの `formatIndex` が
   `.SetPlugin(plugin)` に渡されておらず、デフォルト 0 = macOS では
   `FMT_AIFF` になっていた (`modules/import-export/mod-pcm/ExportPCM.cpp`
   の `kFormats[]` は `__WXMAC__` で {AIFF, WAV} 順)。
   `.SetPlugin(plugin, formatIndex)` の 1 行修正

### 934fe7bda — mcp-companion: origin gate / turn 多重防止 / instructions バージョン管理

`server.py`:
- **CORS/Origin ゲート**: 全レスポンスの `Access-Control-Allow-Origin` を
  `*` → companion 自身のオリジンに変更。全 POST ルートで Origin ヘッダを
  検証し、外部オリジンは 403 (`_origin_allowed()`, `ALLOWED_ORIGINS`)。
  これにより他タブの悪意ページが `/api/approval` を叩いて shell 承認を
  勝手に通す攻撃経路を遮断。Origin ヘッダ無し (curl 等) は許可。
  traceback はクライアントに返さず stderr のみ (`str(e)` だけ返す)
- **turn 多重防止**: `_inflight_threads` set + lock。実行中スレッドへの
  二重 POST /api/chat は HTTP 409 + 日本語メッセージ。finally で確実に解除
- **タイムアウト整合**: turn/start と SSE idle を 900s → 4000s
  (stem/transcribe の subprocess timeout 3600s より大きく)。長時間の
  ステム分離が 15 分で偽エラーになる問題の解消
- **INSTRUCTIONS_VERSION = 2**: thread/start 成功時に
  `mcp-companion/thread_meta.json` (gitignore 済み、アトミック書き込み) に
  記録。`/api/threads` は各 thread に `instructionsVersion`、トップに
  `currentInstructionsVersion`。`/api/thread` も同様。
  **DEV_INSTRUCTIONS を変えたら必ず INSTRUCTIONS_VERSION を +1 すること**
- **ポート衝突**: bind を Codex warm-start より**前**に移動 (2 個目の起動が
  codex subprocess を無駄に spawn しない) + OSError を親切な日本語
  メッセージ + exit 1 に
- **DEV_INSTRUCTIONS 更新** (= バージョン 2 の内容): スペース入り path の
  二重引用符必須ルールを追加。「Export2 前に必ず SelectAll:」を
  「選択が無ければ自動で全体が書き出される (明示したければ SelectAll: 可)」に緩和

`codex_bridge.py`: `stop()` の kill() フォールバック後に `wait(timeout=5)` でゾンビ回収。

`stem_mcp_server.py` / `transcribe_mcp_server.py`: `_run_with_pgroup_kill()` —
`start_new_session=True` の Popen + timeout 時 `os.killpg(SIGKILL)`。
torch/ONNX のワーカー子プロセスがタイムアウト後に生き残る問題の解消。

`index.html`:
- 旧 instructions スレッドに「旧」バッジ (一覧) + 警告バナー (スレッド表示)。
  API にバージョンフィールドが無い場合は非表示 (graceful degradation)
- スレッド読込失敗時に「空スレッド」と誤表示 → エラー表示 + 再試行ボタン
- toolCard の id 欠落時キー衝突修正 (連番フォールバック + data-open 追跡)
- SSE の複数 data: 行を仕様どおり改行連結

## 検証済みのランタイム挙動 (実機、再実証不要)

スモークスイート: `scripts/mcp-smoke-test.py` — **18/18 PASS**。
前提: Otis 起動済み・空プロジェクト。主要な検証点:

- **A. 空セレクション Export2**: SelectNone: の直後の
  `Export2: Filename="/tmp/otis smoke test/out one.wav"` が 882,044 byte の
  ファイルを出力 (10 秒トーン、旧挙動は 410 byte のヘッダーのみ)
- **B. WAV が本当に RIFF/WAVE**: 出力先頭 12 byte = `RIFF....WAVE`
  (旧挙動は .wav 拡張子でも AIFF)
- **C. quoting**: スペース入り path が二重引用符でも単引用符でも
  Import2/Export2 で機能。**未クオートは可視のエラー**
  ("BatchCommand finished: Failed!") になり、静かな誤動作をしない
- **D. エラー後に wedge しない**: 未クオートエラーの直後のコマンドが正常実行
- **E. モーダル wedge からの自動復旧** (スモーク外で実証):
  トラック 0 本の状態で `Tone:` を実行 → Otis が「『トーン』を使うには
  1 トラック以上選択する必要があります。」モーダルを表示 → 当該 MCP
  リクエストは 300s タイムアウト、後続は fail-fast エラー →
  `osascript -e 'tell application "System Events" to tell process "Audacity"
  to click button 1 of front window'` でダイアログを閉じると
  **再起動なしで全コマンドが復旧**
- **F. companion**: GET / 200、CORS ヘッダが固定オリジン、外部 Origin の
  POST が 403、同一オリジン POST は通過、2 個目の起動が warm-start 前に
  日本語メッセージで exit 1

## 既知の地雷 (SESSION4 から更新)

- ~~Import2/Export2 の Filename 空白分割~~ → **根治済み** (quoting 対応 +
  可視エラー化)。ただし「スペース入り値は要引用符」のルール自体は残る
  (未クオートは今はエラーになる)
- ~~Export2 の空セレクション → ヘッダーのみファイル~~ → **根治済み** (全体フォールバック)
- ~~Export2 が .wav でも AIFF を書く~~ → **根治済み** (formatIndex 伝達)
- **モーダルダイアログを出すコマンドはそのコマンド自体は失敗する**:
  例 `Tone:` をトラック未選択で実行。MCP 層は 300s タイムアウト +
  fail-fast + 自動復旧で守られているが、ダイアログを閉じるのは人間 (または
  osascript)。**生成系 (Tone/Chirp/Noise) は先に `NewMonoTrack:` +
  `SelectTracks:` を実行すること** (スモークテスト参照)。スクリプト実行中の
  モーダル抑止は未着手の将来課題
- **古いチャットスレッドの stale DEV_INSTRUCTIONS**: UI で可視化済み
  (「旧」バッジ)。ただし resume 時の developerInstructions 再送は
  Codex app-server API の対応が不明なため未実施。新規スレッド誘導が現時点の解
- macOS のクラッシュリカバリダイアログが起動を阻害する場合:
  `killall Audacity && mv ~/Library/Application\ Support/audacity/SessionData
  ~/Library/Application\ Support/audacity/SessionData.bak` (SESSION4 と同じ)
- `open -na "Google Chrome" --args --app=URL` は既存 Chrome に merge されて
  `--app` が落ちる → バイナリ直接起動 (SESSION4 と同じ、変更なし)

## 再現手順

### 1. ビルド

```
cmake --build /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build -j
```

本セッション終了時点でビルド成功 (63/63)。クリーンビルドは `BUILD_HANDOFF.md` §3。

### 2. 起動 + スモークテスト

```
open /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp/build/RelWithDebInfo/Audacity.app
# MCP が 127.0.0.1:4830 で立つのを数秒待つ
python3 scripts/mcp-smoke-test.py   # 18/18 PASS を期待
```

注意: スモークはプロジェクトにトラックを追加する。実行後はプロジェクトを
破棄してよい。killall で終了した場合は次回起動時に SessionData を退避。

### 3. companion

```
cd mcp-companion && ./run.sh        # ポート 8765
```

エンドツーエンドのステム分離手順は SESSION4 §再現手順 4 と同じだが、
**`SelectAll:` の前置はもう必須ではない**。

### 4. デプロイ (DMG)

```
./scripts/make-dmg.sh               # → build/Otis-MCP.dmg
```

本セッションで実行済み: 28MB、マウントして Otis.app (76MB, arm64,
ad-hoc 署名) と companion 一式・ランチャー・README を確認済み。

## 未解決 / 次の優先候補 (約束ではなく提案)

1. **スクリプト実行中のモーダル抑止**: コマンドが scripting 経由のとき
   `AudacityMessageBox` / `BasicUI::ShowMessageBox` を抑止してエラー文字列を
   レスポンスに載せる仕組み。relay 側の防御 (300s + fail-fast + 自動復旧) は
   済んでいるので、残るは UX 改善
2. **MCP ツール面の拡張**: 現在は `run_command` + `get_info` の 2 つだけの
   stringly-typed API。頻出操作 (import/export/select/effect) を構造化引数の
   専用ツールにすると LLM の失敗率がさらに下がる
3. ステム/トランスクライブのトラック命名整理 (SESSION4 からの持ち越し。
   `Import2` 後に `SetTrackStatus:` 系でリネームするのが最小侵襲)
4. レスポンシブ監査 1024–1680px (持ち越し。監査済み情報: `#main` に
   `min-width:0` があり構造破綻は静的解析上なし、@media は皆無)
5. 真の Otis.app バンドル化 (`OTIS_REBRAND_HANDOFF.md` 参照、持ち越し)
6. `ResponseIsFailed` は依然ヒューリスティック。CommandBuilder から本物の
   成否 bool を plumbing すればさらに堅牢 (中規模)
7. thread/resume 時の developerInstructions 再送可否を Codex app-server の
   API ドキュメントで確認

## このセッションの進め方 (次の LLM への文脈)

ユーザーの方針: wave 型のマルチエージェント進行。監査・機械的修正は安価な
モデル (sonnet) のサブエージェントに委任し、高価なモデルはマネージャー
(トリアージ・diff レビュー・設計差し戻し・検証) に徹する。人間の判断が
不要な限り自動で次 wave へ進む。デバッグ完了後はデプロイ (make-dmg.sh) +
handoff 作成 + GitHub (fork/mcp-llm) への push まで行う。

以上。
