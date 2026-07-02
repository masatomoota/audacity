<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/otis/otis-lockup-dark.svg">
    <img src="assets/otis/otis-lockup-light.svg" alt="Otis" width="360">
  </picture>
</p>

<p align="center">
  <b>話しかけて、音を編集する。</b><br>
  LLM がエンドツーエンドで操作する、フル機能のマルチトラック・オーディオエディタ。
</p>

<p align="center">
  <img alt="License" src="https://img.shields.io/badge/license-GPLv2--or--later-D8382A">
  <img alt="Platform" src="https://img.shields.io/badge/macOS_(Apple_Silicon)-1A1714">
  <img alt="Built on" src="https://img.shields.io/badge/built%20on-Audacity%203.7-E8A33D">
  <img alt="MCP" src="https://img.shields.io/badge/MCP-JSON--RPC%202.0-4493f8">
</p>

---

## Otis とは？

**Otis** は、[Audacity](https://www.audacityteam.org) 3.7 をベースにした**オープンソースのオーディオエディタ**です。
最大の特徴は、**LLM（大規模言語モデル）が自然言語でエディタを直接操作できる**こと。

> **「冒頭の無音をカットして、最後の 2 秒をフェードアウトして、MP3 で書き出して」**
>
> — こう話しかけるだけで、Otis が実際のエディタ上で編集を行います。ブラックボックスではなく、目の前の DAW が動きます。

内蔵の **MCP（Model Context Protocol）サーバ**がエディタのコマンド体系を AI に公開し、付属のチャットコンパニオンアプリから自然言語で操作できます。

---

## ✨ 主な機能

### 🎤 話しかけて編集
- 内蔵 MCP サーバ（`mod-mcp-server`）が、Audacity の全コマンドを JSON-RPC で外部に公開
- 付属のチャット UI からの指示で、トラック作成・選択・エフェクト適用・書き出しまで一気通貫

### 🧠 音を「聴く」AI — 知覚コマンド
LLM がオーディオを**数値で判断**できるよう、独自の分析コマンドを搭載：

| コマンド | 機能 |
|---|---|
| `GetAudioStats` | ピーク（true-peak 対応）、RMS、クリップ数、DC オフセット |
| `GetLoudness` | EBU R128 統合ラウドネス (LUFS) ＋ short-term / momentary 最大値 |
| `GetSpectrum` | FFT スペクトル分析（帯域 dB、支配周波数、スペクトル重心） |
| `DetectSilence` | 無音区間の検出（先頭・末尾の無音も） |
| `DetectOnsets` | アタック（音の立ち上がり）時刻の検出 |

→ 「クリップしてる？」「ラウドネスは基準内？」「どこに無音がある？」をAIが自分で測って判断・修正できます。

### 🎵 ステム分離（UVR レベル）
- チャットで「ボーカルとインストに分けて」と頼むだけ
- **UVR-MDX-NET** / **Demucs** 対応（audio-separator エンジン）
- Apple Silicon GPU（MPS）対応で高速

### 🎙️ 音声文字起こし（Whisper）
- チャットで「この音声を文字起こしして」と頼むだけ
- **mlx-whisper**（Apple Silicon ネイティブ）でローカル処理
- OpenAI API 経由も選択可能

### 🎨 Audacity の全機能
- マルチトラック編集、32-bit float 処理
- 35+ のビルトインエフェクト ＋ Audio Unit プラグイン対応
- あらゆるオーディオデバイスからの録音
- **WAV / AIFF / MP3 / FLAC / OGG Vorbis / Opus** を標準対応（ffmpeg 不要）
- M4A / AAC / WMA / AC3 等は `brew install ffmpeg` で追加対応（DMG には ffmpeg を同梱しません — ライセンスと配布サイズの観点から）
- Nyquist / スクリプティングによる自動化

### 🖥️ UI の改善（Audacity からの差分）
- **Retina（HiDPI）対応**：トラック名・クリップタイトルがくっきり鮮明に
- **光学的な垂直センタリング**：フォントメトリクスに基づく精密なテキスト配置
- **言語非依存のタイムコード**：`00:00:00.000` のコロン表記（日本語ロケールでも崩れない）
- **Otis ブランディング**：独自アイコン・ロゴ・緋色（scarlet）のアクセントカラー

---

## 📐 アーキテクチャ

```
┌─────────────────────────────────────────────────────────────┐
│  ブラウザ（チャット UI）                                       │
│  http://127.0.0.1:8765                                      │
└────────────┬────────────────────────────────────────────────┘
             │ HTTP / SSE
             ▼
┌─────────────────────────────────────────────────────────────┐
│  server.py（SSE ブリッジ）                                    │
│  ├── codex_bridge.py ─── stdio JSON-RPC ──▶ codex app-server │
│  ├── stem_mcp_server.py（ステム分離 MCP）                      │
│  └── transcribe_mcp_server.py（文字起こし MCP）                │
└────────────┬────────────────────────────────────────────────┘
             │ MCP (JSON-RPC 2.0 over HTTP)
             ▼
┌─────────────────────────────────────────────────────────────┐
│  Otis（Audacity 3.7 ベース）                                  │
│  └── mod-mcp-server（内蔵モジュール）                          │
│      POST http://127.0.0.1:4830/mcp                         │
│      ├── tools/list … コマンドカタログ                         │
│      ├── tools/call … コマンド実行（メインスレッド同期）          │
│      └── ScriptCommandRelay（既存・無改変で再利用）             │
└─────────────────────────────────────────────────────────────┘
```

**設計のポイント：**
- コマンド実行は `AppCommandEvent` + `wxSemaphore` で**必ずメインスレッドに整流**（スレッド安全）
- MCP サーバは `127.0.0.1` 固定バインド（外部到達不可）
- Codex の `CODEX_HOME` を隔離し、ユーザのグローバル設定を汚さない

---

## 🚀 クイックスタート

### ビルド済み DMG を使う場合

1. [Releases](https://github.com/masatomoota/audacity/releases) から `Otis-MCP.dmg` をダウンロード（リリース準備中）
2. DMG をマウントし、`Otis.app` を `/Applications` にドラッグ
3. 初回のみ Gatekeeper を解除（DMG は未署名・公証なしのため）：
   ```bash
   xattr -dr com.apple.quarantine /Applications/Otis.app
   ```
4. DMG 内の「**Otis と AI チャットを起動.command**」をダブルクリック
   - Otis（エディタ本体）と AI チャットコンパニオンが一緒に起動し、Chrome の `--app` モード（タブやアドレスバーのない専用ウィンドウ）でチャット UI が開きます
5. 初回のみ ChatGPT にログイン（UI の「ChatGPT でログイン」ボタン、または事前に `codex login`）

### ソースからビルドする場合

#### 前提条件
- macOS（Apple Silicon）
- CMake ≥ 3.16、Ninja、Python 3、Xcode Command Line Tools
- Conan 2（なければ自動インストールされる）

#### ビルド

```bash
git clone https://github.com/masatomoota/audacity.git otis
cd otis
git checkout mcp-llm

# Configure（AU-only / GPLv2 ビルド）
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -Daudacity_has_vst3=Off \
  -Daudacity_use_vst=Off \
  -Daudacity_bundle_gplv3=Off

# Build
cmake --build build -j

# 起動
open build/RelWithDebInfo/Audacity.app
```

> **⚠️ CMake 4.x を使う場合**、`-DCMAKE_POLICY_VERSION_MINIMUM=3.5` が必須です（vendored な CMake ファイルの互換性のため）。

#### MCP サーバの疎通確認

```bash
# Otis 起動後に実行
curl -s -X POST http://127.0.0.1:4830/mcp \
  -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'
# → {"jsonrpc":"2.0","id":1,"result":{}}
```

#### MCP ツール一覧

`tools/list` は以下の 10 個のツールを返します。頻出操作には専用ツール（2〜9）を使い、それ以外の操作にのみ `run_command` を使うことを推奨します。

| ツール | 用途 |
|---|---|
| `run_command` | 任意の Audacity スクリプトコマンドを実行（専用ツールにない操作用） |
| `get_info` | `GetInfo` コマンドでトラック・クリップ・ラベル等のメタデータを取得 |
| `import_audio` | 音声ファイルを新規トラックとしてインポート |
| `export_audio` | 音声を書き出し（`start`/`end` を両方指定するとその範囲のみ） |
| `select_audio` | 時間・トラックの選択（`mode`: `range`/`all`/`none`） |
| `list_tracks` | トラック一覧を取得（配列順が `track` インデックスに対応） |
| `generate_tone` | トーン生成（トラック・選択の準備込みで安全。`track` 省略時は新規トラック作成） |
| `apply_effect` | 名前付きエフェクトをパラメータ付きで適用（事前に `select_audio` で対象を選択） |
| `set_track` | トラック名・音量（dB）・パン・ミュート・ソロを変更 |
| `remove_track` | トラックを削除 |

#### チャットコンパニオンの起動

```bash
cd mcp-companion
./run.sh
# → ブラウザで http://127.0.0.1:8765 が開きます
```

#### DMG の作成（配布用）

```bash
./scripts/make-dmg.sh
# → build/Otis-MCP.dmg
```

---

## 🎛️ チャットでできることの例

| やりたいこと | チャットへの指示例 |
|---|---|
| トーン生成 | 「440Hz のサイン波を 3 秒作って」 |
| 録音 | 「マイクから 10 秒録音して」 |
| ノイズ除去 | 「このトラックのノイズを除去して」 |
| 音量調整 | 「音量を -3dB にして」 |
| フェード | 「最後の 2 秒をフェードアウトして」 |
| ステム分離 | 「ボーカルとインストに分離して」 |
| 文字起こし | 「この音声を文字起こしして」 |
| 音質チェック | 「クリッピングしていないか確認して」 |
| ラウドネス計測 | 「ラウドネスは何 LUFS？」 |
| 書き出し | 「MP3 で書き出して」 |
| 複合操作 | 「冒頭の無音をカット → ノーマライズ → WAV で書き出して」 |

---

## 📁 リポジトリ構成

```
otis/
├── src/                              # Audacity コアソース
│   ├── commands/
│   │   ├── GetAudioStatsCommand.*    # [追加] 音声統計（知覚コマンド）
│   │   ├── GetSpectrumCommand.*      # [追加] スペクトル分析
│   │   ├── DetectSilenceCommand.*    # [追加] 無音検出
│   │   ├── DetectOnsetsCommand.*     # [追加] アタック検出
│   │   └── GetLoudnessCommand.*      # [追加] ラウドネス計測
│   └── widgets/BackedPanel.cpp       # [変更] HiDPI バッキング修正
│
├── modules/scripting/
│   ├── mod-mcp-server/               # [追加] MCP サーバモジュール
│   │   ├── MCPHttpServer.cpp         #   JSON-RPC 2.0 / HTTP サーバ
│   │   ├── MCPServerCallback.cpp     #   モジュール ABI・リレー接続
│   │   └── lib/                      #   cpp-httplib + nlohmann/json（vendored）
│   └── mod-script-pipe/              # 既存のパイプスクリプティング
│
├── mcp-companion/                    # [追加] AI チャットコンパニオン
│   ├── server.py                     #   SSE ブリッジサーバ
│   ├── codex_bridge.py               #   Codex app-server クライアント
│   ├── index.html                    #   チャット UI（日本語）
│   ├── stem_mcp_server.py            #   ステム分離 MCP サーバ
│   ├── transcribe_mcp_server.py      #   文字起こし MCP サーバ
│   ├── run.sh                        #   コンパニオン起動スクリプト
│   └── セットアップして起動.command     #   ワンクリックランチャー
│
├── scripts/make-dmg.sh               # [追加] DMG パッケージング
├── assets/otis/                      # [追加] ブランドアセット（アイコン・ロゴ）
└── libraries/                        # Audacity 共有ライブラリ群
```

---

## ⚙️ 音声 AI ツールのセットアップ（任意）

ステム分離と文字起こしは追加セットアップが必要です：

```bash
# DMG 同梱の「ステム分離セットアップ.command」をダブルクリック
# または手動で：
python3 -m venv ~/.audacity-mcp-companion/sep-venv
~/.audacity-mcp-companion/sep-venv/bin/pip install "audio-separator[cpu]" mlx-whisper
```

- ステム分離のモデルは初回使用時に自動ダウンロード（約 40MB〜）
- 文字起こしのモデルも初回に自動ダウンロード（whisper-large-v3-turbo、約 1.5GB）

---

## 🔧 環境変数

| 変数 | 既定値 | 説明 |
|---|---|---|
| `MCP_URL` | `http://127.0.0.1:4830/mcp` | Otis の MCP エンドポイント |
| `COMPANION_PORT` | `8765` | チャット UI のポート |
| `CODEX_BIN` | `codex` | Codex CLI のパス |
| `CODEX_MODEL` | （アカウント既定） | モデル指定（例: `gpt-4o`） |
| `CODEX_HOME_COMPANION` | `~/.audacity-mcp-companion` | Codex の隔離ホーム |
| `CODEX_SANDBOX` | `danger-full-access` | サンドボックスモード（変更非推奨） |

> **サンドボックスの注意：** `danger-full-access` のままにしてください。制限モード（`read-only` 等）では MCP の localhost 通信がブロックされます。

---

## 📄 開発ドキュメント

このリポジトリには、実装の経緯と設計判断を詳細に記録したハンドオフ文書群があります：

| ドキュメント | 内容 |
|---|---|
| [`MCP_LLM_CONTROL_HANDOFF.md`](MCP_LLM_CONTROL_HANDOFF.md) | 設計思想・Audacity 3.x 選定の根拠・ロードマップ |
| [`MCP_PHASE0_IMPLEMENTATION_HANDOFF.md`](MCP_PHASE0_IMPLEMENTATION_HANDOFF.md) | Phase 0 実装完了（MCP サーバ・知覚コマンド・コンパニオン） |
| [`MCP_SESSION2_HANDOFF.md`](MCP_SESSION2_HANDOFF.md) | UI 修正・Codex 再構築・ステム分離・DMG 配布 |
| [`MCP_SESSION3_HANDOFF.md`](MCP_SESSION3_HANDOFF.md) | C++ コアのバグ監査（32 ファイル・30+ 件の修正） |
| [`MCP_SESSION4_HANDOFF.md`](MCP_SESSION4_HANDOFF.md) | Otis リブランド・チャットからの実機検証・IME/Chrome --app/ステム検証 |
| [`BUILD_HANDOFF.md`](BUILD_HANDOFF.md) | ビルド・パッケージの完全再現手順 |
| [`OTIS_REBRAND_HANDOFF.md`](OTIS_REBRAND_HANDOFF.md) | Otis へのリブランド実装ガイド |
| [`OTIS_UI_COLOR_HANDOFF.md`](OTIS_UI_COLOR_HANDOFF.md) | UI アクセントカラーの設計 |
| [`BUILDING.md`](BUILDING.md) | Audacity 公式のビルド手順（Windows / macOS / Linux） |

---

## 📜 ライセンス

Otis は **GPLv2-or-later** のオープンソースソフトウェアです（Audacity から継承）。

- VST3 SDK を除外した AU-only ビルドにより、GPLv3 コンポーネントを含みません
- サードパーティライブラリ（`lib-src/`）は個別のライセンスに従います
- 同梱の `cpp-httplib` と `nlohmann/json` は MIT ライセンス（GPL 互換）

詳細は [`LICENSE.txt`](LICENSE.txt) を参照してください。

---

## 🙏 謝辞

Otis は [Audacity](https://www.audacityteam.org) プロジェクトとそのコントリビュータの成果の上に成り立っています。

- **Audacity** は Muse Group の登録商標です。「Otis」は独立したフォークであり、Audacity プロジェクトと提携・公認の関係にはありません。
- ステム分離は [audio-separator](https://github.com/nomadkaraoke/python-audio-separator) / [UVR](https://github.com/Anjok07/ultimatevocalremovergui) / [Demucs](https://github.com/facebookresearch/demucs) の成果を活用しています
- 文字起こしは [mlx-whisper](https://github.com/ml-explore/mlx-examples) / [OpenAI Whisper](https://github.com/openai/whisper) を利用しています

---

<sub>ブランドアセットは <a href="assets/otis/">assets/otis/</a> にあり、<code>generate_assets.py</code> から再生成できます。</sub>
