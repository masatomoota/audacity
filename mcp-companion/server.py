#!/usr/bin/env python3
"""
Audacity MCP Companion — Codex app-server edition.

A localhost web chat that drives Audacity by natural language.  Unlike the
legacy companion (legacy/server.py), the LLM brain is **not** a direct
Anthropic/OpenAI API call with an API key.  Instead it is the **Codex app
server** (`codex app-server`), which gives us, for free:

  * ChatGPT *web login* — no API key; auth handled by Codex.
  * Persistent *memory* — every conversation is a Codex thread, saved to disk
    and resumable across restarts.

Codex's agent reaches Audacity through the running Audacity MCP server
(http://127.0.0.1:4830/mcp), wired up in an isolated CODEX_HOME so only the
Audacity tools are in scope.

Stdlib only — no pip.  See README.md.
"""

import http.server
import json
import os
import sys
import threading
import urllib.request
import urllib.error
import webbrowser
import traceback
from pathlib import Path

from codex_bridge import CodexAppServer, CodexError, _PROCESS_DOWN

# Bumped whenever DEV_INSTRUCTIONS changes meaningfully. Recorded per-thread in
# thread_meta.json at thread/start time so the UI can flag threads that were
# started under an older prompt (see /api/threads, /api/thread).
INSTRUCTIONS_VERSION = 2


# --------------------------------------------------------------------------- #
# Config
# --------------------------------------------------------------------------- #

def _load_dotenv():
    candidates = []
    if os.environ.get("ENV_FILE"):
        candidates.append(Path(os.environ["ENV_FILE"]))
    candidates.append(Path(__file__).parent.resolve() / ".env")
    candidates.append(Path.cwd() / ".env")
    seen = set()
    for path in candidates:
        try:
            rp = path.resolve()
        except Exception:
            continue
        if rp in seen or not rp.is_file():
            continue
        seen.add(rp)
        try:
            for line in rp.read_text().splitlines():
                s = line.strip()
                if not s or s.startswith("#"):
                    continue
                if s.startswith("export "):
                    s = s[len("export "):].strip()
                if "=" not in s:
                    continue
                k, v = s.split("=", 1)
                k = k.strip()
                v = v.strip().strip('"').strip("'")
                if k and k not in os.environ:
                    os.environ[k] = v
        except Exception:
            pass


_load_dotenv()

BASE_DIR = Path(__file__).parent.resolve()

COMPANION_HOST = os.environ.get("COMPANION_HOST", "127.0.0.1")
COMPANION_PORT = int(os.environ.get("COMPANION_PORT", "8765"))

# Origins allowed to talk to this localhost server (CORS + Origin-header gate
# on state-changing POSTs). Built from COMPANION_HOST/PORT rather than "*" so a
# malicious page in another tab cannot drive the companion / Audacity.
ALLOWED_ORIGINS = {
    f"http://{COMPANION_HOST}:{COMPANION_PORT}",
    f"http://localhost:{COMPANION_PORT}",
}

# Audacity's MCP server (the in-app mod-mcp-server).
MCP_URL = os.environ.get("MCP_URL", "http://127.0.0.1:4830/mcp")

# Isolated Codex home so the companion only sees the Audacity MCP server and
# keeps its own threads/auth separate from the user's global Codex.  Defaults to
# a writable per-user location (NOT next to the scripts) so the companion runs
# correctly even when launched from a read-only location such as a mounted DMG.
CODEX_HOME = Path(os.environ.get(
    "CODEX_HOME_COMPANION", str(Path.home() / ".audacity-mcp-companion"))).resolve()
CODEX_WORKDIR = Path(os.environ.get(
    "CODEX_WORKDIR", str(CODEX_HOME / "workdir"))).resolve()
CODEX_BIN = os.environ.get("CODEX_BIN", "codex")
# Optional model override (otherwise the account default, e.g. gpt-5.5).
CODEX_MODEL = os.environ.get("CODEX_MODEL", "").strip()
GLOBAL_CODEX_HOME = Path(os.environ.get(
    "GLOBAL_CODEX_HOME", str(Path.home() / ".codex")))

# Runtime state (NOT committed — see .gitignore): maps threadId -> metadata,
# currently just {"instructionsVersion": N} recorded when the thread was
# started. Lets the UI flag threads whose developer instructions are stale.
THREAD_META_PATH = BASE_DIR / "thread_meta.json"
_thread_meta_lock = threading.Lock()

DEV_INSTRUCTIONS = """あなたは「Otis」というチャット内のアシスタントで、音声編集ソフト Otis を操作します。返答は必ず日本語で、簡潔に行ってください。

使えるツール:
- run_command: 任意の Otis スクリプトコマンドを実行します。例:
  "NewMonoTrack" / "Select: Start=0 End=3" /
  "Tone: Frequency=440 Amplitude=0.5 Waveform=Sine Start=0 End=3" /
  "Amplify: Ratio=0.5" / "Normalize:" / "Import2: Filename=/path/in.wav" /
  "Export2: Filename=/tmp/out.wav"
  スペースを含むパス・値は必ず二重引用符で囲むこと（例:
  Import2: Filename="/path with spaces/in.wav"）。引用符なしでスペースを
  含めると値が途中で切り詰められ、意図しないファイルが開かれる/書き出される。
- get_info: Otis の状態を取得します。type=Tracks / Selection / Clips / Labels、
  または type=Commands で全コマンドのカタログ（パラメータ付き）。
  run_command 経由の解析コマンドもあります: "GetAudioStats:" "GetSpectrum:"
  "GetLoudness:" "DetectSilence:" "DetectOnsets:"（peak/RMS/true-peak dBFS、
  クリップ、LUFS、スペクトル、無音、オンセット等を JSON で返す）。
- separate_stems: 音声ファイルをステム（ボーカル/インスト等）に分離します
  （UVR/audio-separator）。ステム分離を頼まれたら次の手順で行う:
    1) run_command で "Export2: Filename=/tmp/aud_stem_src.wav NumChannels=2"
       を実行し、対象を WAV に書き出す。選択範囲があればその範囲が、
       選択範囲が無ければプロジェクト全体が自動的に書き出される。
       プロジェクト全体を明示的に書き出したい場合は、先に run_command で
       "SelectAll:" を実行してから Export2 してもよい。
    2) separate_stems(input_path="/tmp/aud_stem_src.wav") を呼ぶ。既定モデルは
       Vocals/Instrumental。4ステム(vocals/drums/bass/other)は
       model="htdemucs.yaml"。利用可能なモデルは list_stem_models で確認。
    3) 返ってきた各ステムのファイルパスを run_command の
       Import2: Filename=... で Otis に読み込む。
    4) どのモデルで分離し、どのトラックを追加したかを日本語で報告する。
    （分離には数分かかることがあります。）
- transcribe_audio: 音声をローカル Whisper で文字起こしします（APIキー不要・
  端末内で処理）。文字起こしを頼まれたら:
    1) run_command で Export2: Filename=/tmp/aud_transcribe_src.wav を実行し、
       対象を WAV に書き出す。選択範囲があればその範囲が、選択範囲が無ければ
       プロジェクト全体が自動的に書き出される。プロジェクト全体を明示的に
       書き出したい場合は、先に run_command で "SelectAll:" を実行してから
       Export2 してもよい。
    2) transcribe_audio(input_path="/tmp/aud_transcribe_src.wav") を呼ぶ。
       言語が分かっていれば language="ja" 等を指定（省略で自動判定）。
    3) 返ってきた全文を日本語で提示する。タイムスタンプ付きセグメントも返るので、
       ユーザーが望めば run_command でラベルトラック化もできる。

作法:
- これは Otis 操作・解析・文字起こし・ステム分離の専用アシスタントです。
- 仕事は基本的に Otis ツール（run_command/get_info/separate_stems/
  transcribe_audio）で完結させる。シェルコマンド・OS 操作・ファイルの削除/移動/
  改変などの Otis 外の操作は原則使わない。どうしても必要なときだけ実行を試みて
  よいが、その種の操作は**実行前にユーザーへ承認ダイアログが表示される**（ユーザーが
  許可しなければ実行されない）。安全のため、不要な OS 操作は避ける。
- 不可能・非常識・矛盾した依頼は、実行を試みず簡潔に説明して確認を求める
  （大量の繰り返し操作で延々ループしない）。
- ジェネレータやエフェクトは通常まず選択が必要 — "Select:" を使う。
- コマンド構文が不確かなら get_info type=Commands を参照する。
- 「クリップしてる?」「音量は?」等は解析コマンドで測ってから数値で判断する。
- 破壊的操作やファイル書き出しの前は、明確な指示がない限り確認する。
- 返答は日本語で簡潔に。実行した操作と結果を述べる。"""


# --------------------------------------------------------------------------- #
# Thread metadata (instructionsVersion tracking)
# --------------------------------------------------------------------------- #

def _read_thread_meta():
    try:
        with _thread_meta_lock:
            if not THREAD_META_PATH.is_file():
                return {}
            return json.loads(THREAD_META_PATH.read_text(encoding="utf-8"))
    except Exception:
        return {}


def _record_thread_meta(thread_id, instructions_version=INSTRUCTIONS_VERSION):
    """Record {"<thread_id>": {"instructionsVersion": N}} in thread_meta.json.
    Atomic write (tmp file + os.replace) so a crash mid-write can't corrupt the
    file for concurrent readers."""
    with _thread_meta_lock:
        try:
            data = json.loads(THREAD_META_PATH.read_text(encoding="utf-8")) \
                if THREAD_META_PATH.is_file() else {}
        except Exception:
            data = {}
        data[thread_id] = {"instructionsVersion": instructions_version}
        tmp_path = THREAD_META_PATH.with_suffix(".json.tmp")
        try:
            tmp_path.write_text(json.dumps(data), encoding="utf-8")
            os.replace(tmp_path, THREAD_META_PATH)
        except Exception as e:
            print("[thread_meta] failed to write:", e)


def _thread_instructions_version(thread_id):
    meta = _read_thread_meta().get(thread_id)
    if not meta:
        return None
    return meta.get("instructionsVersion")


# --------------------------------------------------------------------------- #
# Direct Audacity MCP health check (independent of Codex)
# --------------------------------------------------------------------------- #

_mcp_id = 0
_mcp_id_lock = threading.Lock()


def _audacity_mcp_status():
    """Return (ok, tool_names, error) by pinging Audacity's MCP directly."""
    global _mcp_id
    with _mcp_id_lock:
        _mcp_id += 1
        rid = _mcp_id
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": "tools/list"}).encode()
    req = urllib.request.Request(
        MCP_URL, data=body,
        headers={"Content-Type": "application/json"}, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=4) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        if "error" in data:
            return False, [], str(data["error"])
        tools = data.get("result", {}).get("tools", [])
        return True, [t.get("name", "") for t in tools], None
    except Exception as e:
        return False, [], str(e)


# --------------------------------------------------------------------------- #
# Codex home setup
# --------------------------------------------------------------------------- #

def setup_codex_home():
    CODEX_HOME.mkdir(parents=True, exist_ok=True)
    CODEX_WORKDIR.mkdir(parents=True, exist_ok=True)

    # Inherit an existing ChatGPT login from the global Codex home so the user
    # does not have to re-authenticate (they still can re-login from the UI).
    local_auth = CODEX_HOME / "auth.json"
    global_auth = GLOBAL_CODEX_HOME / "auth.json"
    if not local_auth.exists() and global_auth.exists():
        try:
            local_auth.write_bytes(global_auth.read_bytes())
            try:
                os.chmod(local_auth, 0o600)
            except Exception:
                pass
            print(f"  Inherited ChatGPT login from {global_auth}")
        except Exception as e:
            print(f"  (could not inherit global auth: {e})")

    # Write an isolated config: ONLY the Audacity MCP server is in scope.
    #
    # sandbox_mode = "danger-full-access" is required: Codex's restricted
    # sandboxes ("read-only"/"workspace-write") block the agent's MCP-over-HTTP
    # call to Audacity's localhost server (the call comes back empty and the
    # turn reports the tool as "rejected"). Full access is acceptable here — it
    # is a localhost, user-driven audio tool, the Codex home is isolated to this
    # companion, and the developer instructions forbid shell/file use.
    sandbox = os.environ.get("CODEX_SANDBOX", "danger-full-access")
    lines = [
        "# Generated by Audacity MCP Companion. Edit MCP_URL via .env instead.",
        f'approval_policy = "{os.environ.get("CODEX_APPROVAL", "untrusted")}"',
        f'sandbox_mode = "{sandbox}"',
    ]
    if CODEX_MODEL:
        lines.append(f'model = "{CODEX_MODEL}"')
    lines += [
        "",
        "[mcp_servers.audacity]",
        f'url = "{MCP_URL}"',
        "",
    ]
    # Stem separation MCP server (UVR/audio-separator). Registered as a stdio MCP
    # server so the agent gets a `separate_stems` tool. Runs in the companion's
    # Python (json + subprocess only); it shells out to the sep-venv's
    # audio-separator. If the venv isn't set up yet, the tool returns install
    # instructions rather than failing the whole thread.
    stem_server = BASE_DIR / "stem_mcp_server.py"
    if stem_server.exists():
        lines += [
            "[mcp_servers.stem_separator]",
            f'command = "{sys.executable}"',
            f'args = ["{stem_server}"]',
            "",
        ]
    # Transcription MCP server (local Whisper / mlx-whisper). Same pattern: runs
    # in the companion's Python and shells out to the audio venv's mlx_whisper.
    transcribe_server = BASE_DIR / "transcribe_mcp_server.py"
    if transcribe_server.exists():
        lines += [
            "[mcp_servers.transcribe]",
            f'command = "{sys.executable}"',
            f'args = ["{transcribe_server}"]',
            "",
        ]
    (CODEX_HOME / "config.toml").write_text("\n".join(lines))


# --------------------------------------------------------------------------- #
# Global Codex app-server bridge
# --------------------------------------------------------------------------- #

bridge = None  # type: CodexAppServer | None
_bridge_lock = threading.Lock()

# Thread ids with a turn currently in flight (prevents overlapping /api/chat
# POSTs to the same thread, which otherwise corrupt Codex's turn state).
_inflight_threads = set()
_inflight_lock = threading.Lock()


def _try_mark_inflight(thread_id):
    """Atomically mark thread_id as in-flight. Returns False if it already was
    (caller should reject with 409)."""
    with _inflight_lock:
        if thread_id in _inflight_threads:
            return False
        _inflight_threads.add(thread_id)
        return True


def _clear_inflight(thread_id):
    with _inflight_lock:
        _inflight_threads.discard(thread_id)


def get_bridge():
    global bridge
    with _bridge_lock:
        if bridge is not None and bridge.is_alive():
            return bridge
        # (Re)start.
        if bridge is not None:
            try:
                bridge.stop()
            except Exception:
                pass
        b = CodexAppServer(
            codex_home=str(CODEX_HOME),
            cwd=str(CODEX_WORKDIR),
            codex_bin=CODEX_BIN,
            log=lambda *a: print("[bridge]", *a),
        )
        b.start()
        bridge = b
        return bridge


def start_thread(b):
    params = {
        "source": "startup",
        "cwd": str(CODEX_WORKDIR),
        "developerInstructions": DEV_INSTRUCTIONS,
    }
    resp = b.request("thread/start", params, timeout=60)
    thread_id = resp["thread"]["id"]
    _record_thread_meta(thread_id)
    return thread_id, resp


# --------------------------------------------------------------------------- #
# HTTP handler
# --------------------------------------------------------------------------- #

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print(f"[{self.address_string()}] {fmt % args}")

    # ---- small helpers ----
    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        if length == 0:
            return {}
        raw = self.rfile.read(length)
        try:
            return json.loads(raw.decode("utf-8"))
        except Exception:
            return {}

    def _cors_origin(self):
        """Echo back the request Origin if it's one of ours, else fall back to
        the primary allowed origin. Never "*" — this is a localhost tool that
        can drive Audacity, so we don't want any page in any tab to reach it."""
        origin = self.headers.get("Origin")
        if origin in ALLOWED_ORIGINS:
            return origin
        return f"http://{COMPANION_HOST}:{COMPANION_PORT}"

    def _origin_allowed(self):
        """Origin gate for state-changing POSTs. No Origin header (curl, other
        local tools) is allowed through; a present-but-foreign Origin is not."""
        origin = self.headers.get("Origin")
        if origin is None:
            return True
        return origin in ALLOWED_ORIGINS

    def send_json(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", self._cors_origin())
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, content):
        body = content if isinstance(content, bytes) else content.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _sse_open(self):
        # Close the connection when the stream ends so the client (fetch reader
        # / EventSource) sees EOF. Without this, HTTP/1.1 keep-alive leaves the
        # request hanging after the final `done` event.
        self.close_connection = True
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.send_header("X-Accel-Buffering", "no")
        self.send_header("Access-Control-Allow-Origin", self._cors_origin())
        self.end_headers()

    def _sse(self, event, data):
        chunk = f"event: {event}\ndata: {json.dumps(data)}\n\n".encode("utf-8")
        try:
            self.wfile.write(chunk)
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            # Client navigated away / aborted — stop streaming quietly.
            raise BrokenPipeError("client disconnected")

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", self._cors_origin())
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", "0")
        self.end_headers()

    # ------------------------------------------------------------------ GET
    def do_GET(self):
        try:
            path = self.path.split("?")[0]
            if path == "/":
                html = BASE_DIR / "index.html"
                if not html.exists():
                    self.send_json({"error": "index.html not found"}, 500)
                    return
                self.send_html(html.read_bytes())
            elif path == "/api/status":
                self.handle_status()
            elif path == "/api/threads":
                self.handle_threads()
            elif path == "/api/thread":
                self.handle_thread_read()
            elif path == "/api/login/wait":
                self.handle_login_wait()
            else:
                self.send_json({"error": "Not found"}, 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            traceback.print_exc()
            try:
                self.send_json({"error": str(e)}, 500)
            except Exception:
                pass

    # ------------------------------------------------------------------ POST
    def do_POST(self):
        try:
            path = self.path.split("?")[0]
            if not self._origin_allowed():
                self.send_json({"error": "forbidden origin"}, 403)
                return
            if path == "/api/chat":
                self.handle_chat()
            elif path == "/api/login":
                self.handle_login()
            elif path == "/api/logout":
                self.handle_logout()
            elif path == "/api/interrupt":
                self.handle_interrupt()
            elif path == "/api/approval":
                self.handle_approval()
            else:
                self.send_json({"error": "Not found"}, 404)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:
            traceback.print_exc()
            try:
                self.send_json({"error": str(e)}, 500)
            except Exception:
                pass

    # ---------------------------------------------------------- endpoints
    def handle_status(self):
        out = {"codex": False, "audacity": False, "audacityTools": [],
               "account": None, "model": CODEX_MODEL or None}
        try:
            b = get_bridge()
            out["codex"] = b.is_alive()
            try:
                acct = b.request("account/read", {}, timeout=15)
                out["account"] = acct
            except CodexError as e:
                out["accountError"] = str(e)
        except Exception as e:
            out["codexError"] = str(e)
        ok, tools, err = _audacity_mcp_status()
        out["audacity"] = ok
        out["audacityTools"] = tools
        if err:
            out["audacityError"] = err
        self.send_json(out)

    def handle_login(self):
        body = self._read_body()
        streamlined = bool(body.get("streamlined", True))
        try:
            b = get_bridge()
            resp = b.request("account/login/start",
                             {"type": "chatgpt",
                              "codexStreamlinedLogin": streamlined},
                             timeout=30)
        except CodexError as e:
            self.send_json({"error": str(e)}, 500)
            return
        auth_url = resp.get("authUrl")
        if auth_url:
            try:
                webbrowser.open(auth_url)
            except Exception:
                pass
        self.send_json(resp)

    def handle_login_wait(self):
        from urllib.parse import urlparse, parse_qs
        qs = parse_qs(urlparse(self.path).query)
        login_id = (qs.get("loginId") or [""])[0]
        try:
            b = get_bridge()
        except Exception as e:
            self.send_json({"error": str(e)}, 500)
            return
        ev = b.wait_login(login_id, timeout=300)
        if ev is None:
            self.send_json({"success": False, "error": "timeout"})
        else:
            self.send_json(ev)

    def handle_logout(self):
        try:
            b = get_bridge()
            b.request("account/logout", {}, timeout=15)
        except CodexError as e:
            self.send_json({"error": str(e)}, 500)
            return
        self.send_json({"ok": True})

    def handle_threads(self):
        try:
            b = get_bridge()
            resp = b.request("thread/list", {}, timeout=30)
        except CodexError as e:
            self.send_json({"error": str(e)}, 500)
            return
        threads = []
        for t in resp.get("data", []):
            if t.get("ephemeral"):
                continue
            threads.append({
                "id": t.get("id"),
                "title": t.get("name") or (t.get("preview") or "").strip()[:80]
                         or "Untitled",
                "preview": (t.get("preview") or "").strip()[:120],
                "createdAt": t.get("createdAt"),
                "updatedAt": t.get("updatedAt"),
                "instructionsVersion": _thread_instructions_version(t.get("id")),
            })
        self.send_json({"threads": threads,
                         "currentInstructionsVersion": INSTRUCTIONS_VERSION})

    def handle_thread_read(self):
        from urllib.parse import urlparse, parse_qs
        qs = parse_qs(urlparse(self.path).query)
        tid = (qs.get("id") or [""])[0]
        if not tid:
            self.send_json({"error": "missing id"}, 400)
            return
        try:
            b = get_bridge()
            # Resume so the app-server holds it in memory, then read history.
            try:
                b.request("thread/resume", {"threadId": tid}, timeout=30)
                b.loaded_threads.add(tid)
            except CodexError:
                pass
            resp = b.request("thread/read",
                             {"threadId": tid, "includeTurns": True}, timeout=30)
        except CodexError as e:
            self.send_json({"error": str(e)}, 500)
            return
        self.send_json({"messages": _extract_messages(resp),
                         "instructionsVersion": _thread_instructions_version(tid),
                         "currentInstructionsVersion": INSTRUCTIONS_VERSION})

    def handle_interrupt(self):
        body = self._read_body()
        tid = body.get("threadId")
        try:
            b = get_bridge()
            b.request("turn/interrupt", {"threadId": tid}, timeout=15)
        except CodexError as e:
            self.send_json({"error": str(e)}, 500)
            return
        self.send_json({"ok": True})

    def handle_approval(self):
        """User clicked 許可/拒否 on a shell/exec/file approval card."""
        body = self._read_body()
        rid = body.get("requestId")
        allow = bool(body.get("allow"))
        ok = False
        try:
            ok = get_bridge().resolve_approval(rid, allow)
        except Exception as e:
            self.send_json({"error": str(e)}, 500)
            return
        self.send_json({"ok": ok})

    # ----------------------------------------------------------- chat (SSE)
    def handle_chat(self):
        body = self._read_body()
        message = (body.get("message") or "").strip()
        thread_id = body.get("threadId")
        if not message:
            self.send_json({"error": "empty message"}, 400)
            return

        # Reject overlapping turns on the same existing thread before we do
        # anything else (new threads get a fresh id below, so they can't
        # collide with each other). Cleared in the `finally` below regardless
        # of how the turn ends (success, exception, or timeout).
        if thread_id and not _try_mark_inflight(thread_id):
            self.send_json({
                "error": "このスレッドは前のターンを実行中です。完了を待つか、"
                         "新しいチャットを開始してください。"}, 409)
            return
        marked_thread_id = thread_id  # what we actually marked in-flight (or None)

        try:
            try:
                b = get_bridge()
            except Exception as e:
                self.send_json({"error": f"Codex app-server failed to start: {e}"}, 500)
                return

            new_thread = False
            if not thread_id:
                try:
                    thread_id, _ = start_thread(b)
                    new_thread = True
                    b.loaded_threads.add(thread_id)
                except CodexError as e:
                    self.send_json({"error": str(e)}, 500)
                    return
                # The new thread's id is only known now — mark it in-flight so a
                # (very unlikely) racing request against the same id is caught.
                if not _try_mark_inflight(thread_id):
                    self.send_json({"error": "duplicate thread id"}, 409)
                    return
                marked_thread_id = thread_id
            elif thread_id not in b.loaded_threads:
                # Fresh app-server: load the thread from disk before the turn.
                try:
                    b.request("thread/resume", {"threadId": thread_id}, timeout=60)
                    b.loaded_threads.add(thread_id)
                except CodexError:
                    # Thread is gone (or unreadable) — start a new one instead.
                    _clear_inflight(marked_thread_id)
                    marked_thread_id = None
                    try:
                        thread_id, _ = start_thread(b)
                        new_thread = True
                        b.loaded_threads.add(thread_id)
                    except CodexError as e:
                        self.send_json({"error": str(e)}, 500)
                        return
                    if not _try_mark_inflight(thread_id):
                        self.send_json({"error": "duplicate thread id"}, 409)
                        return
                    marked_thread_id = thread_id

            q = b.subscribe(thread_id)
            self._sse_open()
            self._sse("meta", {"threadId": thread_id, "newThread": new_thread})

            # Kick off the turn in a background thread so we can stream immediately.
            turn_err = {}

            def _start_turn():
                try:
                    b.request(
                        "turn/start",
                        {"threadId": thread_id,
                         "input": [{"type": "text", "text": message}]},
                        timeout=4000,
                    )
                except CodexError as e:
                    turn_err["error"] = str(e)

            tt = threading.Thread(target=_start_turn, daemon=True)
            tt.start()

            try:
                self._stream_turn(q, thread_id)
            finally:
                b.unsubscribe(thread_id, q)
            if turn_err.get("error"):
                try:
                    self._sse("error", {"message": turn_err["error"]})
                except Exception:
                    pass
        finally:
            if marked_thread_id:
                _clear_inflight(marked_thread_id)

    def _stream_turn(self, q, thread_id):
        import queue as _queue
        idle_deadline = 4000  # seconds of total silence before giving up
        while True:
            try:
                msg = q.get(timeout=idle_deadline)
            except _queue.Empty:
                self._sse("error", {"message": "timed out waiting for Codex"})
                return
            if msg is _PROCESS_DOWN or msg == _PROCESS_DOWN:
                self._sse("error", {"message": "Codex app-server stopped"})
                return

            # Shell/exec/file approval request → ask the user in the UI. The turn
            # stays open; codex waits until the user POSTs /api/approval.
            if isinstance(msg, dict) and "_approval" in msg:
                self._sse("approval", msg["_approval"])
                continue

            method = msg.get("method", "")
            p = msg.get("params", {})

            if method == "item/agentMessage/delta":
                self._sse("delta", {"text": p.get("delta", "")})

            elif method in ("item/reasoning/textDelta",
                            "item/reasoning/summaryTextDelta"):
                self._sse("reasoning", {"text": p.get("delta", "")})

            elif method == "item/started":
                it = p.get("item", {})
                if it.get("type") == "mcpToolCall":
                    self._sse("tool_start", {
                        "id": it.get("id"),
                        "server": it.get("server"),
                        "tool": it.get("tool"),
                        "arguments": it.get("arguments"),
                    })

            elif method == "item/completed":
                it = p.get("item", {})
                itype = it.get("type")
                if itype == "mcpToolCall":
                    self._sse("tool_end", {
                        "id": it.get("id"),
                        "server": it.get("server"),
                        "tool": it.get("tool"),
                        "arguments": it.get("arguments"),
                        "status": it.get("status"),
                        "result": _tool_text(it.get("result")),
                    })
                elif itype == "agentMessage":
                    self._sse("message", {"text": it.get("text", "")})

            elif method == "turn/completed":
                self._sse("done", {"threadId": thread_id})
                return

            elif method == "turn/failed" or method == "error":
                self._sse("error", {"message": json.dumps(p)[:500]})
                return


# --------------------------------------------------------------------------- #
# Helpers to flatten Codex item shapes
# --------------------------------------------------------------------------- #

def _tool_text(result):
    if not isinstance(result, dict):
        return ""
    content = result.get("content", [])
    texts = [c.get("text", "") for c in content
             if isinstance(c, dict) and c.get("type") == "text"]
    return "\n".join(t for t in texts if t).strip()


def _extract_messages(thread_read_resp):
    """Flatten a thread/read response into [{role, text, tools?}] for the UI."""
    out = []
    thread = thread_read_resp.get("thread", thread_read_resp)
    turns = thread.get("turns", []) if isinstance(thread, dict) else []
    for turn in turns:
        items = turn.get("items", []) if isinstance(turn, dict) else []
        for it in items:
            itype = it.get("type")
            if itype == "userMessage":
                txt = _content_text(it.get("content"))
                if txt:
                    out.append({"role": "user", "text": txt})
            elif itype == "agentMessage":
                txt = it.get("text", "")
                if txt:
                    out.append({"role": "assistant", "text": txt})
            elif itype == "mcpToolCall":
                out.append({"role": "tool",
                            "tool": it.get("tool"),
                            "server": it.get("server"),
                            "arguments": it.get("arguments"),
                            "result": _tool_text(it.get("result"))})
    return out


def _content_text(content):
    if isinstance(content, str):
        return content
    if isinstance(content, list):
        parts = [c.get("text", "") for c in content
                 if isinstance(c, dict) and c.get("type") in ("text", "input_text")]
        return " ".join(p for p in parts if p).strip()
    return ""


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #

def main():
    setup_codex_home()
    print("Audacity MCP Companion (Codex app-server edition)")
    print(f"  Web UI      : http://{COMPANION_HOST}:{COMPANION_PORT}")
    print(f"  Audacity MCP: {MCP_URL}")
    print(f"  CODEX_HOME  : {CODEX_HOME}")
    print(f"  Model       : {CODEX_MODEL or '(account default)'}")

    # Bind the port FIRST, before warm-starting the Codex app-server: an
    # accidental second launch should fail fast on the port conflict instead
    # of spawning (and then orphaning) another codex subprocess.
    try:
        server = http.server.ThreadingHTTPServer((COMPANION_HOST, COMPANION_PORT), Handler)
    except OSError as e:
        print(f"ポート {COMPANION_PORT} は使用中です — companion が既に起動している"
              f"可能性があります。http://{COMPANION_HOST}:{COMPANION_PORT} を開くか、"
              f"既存プロセスを停止してください。({e})")
        sys.exit(1)

    # Warm-start the app server so the first chat is fast and we surface auth
    # problems early.
    try:
        b = get_bridge()
        print(f"  app-server  : up (codexHome={b.initialize_result.get('codexHome')})")
        try:
            acct = b.request("account/read", {}, timeout=15)
            a = (acct or {}).get("account")
            if a and a.get("type") == "chatgpt":
                print(f"  Login       : ChatGPT ({a.get('email') or a.get('planType')})")
            elif a:
                print(f"  Login       : {a.get('type')}")
            else:
                print("  Login       : NOT logged in — use the UI to log in")
        except Exception as e:
            print(f"  Login       : (status unknown: {e})")
    except Exception as e:
        print(f"  app-server  : FAILED to start: {e}")

    ok, tools, err = _audacity_mcp_status()
    print(f"  Audacity    : {'connected, tools=' + str(tools) if ok else 'NOT reachable (' + str(err) + ')'}")

    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        server.shutdown()
        if bridge is not None:
            bridge.stop()


if __name__ == "__main__":
    main()
