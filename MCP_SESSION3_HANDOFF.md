# Audacity — Session 3 ハンドオフ（**コアバグレビュー & 安全な修正**）

> **ABSTRACT (English, for any LLM picking this up cold):**
> This session is **not** an MCP/UI/dist work — it is a **focused bug audit of the Audacity C++ core** (excluding `mcp-companion`, `mod-mcp-server`, and the `lib-src` vendor tree). The previous handoff `MCP_SESSION2_HANDOFF.md` documents the MCP/companion/UI/DMG track. This document covers everything done in the bug-review track during the same `mcp-llm` branch, applied on top of commit `018b9f529` (the HEAD of the MCP track at session start). Method: multi-agent sonnet finder → adversarial sonnet verifier → opus spot-check → on-disk fix → ninja build verification. Result: **32 source files changed (+379 −86), all compiled cleanly (exit 0, 0 errors), 0 stray edits outside the audit scope.** 6 real bugs were deliberately **NOT** fixed and 4 plausible-looking findings were **rejected as false positives** — those decisions are critical to preserve, so READ §6 and §7 before changing anything in those areas. The full per-finding rationale (~945 lines) lives in `MCP_SESSION3_BUG_REVIEW.md`; this document is the navigation index + actionable status. Prose is Japanese; all paths/symbols/code are English with `file:line` citations verified against the working tree at handoff time.

---

## 0. このハンドオフが前提とする状態

| 項目 | 値 |
|---|---|
| ブランチ | `mcp-llm` |
| ベースコミット（このセッション開始時の HEAD） | `018b9f529` (mcp-companion: interactive shell-approval UI …) |
| このセッションでのコミット | **このハンドオフのコミットでまとめて1本** （詳細は §3） |
| MCP/companion ファイル群 | **未変更**（別スレ担当）。`mcp-companion/`, `modules/mod-mcp-server/`, `stem_mcp_server.py` 等は触っていない |
| `lib-src/` ベンダーコード | **未変更**（スコープ外） |
| ビルド検証 | ✅ `ninja -C build` exit 0 / 0 errors（最終: 09:39 編集、09:40 ビルド完了） |
| 実行時テスト | ❌ 未実施（特に RT 系は要・実機テスト。§5 参照） |
| 詳細レポート | `MCP_SESSION3_BUG_REVIEW.md`（全 wave の証拠付き記録、945 行） |

### 前のハンドオフとの関係

| ファイル | 何をカバーするか |
|---|---|
| `MCP_LLM_CONTROL_HANDOFF.md` | Phase 0 以前の設計／Audacity 3.x 選定理由／ロードマップ |
| `MCP_PHASE0_IMPLEMENTATION_HANDOFF.md` | Phase 0（mod-mcp-server、知覚コマンド、Codex app-server コンパニオン）完了まで |
| `MCP_SESSION2_HANDOFF.md` | UI 修正 3 件・コンパニオン再構築・ステム分離・DMG 配布 |
| **本ドキュメント（MCP_SESSION3_HANDOFF.md）** | **MCP とは独立の作業**: Audacity コアの C++ バグ監査と安全な修正 |

---

## 1. このセッションでやったこと（要旨）

### 何を調べたか
- スコープ: `src/`, `libraries/`, `modules/` 配下の first-party C++（MCP 関連と `lib-src/` 除く）約 54 万行を 20 領域に分割。
- 領域: import/export パーサ群、aup3/SQLite プロジェクト I/O、XML シリアライザ、undo/redo、リアルタイム音声 I/O・スレッド、メモリ/サンプル管理、time-stretch ほか。

### どう調べたか（再現性のため）
1. **Wave 1 — finder + adversarial verifier**: 20 領域に sonnet finder → 各指摘に sonnet skeptic verifier。生 121 件 → confirmed 51 / rejected 70（誤検知自動除去）。
2. **Wave 2 — false-negative 回収**: 棄却 70 件を中立な sonnet 5並列で再判定 → 取りこぼし 5 件回収（うち HIGH 2）。
3. **Wave 3 — 最優先のみ修正**: HIGH 8 件のうち、私(opus)が実コードで再検証し**真かつ局所修正可**の 6 件のみ適用。`ImportPCM.cpp` の「無限ループ」と `ExportCL` の「シェル注入」は**誤検知**として却下。
4. **Wave 4 — 残 medium + RT2件**:
   - medium 18 ファイルを sonnet 並列で「再検証→真のみ修正」（22 件適用 / 2 件却下）。
   - H6 (`RealtimeEffectList::Visit` 競合) を copy-on-write で実装。
   - H1 (`AudioIO seek/stop` デッドロック) は upstream の意図設計で局所修正不可と判定 → 見送り。
   - RT medium 4 件のうち 2 件を私が revert（`RealtimeEffectManager::mSuspended` は upstream が同一修正を 2 日で revert 済み回帰のため、`RealtimeEffectState::Finalize` の「コピー削除」案は設定喪失リスクのため）。
5. **Wave 5 — 残3タスク調査＋安全対応**: A1(mSuspended)/A2(Finalize race)/H1/CheckVersion を sonnet 4並列で深掘り → A2 を安全な形（`Finalize(bool rtStopped)`）で実装、CheckVersion は死蔵コード整理（挙動不変）、A1/H1 は理由付きで見送り。
6. **ビルド検証**: `ninja -C build` で全 wave の最終状態をコンパイル確認（exit 0、エラー 0）。**実行時テスト未実施**。

### 数字
- 確定バグ: **約 59 件**（HIGH 10 / MEDIUM 32 / LOW 16+）
- 適用修正: **30+ 件 / 32 ソースファイル**（+379 −86 行）
- 誤検知として却下: **4 件**（重要：§7 で再導入を防ぐ）
- 真だが意図的に未修正: **6 件**（重要：§6 でテストと再開条件）

---

## 2. ビルド & 検証手順（次の LLM 用）

```bash
cd /Volumes/work-ssd-4TB-USB4/_Git_Repository/audacity-mcp

# 1. ビルド検証（このセッション時点で exit 0 確認済み）
ninja -C build
echo $?   # 0 を期待

# 2. 変更ファイル確認（MCP 系は出ない＝想定どおり）
git diff --stat -- '*.cpp' '*.h' | grep -vE "mcp-companion|stem_mcp"

# 3. アプリ起動（ユーザーの ./deploy.sh 承認済み — このリポは隔離・復旧可能）
./build/RelWithDebInfo/Audacity.app/Contents/MacOS/Audacity
```

ビルドは macOS の `install_name_tool` が `.so` 書き換えのため毎回再リンク扱いになる既知のクセあり（ninja dry-run が常に dirty を返す）。**問題ない**。エラー判定は `ninja` の exit code を信じてよい。

`compile_commands.json` は無いので clangd 等を使うなら別途生成（`cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON …`）。

---

## 3. 変更ファイル全インベントリ（カテゴリ別）

> 全 32 ファイル。`MCP_SESSION3_BUG_REVIEW.md` の対応節に詳細あり。各エントリは [bug-class] **問題** → **修正** の形式。

### 3.1 import/export パーサ（不正/敵対的ファイル耐性の向上）
| ファイル | line | 修正内容 |
|---|---|---|
| `modules/import-export/mod-pcm/PCM(ImportPCM).cpp` | — | （誤検知のため未変更。§7 参照） |
| `modules/import-export/mod-aup/ImportAUP.cpp` | 1007/1036/1069/1088 + 1099/1114 + 1028 | [null-deref] 空 `mHandlers` 4箇所ガード、HandleSequence の null waveclip ガード、null handler を `mClips` に積まない |
| `modules/import-export/mod-flac/ImportFLAC.cpp` | 255 | [UB] `1 << 31` 回避（`1u << (bps-1)`） |
| `modules/import-export/mod-ogg/ImportOGG.cpp` | — | （変更なし） |
| `modules/import-export/mod-opus/ImportOpus.cpp` | 207-211 | [OOB-read] `OP_HOLE` 時に `continue`（負値→size_t の AppendBuffer に渡る大穴を回避） |
| `modules/import-export/mod-opus/ExportOpus.cpp` | 739-743 | [logic] 出力バッファ下限 `nbStreams * 1275` で `OPUS_BUFFER_TOO_SMALL` 回避 |
| `modules/import-export/mod-mp3/ExportMP3.cpp` | 2023 | [logic] 末尾 ID3 書込みの閾値を `bytes`→`id3len` に修正（**末尾 ID3 付き MP3 エクスポートの誤エラー解消**） |
| `modules/import-export/mod-ffmpeg/ExportFFmpeg.cpp` | 1442-1449 | [logic] `got_output` で空パケット書込み回避 |
| `modules/import-export/mod-ffmpeg/ImportFFmpeg.cpp` | 593-594, 615-616 | [div-by-zero] `channelsCount<=0` ガード（int16/float 両経路） |
| `modules/import-export/mod-cl/ExportCL.cpp` | 538-582 | [integer-overflow] WAV RIFF 長を `UINT32_MAX` クランプ（>4GiB 巻き戻り回避）。**「シェル注入」は誤検知。§7 参照** |
| `modules/import-export/mod-lof/ImportLOF.cpp` | 271-306, 450-469 | [logic] **循環/過深ネスト `.lof` のスタック枯渇防止**（thread_local 集合 + 深さ 16 制限） |
| `libraries/lib-import-export/Export.cpp` | 99-105 | [logic] processor の出力先を temp パスに（**既存ファイル直接上書きの実バグを修正**、temp→target rename 機構を復活） |
| `libraries/lib-import-export/LibsndfileTagger.cpp` | 42-45 | [null-deref] `sf_close(nullptr)` ガード |
| `libraries/lib-import-export/GetAcidizerTags.cpp` | 67-74 | [logic] 短い LIST チャンクで `continue` 前にイテレータ前進（**起動ハング解消**） |
| `src/export/ExportOptionsHandler.cpp` | 159-171 | [null-deref] TypeRange の vector 範囲 + `get_if` null チェック |

### 3.2 プロジェクト I/O（aup3 / XML / SQLite）
| ファイル | line | 修正内容 |
|---|---|---|
| `libraries/lib-project-file-io/ProjectFileIO.cpp` | 1164-1183 | [data-loss] **`CopyTo()` の COMMIT 戻り値チェック**（ディスクフル時に Compact が空 DB を「成功」として元プロジェクトを上書きする最重要バグ） |
| 〃 | 1541-1553 | [logic] Compact() の最終 rename フォールバック（backup→orig 復旧失敗時に open 不能を防ぐ） |
| 〃 | 788-799 | [整理] CheckVersion の死蔵コード除去（**挙動完全不変**。詳細は §6.4） |
| `libraries/lib-project-file-io/ProjectSerializer.cpp` | 218-221, 254-257 | [null-deref] EndTag/WriteData の空 `mHandlers` ガード |
| 〃 | 565-567 | [allocation-bomb] ReadString に `len < 0` ガード（不正ファイルの `bad_alloc`→process termination 回避） |
| 〃 | 608-611 | [null-deref] FT_Pop の空スタック throw |
| 〃 | 319-320 | [UAF] `mCurrentTagName` を `string_view`→所有 `std::string`（FT_Push/FT_Pop で `mIds` が破壊されても dangling しない） |
| `libraries/lib-sqlite-helpers/sqlite/Connection.cpp` | 302 | [logic] `Connection::Close()` のロールバックが空ローカル vector を走査していたバグ（`= mPendingTransactions` を追加） |
| `libraries/lib-transactions/TransactionScope.cpp` | 48-54 | [null-deref] `Commit()` の `mpImpl == nullptr` 早期 return（factory 未設置時のクラッシュ回避） |
| `libraries/lib-project-history/UndoManager.cpp` | 139-143 | [OOB] `RemoveStates` の current/saved を `>=` 補正（プロジェクト復元時の OOB 回避） |

### 3.3 リアルタイム/並行性（最も慎重に扱った領域）
| ファイル | line | 修正内容 |
|---|---|---|
| `libraries/lib-audio-io/AudioIO.h` | 285 | [data-race] **`mSeek` を `std::atomic<double>` 化**（main thread と RT callback 間） |
| `libraries/lib-audio-io/PlaybackSchedule.cpp` | 329-336 | [logic] 新規 Node に `active.test_and_set()`（pool 早期再利用→consumer の dangling 防止） |
| 〃 | 416-454 | [data-race] Consumer の tail load を `relaxed`→`acquire`（ARM64 で producer の release store と正しくペア。x86 では no-op） |
| `libraries/lib-concurrency/concurrency/CancellationContext.cpp` | 57-67 | [data-race] `OnCancelled` ロック下で `mCancelled` 再チェック（TOCTOU 解消。`Cancel()` が drain 前に `exchange(true)` するため正当） |
| `libraries/lib-realtime-effects/RealtimeEffectList.h` | 82-108, 172-191 | [HIGH data-race] **`Visit()` の copy-on-write 化**。新規 `mSnapshot`(`shared_ptr<const States>`) を追加、Visit は `std::atomic_load` でローカルに取り出し走査（ロックを処理中保持しない） |
| `libraries/lib-realtime-effects/RealtimeEffectList.cpp` | 全 mutator | 各 mutator は `mStates` を `mLock` 下で更新後 `PublishSnapshot()`、通知はロック外。詳細は §4.1 |
| `libraries/lib-realtime-effects/RealtimeEffectState.{h,cpp}` | sig + 700-720 | [data-race] **`Finalize(bool rtStopped)` 化**。`mMainSettings = mWorkerSettings` のコピーは通常停止時のみ実行、hot-remove では skip（mMainSettings は直前 Flush 済で安全）。Wave 4 で revert した「コピー全削除」案より安全 |
| `libraries/lib-realtime-effects/RealtimeEffectManager.cpp` | 98, 273-276, 290-293 | Finalize 呼出 3箇所を `Finalize(true/false)` に。Manager::Finalize=true、ReplaceState/RemoveState=false |
| `libraries/lib-audio-devices/AudioIOBase.{h,cpp}` | h:241-250, cpp:462-470 | [logic] **新規 API `InvalidateDeviceCache()`** 追加（Pa_Terminate/Pa_Initialize 後にデバイスキャッシュをクリア） |
| `libraries/lib-audio-devices/DeviceManager.cpp` | 260-266 | [logic] Rescan() で `Pa_Initialize` 後に `AudioIOBase::InvalidateDeviceCache()` を呼ぶ |
| `libraries/lib-audio-devices/DeviceChange.cpp` | 54-57 | [resource-leak] デストラクタの `CoInitialize`→`CoUninitialize`（Windows COM apartment leak） |

### 3.4 サンプル/トラック/time-stretch
| ファイル | line | 修正内容 |
|---|---|---|
| `libraries/lib-wave-track/WaveTrack.cpp` | 620-625 | [null-deref] `LinkConsistencyFix` の null `next->GetName()` 除去（壊れたステレオプロジェクト読込時のクラッシュ解消） |
| `libraries/lib-time-and-pitch/StaffPadTimeAndPitch.cpp` | 124-127 | [logic] `GetSamples` の内側ループに `if (IllState()) break;`（**音声スレッドのスピン解消**） |
| 〃 | 37-41 | [読みやすさ] `GetFftSize` の括弧追加（**元コードも正しく等価**。finding は誤検知だが無害なので保持） |

---

## 4. 設計上の重要メモ（次の LLM がこの領域を触る時に必須）

### 4.1 `RealtimeEffectList` の copy-on-write モデル
**最重要の構造変更**。RT (audio) スレッドが触る `mSnapshot` と main thread の正本 `mStates` の二本立て。

- 不変条件: `mStates` 変更後、**`mLock` 解放前に必ず `PublishSnapshot()`** を呼ぶこと。
- RT 側: `Visit()` は `std::atomic_load(&mSnapshot)` でローカル `shared_ptr` を取り、走査中に snapshot が swap されてもバッファは生存。
- ロック保持中に Observer の `Publish(...)` を呼ばない（既存もそうなっており踏襲）。
- 新しい mutator を追加するなら必ず同じパターン（`{ LockGuard g{mLock}; … ; PublishSnapshot(); }` + 通知はロック外）。
- ⚠️ 注意: libstdc++/libc++ の shared_ptr の `atomic_load/store` はロックプール実装で**完全 wait-free ではない**（極小区間）。UB 解消の正味改善だが、厳密 RT 無ロックを要するなら将来 lock-free 化検討。
- ⚠️ C++17 で `std::atomic_load(shared_ptr*)` を使用（C++20 で deprecated）。プロジェクトは `set(CMAKE_CXX_STANDARD 17)` 固定（`CMakeLists.txt:140`）。C++20 移行時は `std::atomic<std::shared_ptr<…>>` に置き換える。

### 4.2 `RealtimeEffectState::Finalize(bool rtStopped)`
- `rtStopped == true`: 通常の playback-stop パス（`RealtimeEffectManager::Finalize`）。`WaitForAudioThreadStopped` + `Pa_CloseStream` を経て RT は join 同等で停止済 → `mMainSettings = mWorkerSettings` 安全。
- `rtStopped == false`: hot remove/replace パス（`ReplaceState`/`RemoveState` 中）。RT は旧 snapshot で **まだ処理中の可能性** → コピーを skip。`mMainSettings` は直前の `Access::Flush()` で同期済のため設定喪失なし。
- **新規 Finalize 呼出を追加する時は必ずどちらかを明示**。デフォルト引数を付けると hot path で誤って `true` が漏れる恐れがあるため**意図的に付けていない**。

### 4.3 `AudioIO::CallbackDoSeek` を触る前に読むこと
- `mSuspendAudioThread` は **StopStream と CallbackDoSeek の必須の相互排他**（StopStream のティアダウン vs seek 中のバッファ再配置の use-after-free を防止）。
- 既存の `wxMutexLocker` は worker が別ロックなので**有界スタール**（〜数百ms）であり**無限デッドロックではない**。
- 「StopStream を try-lock に置き換える」「critical section を縮める」のどちらも**安全に行うことはほぼ不可能**。詳細は `MCP_SESSION3_BUG_REVIEW.md` の Wave 5 §H1。
- 正攻法は「seek を worker thread に委譲して RT callback では set_request だけ」だが大規模・要実機テスト。本セッションでは見送り。
- 関連: `mSeek` は atomic 化済み（`AudioIO.h:285`）。

---

## 5. テスト計画（必須・順序付き）

ビルドは通っているが**実行時テストは未実施**。マージ前に少なくとも以下を実施すること。RT 系（5.1）は特に。

### 5.1 RT/concurrency 系（最優先）
| 修正 | テストシナリオ | 期待結果 |
|---|---|---|
| `RealtimeEffectList::Visit` COW | 再生中にエフェクトを追加/削除/置換/順序入れ替え（マスター/トラック両方、複数回） | クラッシュなし・音切れなし・追加直後から音に反映 |
| `RealtimeEffectState::Finalize(false)` | 再生中にエフェクトを RemoveState → 連続して別エフェクトを Add → 停止→`WriteXML`/プロジェクト保存→再オープン | 残ったエフェクトのパラメータが正しく保存/復元される（hot-remove したエフェクトは消える） |
| `RealtimeEffectState::Finalize(true)` | 通常の playback-stop → プロジェクト保存→再オープン | 各エフェクトの最終パラメータが保存/復元される |
| `AudioIO::mSeek` atomic | 高速で seek キーを連打しながら停止 | クラッシュなし、seek/stop が正常に終了 |
| `PlaybackSchedule` (acquire, active flag) | 長時間再生 + 大量のループ境界跨ぎ（特に ARM64 macOS） | 時刻ジャンプや遅延カウンタ異常なし |
| `CancellationContext` | クラウドアップロードを開始直後にプロジェクトを閉じる | アップロード処理が確実にキャンセルされる |

### 5.2 import/export 系
| 修正 | テストシナリオ | 期待結果 |
|---|---|---|
| `Export.cpp` temp ファイル機構 | 既存 WAV/MP3 を上書きエクスポート → 途中で「キャンセル」 / 途中でディスクフル | **既存ファイルが破壊されないこと**（temp に書かれて削除される） |
| MP3 末尾 ID3 | MP3 でメタデータ付きエクスポート | エラーなく完了し、ID3 タグが書かれている |
| Opus OP_HOLE | gap を含む `.opus` をインポート（手元になければ既存の Opus を `ffmpeg -map 0:a -c copy` で破損させて作成） | クラッシュなし、可能な範囲で読み込める |
| LOF 循環参照 | `a.lof` が `b.lof` を、`b.lof` が `a.lof` を参照する2ファイルを open | "Circular or too-deeply nested LOF" ダイアログ + ハング/スタックオーバーフローなし |
| WaveTrack null channel | 右ch ファイルを欠いた壊れた `.aup3` を open | クラッシュなしで警告ログ |
| Compact データ損失 | 大きいプロジェクトで「Compact and Save Project」をディスクほぼ満杯時に実行（要シナリオ作成） | 失敗時に元プロジェクトが残っていること |

### 5.3 通常パスの回帰確認
- 通常の WAV/MP3/FLAC/Opus エクスポート（メタデータあり/なし）
- 通常のプロジェクト保存/再オープン
- Undo/Redo の通常操作
- デバイス切替（オーディオ I/F ホットプラグ）

---

## 6. 真のバグだが意図的に未修正（再開時の判断材料）

⚠️ **次の LLM が善意で「直し忘れだ」と再導入しないために、各々に明示的な理由がある。**

### 6.1 H1: `AudioIO::CallbackDoSeek` のスタール
- 場所: `libraries/lib-audio-io/AudioIO.cpp:3232-3289` ＋ `StopStream:1472`
- なぜ未修正: ローカルな安全修正案（try-lock、CS 縮小）はいずれも実害ありまたは効果なし。正攻法は seek を worker へ委譲する設計変更だが要大規模改修＋実機テスト。
- 再開条件: 専用タスクとして起こし、(a) `mSeek` を request-queue モデルに変える詳細設計、(b) worker thread 側の状態遷移、(c) 既存 `mSuspendAudioThread` invariant の保証手段、を確定してから実装。詳細は `MCP_SESSION3_BUG_REVIEW.md` の Wave 5 §H1。

### 6.2 A1: `RealtimeEffectManager` `ProcessingScope::mSuspended` 未初期化
- 場所: `libraries/lib-realtime-effects/RealtimeEffectManager.h:259-268`
- なぜ未修正: 同一修正を upstream が `c2f4e8acd` (2024-06-13) で入れ `82312ebe1` で 2日後に reason なし revert。調査の結果、修正により Compressor/Limiter の `DynamicRangeProcessorHistoryPanel` が `RealtimeResume` で不要なセグメント区切りを描く回帰を起こすと判明。元の「バグ」は pause 中の suspend ヒント未伝播のみで**ほぼ無害**（再生中は false=現状と一致）。
- 再開条件: 履歴パネル側の resume サブスクリプションを整理してから再適用。要 UX 判断。詳細は `MCP_SESSION3_BUG_REVIEW.md` の Wave 5 §A1。

### 6.3 `RealtimeEffectList` の atomic shared_ptr が完全 wait-free でない
- 場所: `Visit()` 内 `std::atomic_load(&mSnapshot)`
- なぜ未対応: libstdc++/libc++ ではロックプール実装。UB 解消の正味改善だが厳密 RT 無ロックではない。
- 再開条件: 計測で問題が出れば、C++20 への移行と `std::atomic<std::shared_ptr<…>>` 採用、または専用 hazard pointer 実装を検討。

### 6.4 CheckVersion を真の opt-in（Yes/No, No で開かない）にする
- 場所: `libraries/lib-project-file-io/ProjectFileIO.cpp:788-799`
- なぜ未修正: 現在の修正は**死蔵コード整理のみで挙動完全不変**（既定ダイアログは OK のみ＝そもそも Yes/No ですらなかった）。真の opt-in にするかは**製品判断**（誤ると「古いプロジェクトを開けない」回帰になり得る非対称リスク）。
- 再開条件: もし opt-in 化するなら、`.ButtonStyle(Button::YesNo)` 追加 + メッセージを明示的な質問文に書き直し + `return updateVersion;` 化。あるいは「読み取り専用で開く」第三の選択肢を追加。詳細は `MCP_SESSION3_BUG_REVIEW.md` の Wave 5 §CheckVersion。

### 6.5 残った LOW 16 件（適用していない）
- 場所と内容は `MCP_SESSION3_BUG_REVIEW.md` の Wave 1 §LOW (16) 節を参照。
- 例: AudioIO の `alloca` ベース出力バッファ、UndoManager の `wxListCtrl(-1)` 等。
- いずれも到達性が極めて限定的または挙動への影響が無視できる。優先度低。

### 6.6 W2-M3 の `TransactionScope::Commit` 関連
- 修正済（`MCP_SESSION3_BUG_REVIEW.md` Wave 4 §② リスト参照）。ただし `Begin()`/`Rollback()` 側の同種チェックは未実施（call site の前提次第）。テストで factory 未設置時の他経路 NPE が出たら追加対応。

---

## 7. 誤検知として却下した指摘（再導入しないこと）

⚠️ **「これも直したほうがいいのでは？」と再 finder にかけると同じ誤検知が出る可能性が高い**。以下は実コードで反証済。

### 7.1 `ImportPCM.cpp:463` — 「WAV/AIFF の ID3 スキャンが無限ループ」
- 主張: `len = 0xFFFFFFFF` で `Seek(len + (len&1))` が 0 に巻き戻り無限ループ
- 反証: ループ内で毎周 `f.Read(id, 4)` + `f.Read(&len, 4)` が**8バイトを必ず消費**するため、`Seek(0)` でもファイル位置は前進し EOF で必ず停止。`while (!f.Error())` も EOF で抜ける。
- 結論: 無限ループは成立しない。修正不要。

### 7.2 `ExportCL.cpp:517` — 「ファイル名経由のシェルコマンドインジェクション」
- 主張: `cmd.Replace("%f", path)` で `wxExecute(cmd)` がシェル経由で実行される
- 反証: `wxExecute(wxString, wxEXEC_ASYNC, &process)` は wxWidgets が**自前で argv 分割**し execvp 系で実行する。`/bin/sh -c` には渡らないため `$()`/バッククォート等は解釈されない。
- 残課題（低リスク）: 引用符を含むファイル名で argv 分割の解釈ズレの可能性のみ。任意コード実行は不成立。

### 7.3 `ProjectFileIO::CheckVersion` の「No を尊重する修正」
- 主張: 「ユーザーの No が無視されている」
- 真相: 既定 `MessageBoxOptions` は **OK のみ**を描画。`MessageBoxResult::Yes` は決して返らず `updateVersion` は到達不能な死蔵コード。
- 対応: 死蔵コードのみ整理（挙動完全不変）。真の opt-in 化は製品判断（§6.4）。

### 7.4 `StaffPadTimeAndPitch::GetFftSize` の演算子優先順位
- 主張: `1 << (cond?A:B) + N` で `+` が `<<` より先に効くため `N` が片方にしか効かない
- 真相: **元コードも正しい**。`+` は `<<` より高優先なので `1 << ((cond?A:B) + N)` と等価に解釈される。今回の括弧追加は意味的に等価で**害は無いので保持**（可読性のみ向上）。

---

## 8. 別スレ（MCP 作業）との切り分け

- `mcp-companion/`、`modules/mod-mcp-server/`、`stem_mcp_server.py`、`mcp-companion/index.html`、ルート assets/launcher は**一切触っていない**。
- 別スレが進めた `mcp-companion/index.html` の変更（modified 表示）は **HEAD コミット `018b9f529` 以前**に発生したもの。本セッションの作業ではない。
- 本セッションでステージしてコミットするのは `git diff --stat -- '*.cpp' '*.h' | grep -vE "mcp-companion|stem_mcp"` で出る **32 ファイルだけ**。

---

## 9. 次の LLM への引き継ぎ事項（優先順）

| # | 項目 | 優先 | 内容 |
|---|---|---|---|
| 1 | RT 系の実機テスト | **高** | §5.1 を実施。問題があれば §6.1-6.3 に戻る |
| 2 | import/export 系の実機テスト | **高** | §5.2 を実施。Compact データ損失修正は要シナリオ作成 |
| 3 | H1 worker 委譲設計 | 中 | §6.1 の前提条件を満たす設計タスクを起こす |
| 4 | CheckVersion opt-in 化 | 中 | §6.4 の製品判断後 |
| 5 | A1 履歴パネル整理 | 中 | §6.2 の UX 判断後 |
| 6 | LOW 16 件の精査 | 低 | §6.5。優先度低い |
| 7 | レビュー範囲を `src/` 本体に拡大 | 任意 | effects/menus/tracks/widgets 等を Wave 1 と同じ手法で |

### 個別修正を rollback する手順
何か一つの修正が回帰を起こした場合、以下で簡単に戻せる:
```bash
# このセッションのコミットを表示
git log --oneline -3
# 1ファイル単位で戻す
git checkout HEAD~1 -- libraries/lib-realtime-effects/RealtimeEffectList.cpp
# ハンクごとに戻すなら
git checkout -p HEAD~1 -- libraries/lib-realtime-effects/RealtimeEffectList.cpp
```

---

## 10. 重要参照

- 全 wave の詳細・全 finding の根拠 → `MCP_SESSION3_BUG_REVIEW.md`（945 行）
- 前のハンドオフ（MCP 作業） → `MCP_SESSION2_HANDOFF.md`
- Phase 0 ハンドオフ → `MCP_PHASE0_IMPLEMENTATION_HANDOFF.md`
- 設計意思決定 → `MCP_LLM_CONTROL_HANDOFF.md`

---

## 11. 統計

- **Wave 数**: 5（finder → adversarial verifier → FN 回収 → 慎重修正 → 残課題調査）
- **総エージェント数**: 約 152 本（finder 20 + verifier 121 + FN 回収 5 + 修正 ~6 直接 + 18 medium 並列 + 5 concurrency + 4 調査）
- **総 subagent tokens**: 約 5.2M（sonnet 中心。opus は管理＋実コード spot-check のみ）
- **変更**: 32 source files, +379/-86 lines
- **ビルド**: ninja exit 0、エラー 0、警告は既存コードのみ
- **適用**: ~30 件
- **却下（FP）**: 4 件
- **未修正（理由明記）**: 6 件

> 本セッションは「直すべきもの」と「直すと壊すもの」「直しても無意味なもの」「直せるが要設計のもの」を明確に分けたことが価値。次の LLM は §6 と §7 を最優先で読むこと。
