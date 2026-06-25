#!/usr/bin/env python3
"""
codex_bridge.py — a thin client for the Codex *app server*.

The app server (`codex app-server`) speaks JSON-RPC 2.0 over stdio using
newline-delimited JSON (one message per line).  It provides, for free, the two
things the Audacity companion wants but did not have before:

  1. ChatGPT *web login* (no API key) — auth is managed by Codex.
  2. Persistent conversation *memory* — every thread is written to disk as a
     rollout `.jsonl` and can be listed/resumed across restarts.

The agent's tools come from MCP servers Codex is configured with.  We point
Codex at the running Audacity MCP server (http://127.0.0.1:4830/mcp) via an
*isolated* CODEX_HOME so the only tools in scope are Audacity's `run_command`
and `get_info` — none of the user's global MCP servers leak in.

This module owns a single long-lived `codex app-server` subprocess and
multiplexes:
  * synchronous requests        -> `request(method, params)`
  * fire-and-forget notifies    -> `notify(method, params)`
  * streamed server notifications, routed to per-thread subscriber queues
  * server -> client requests (approvals), auto-answered

Only the Python standard library is used (no pip).
"""

import json
import os
import queue
import subprocess
import threading
import time
from pathlib import Path


# Sentinel placed on a subscriber queue when the underlying process dies.
_PROCESS_DOWN = {"_bridge": "process_down"}


class CodexError(RuntimeError):
    """A JSON-RPC error returned by the app server."""

    def __init__(self, code, message, data=None):
        super().__init__(f"Codex app-server error {code}: {message}")
        self.code = code
        self.message = message
        self.data = data


class CodexAppServer:
    """Owns one `codex app-server` subprocess and the JSON-RPC plumbing."""

    def __init__(self, codex_home, cwd=None, codex_bin="codex",
                 client_name="audacity-companion", client_version="0.2.0",
                 log=None):
        self.codex_home = str(codex_home)
        self.cwd = str(cwd) if cwd else None
        self.codex_bin = codex_bin
        self.client_name = client_name
        self.client_version = client_version
        self._log = log or (lambda *a: None)

        self._proc = None
        self._stdin_lock = threading.Lock()
        self._id_lock = threading.Lock()
        self._next_id = 0

        # id -> {"event": Event, "result": ..., "error": ...}
        self._pending = {}
        self._pending_lock = threading.Lock()

        # threadId -> set(Queue)   (per-chat streaming subscribers)
        self._subs = {}
        self._subs_lock = threading.Lock()

        # loginId -> {"success": bool, "error": str|None}  (login completion)
        self._login_events = {}
        self._login_lock = threading.Lock()
        # Listeners notified on ANY account/* notification (for "logged in?" UI).
        self._account_listeners = []

        self.initialize_result = None
        self._started = False

        # Threads this app-server process has already started or resumed (loaded
        # into memory). turn/start requires the thread to be loaded first.
        self.loaded_threads = set()

    # ------------------------------------------------------------------ start
    def start(self, initialize_timeout=30):
        env = dict(os.environ)
        env["CODEX_HOME"] = self.codex_home
        Path(self.codex_home).mkdir(parents=True, exist_ok=True)

        args = [self.codex_bin, "app-server"]
        self._proc = subprocess.Popen(
            args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
            env=env,
            cwd=self.cwd,
        )
        threading.Thread(target=self._read_stdout, daemon=True).start()
        threading.Thread(target=self._read_stderr, daemon=True).start()

        # Handshake: initialize (request) then `initialized` (notification).
        self.initialize_result = self.request(
            "initialize",
            {
                "clientInfo": {
                    "name": self.client_name,
                    "title": "Audacity Companion",
                    "version": self.client_version,
                },
                "capabilities": None,
            },
            timeout=initialize_timeout,
        )
        self.notify("initialized")
        self._started = True
        return self.initialize_result

    def is_alive(self):
        return self._proc is not None and self._proc.poll() is None

    def stop(self):
        if self._proc is None:
            return
        try:
            self._proc.terminate()
            self._proc.wait(timeout=5)
        except Exception:
            try:
                self._proc.kill()
            except Exception:
                pass

    # ------------------------------------------------------------- transport
    def _alloc_id(self):
        with self._id_lock:
            self._next_id += 1
            return self._next_id

    def _write(self, obj):
        line = json.dumps(obj) + "\n"
        with self._stdin_lock:
            if not self.is_alive():
                raise CodexError(-1, "app-server process is not running")
            self._proc.stdin.write(line)
            self._proc.stdin.flush()

    def request(self, method, params=None, timeout=120):
        """Synchronous JSON-RPC request. `params` defaults to {} because the
        app server rejects requests with a missing `params` field."""
        rid = self._alloc_id()
        ev = threading.Event()
        slot = {"event": ev, "result": None, "error": None}
        with self._pending_lock:
            self._pending[rid] = slot
        self._write({
            "jsonrpc": "2.0",
            "id": rid,
            "method": method,
            "params": {} if params is None else params,
        })
        if not ev.wait(timeout):
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise CodexError(-2, f"timeout waiting for response to {method!r}")
        with self._pending_lock:
            self._pending.pop(rid, None)
        if slot["error"] is not None:
            err = slot["error"]
            raise CodexError(err.get("code", -1), err.get("message", "error"),
                             err.get("data"))
        return slot["result"]

    def notify(self, method, params=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        self._write(msg)

    def _respond(self, rid, result=None, error=None):
        msg = {"jsonrpc": "2.0", "id": rid}
        if error is not None:
            msg["error"] = error
        else:
            msg["result"] = result if result is not None else {}
        try:
            self._write(msg)
        except Exception as e:
            self._log("failed to send response:", e)

    # --------------------------------------------------------- subscriptions
    def subscribe(self, thread_id):
        q = queue.Queue()
        with self._subs_lock:
            self._subs.setdefault(thread_id, set()).add(q)
        return q

    def unsubscribe(self, thread_id, q):
        with self._subs_lock:
            s = self._subs.get(thread_id)
            if s and q in s:
                s.discard(q)
                if not s:
                    self._subs.pop(thread_id, None)

    def _route_to_thread(self, thread_id, message):
        if not thread_id:
            return
        with self._subs_lock:
            subs = list(self._subs.get(thread_id, ()))
        for q in subs:
            q.put(message)

    # --------------------------------------------------------------- login
    def add_account_listener(self, fn):
        self._account_listeners.append(fn)

    def get_login_event(self, login_id):
        with self._login_lock:
            return self._login_events.get(login_id)

    def wait_login(self, login_id, timeout=300):
        deadline = time.time() + timeout
        while time.time() < deadline:
            ev = self.get_login_event(login_id)
            if ev is not None:
                return ev
            time.sleep(0.3)
        return None

    # --------------------------------------------------------- reader threads
    def _read_stdout(self):
        proc = self._proc
        try:
            for line in proc.stdout:
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except Exception as e:
                    self._log("non-JSON stdout:", line[:200], e)
                    continue
                self._dispatch(msg)
        except Exception as e:
            self._log("stdout reader stopped:", e)
        finally:
            self._fail_all_pending("app-server stdout closed")

    def _read_stderr(self):
        proc = self._proc
        try:
            for line in proc.stderr:
                if line.strip():
                    self._log("[codex stderr]", line.rstrip())
        except Exception:
            pass

    def _fail_all_pending(self, reason):
        with self._pending_lock:
            slots = list(self._pending.values())
            self._pending.clear()
        for slot in slots:
            slot["error"] = {"code": -3, "message": reason}
            slot["event"].set()
        # Wake any active chat streams so they don't hang.
        with self._subs_lock:
            all_qs = [q for s in self._subs.values() for q in s]
        for q in all_qs:
            q.put(dict(_PROCESS_DOWN))

    def _dispatch(self, msg):
        # 1) Response to one of our requests.
        if "id" in msg and ("result" in msg or "error" in msg) and "method" not in msg:
            rid = msg["id"]
            with self._pending_lock:
                slot = self._pending.get(rid)
            if slot is not None:
                slot["result"] = msg.get("result")
                slot["error"] = msg.get("error")
                slot["event"].set()
            return

        # 2) Server -> client request (has both method and id): auto-answer.
        if "method" in msg and "id" in msg:
            self._handle_server_request(msg)
            return

        # 3) Notification (method, no id).
        if "method" in msg:
            self._handle_notification(msg)
            return

        self._log("unrecognized message:", json.dumps(msg)[:200])

    def _handle_server_request(self, msg):
        method = msg.get("method", "")
        rid = msg["id"]
        # Auto-approve everything: this is a localhost tool the user explicitly
        # drives.  Approval requests should not normally fire (approval_policy
        # is "never"), but answer defensively so a turn never deadlocks.
        if method.endswith("requestApproval"):
            self._respond(rid, {"decision": "acceptForSession"})
        elif method == "item/tool/requestUserInput":
            self._respond(rid, {"response": ""})
        else:
            # Unknown server request — reply with empty result rather than hang.
            self._log("auto-empty reply to server request:", method)
            self._respond(rid, {})

    def _handle_notification(self, msg):
        method = msg.get("method", "")
        params = msg.get("params") or {}

        if method.startswith("account/login"):
            login_id = params.get("loginId")
            if method == "account/login/completed":
                with self._login_lock:
                    self._login_events[login_id or "_"] = {
                        "success": bool(params.get("success")),
                        "error": params.get("error"),
                    }

        if method.startswith("account/"):
            for fn in list(self._account_listeners):
                try:
                    fn(method, params)
                except Exception:
                    pass

        # Route thread-scoped notifications to any active chat stream.
        tid = params.get("threadId")
        if tid:
            self._route_to_thread(tid, {"method": method, "params": params})
