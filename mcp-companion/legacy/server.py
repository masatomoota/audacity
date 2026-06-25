#!/usr/bin/env python3
"""
Audacity MCP Companion — localhost web server
Serves the chat UI and proxies LLM + MCP calls server-side.
"""

import http.server
import json
import os
import threading
import urllib.request
import urllib.error
import traceback
from pathlib import Path


def _load_dotenv():
    """Load KEY=VALUE pairs from .env into os.environ without overriding vars
    that are already set. Looks at $ENV_FILE, this script's dir, and the CWD."""
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

# ---------------------------------------------------------------------------
# Config (from environment)
# ---------------------------------------------------------------------------
COMPANION_HOST = os.environ.get("COMPANION_HOST", "127.0.0.1")
COMPANION_PORT = int(os.environ.get("COMPANION_PORT", "8765"))

AI_PROVIDER = os.environ.get("AI_PROVIDER", "anthropic").lower()
_default_model = "claude-sonnet-4-6" if AI_PROVIDER == "anthropic" else "gpt-4o"
AI_MODEL = os.environ.get("AI_MODEL", _default_model)

ANTHROPIC_API_KEY = os.environ.get("ANTHROPIC_API_KEY", "")
OPENAI_API_KEY = os.environ.get("OPENAI_API_KEY", "")
OPENAI_BASE_URL = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com").rstrip("/")
MCP_URL = os.environ.get("MCP_URL", "http://127.0.0.1:4830/mcp")

BASE_DIR = Path(__file__).parent.resolve()
MAX_TOOL_ROUNDS = 12

# ---------------------------------------------------------------------------
# MCP helpers
# ---------------------------------------------------------------------------

_mcp_id_counter = 0
_mcp_id_lock = threading.Lock()


def _next_mcp_id():
    global _mcp_id_counter
    with _mcp_id_lock:
        _mcp_id_counter += 1
        return _mcp_id_counter


def mcp_request(method, params=None):
    """Send a JSON-RPC 2.0 request to the MCP server and return the parsed response dict."""
    payload = {
        "jsonrpc": "2.0",
        "id": _next_mcp_id(),
        "method": method,
    }
    if params is not None:
        payload["params"] = params

    body = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        MCP_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.URLError as e:
        raise ConnectionError(f"MCP unreachable: {e}") from e
    except Exception as e:
        raise RuntimeError(f"MCP error: {e}") from e


def mcp_call_tool(name, arguments):
    """Call an MCP tool and return the text output string."""
    resp = mcp_request("tools/call", {"name": name, "arguments": arguments})
    if "error" in resp:
        return f"[MCP error] {resp['error']}"
    result = resp.get("result", {})
    if result.get("isError"):
        content = result.get("content", [])
        texts = [c.get("text", "") for c in content if c.get("type") == "text"]
        return "[Audacity error] " + " ".join(texts)
    content = result.get("content", [])
    texts = [c.get("text", "") for c in content if c.get("type") == "text"]
    return " ".join(texts) if texts else "(no output)"


def mcp_ping():
    """Return (ok, tool_names, error_str)."""
    try:
        resp = mcp_request("tools/list")
        if "error" in resp:
            return False, [], str(resp["error"])
        tools = resp.get("result", {}).get("tools", [])
        names = [t.get("name", "") for t in tools]
        return True, names, None
    except Exception as e:
        return False, [], str(e)


# ---------------------------------------------------------------------------
# Tool definitions (for the LLM)
# ---------------------------------------------------------------------------

MCP_TOOL_DEFS = [
    {
        "name": "run_command",
        "description": (
            "Execute any Audacity scripting command. "
            "Examples: 'NewMonoTrack', 'NewStereoTrack', "
            "'Select: Start=0 End=3', 'Amplify: Ratio=0.5', "
            "'Normalize:', 'FadeIn:', 'FadeOut:', "
            "'Tone: Frequency=440 Amplitude=0.5 Waveform=Sine Start=0 End=3', "
            "'Chirp: StartFrequency=100 EndFrequency=2000 Start=0 End=3', "
            "'Noise: Type=White Level=0.5 Start=0 End=3', "
            "'Import2: Filename=\"/path/to/file.wav\"', "
            "'Export2: Filename=\"/tmp/output.wav\"'. "
            "Call get_info with type=Commands for the full catalog."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "command": {
                    "type": "string",
                    "description": "Audacity scripting command string",
                }
            },
            "required": ["command"],
        },
    },
    {
        "name": "get_info",
        "description": (
            "Query Audacity for information. "
            "type options: Commands (full command catalog), Menus, Tracks, Clips, "
            "Labels, Selection, Preferences, Boxes. "
            "Use type=Tracks or type=Selection to perceive current state before acting. "
            "Use type=Commands to discover available commands and their parameters."
        ),
        "input_schema": {
            "type": "object",
            "properties": {
                "type": {
                    "type": "string",
                    "enum": [
                        "Commands",
                        "Menus",
                        "Tracks",
                        "Clips",
                        "Labels",
                        "Selection",
                        "Preferences",
                        "Boxes",
                    ],
                    "description": "What kind of information to retrieve",
                },
                "format": {
                    "type": "string",
                    "description": "Response format (default: JSON)",
                    "default": "JSON",
                },
            },
            "required": ["type"],
        },
    },
]

SYSTEM_PROMPT = """You are an AI assistant that controls Audacity through its MCP (Model Context Protocol) scripting interface. You have two tools:

1. **run_command** — executes Audacity scripting commands. Key examples:
   - Track creation: `NewMonoTrack`, `NewStereoTrack`
   - Selection: `Select: Start=0 End=3` (seconds), `SelectAll:`, `SelectNone:`
   - Generators (require a selection first): `Tone: Frequency=440 Amplitude=0.5 Waveform=Sine Start=0 End=3`, `Chirp: StartFrequency=100 EndFrequency=2000 Start=0 End=3`, `Noise: Type=White Level=0.5 Start=0 End=3`
   - Effects: `Amplify: Ratio=0.5`, `Normalize:`, `FadeIn:`, `FadeOut:`, `Reverb:`, `Compressor:`
   - File I/O: `Import2: Filename="/path/to/file.wav"`, `Export2: Filename="/tmp/output.wav"`
   - Playback: `Play:`, `Stop:`, `Rewind:`, `SkipToStart:`
   - Edit: `Undo:`, `Redo:`, `Delete:`, `Trim:`, `Split:`

2. **get_info** — query Audacity state:
   - `type=Commands` → full machine-readable command catalog (id, name, params)
   - `type=Tracks` → list of current tracks (name, type, channels, length)
   - `type=Selection` → current selection (start, end, low, high)
   - `type=Clips` → clips info
   - `type=Labels` → label tracks

**Working approach:**
- Before acting, use get_info with type=Tracks or type=Selection to understand current state.
- When unsure about a command's syntax or parameters, use get_info with type=Commands.
- Commands that operate on audio (effects, generators, export) typically require a selection — use `Select:` first.
- Be cautious with destructive operations. Mention what you're about to do before doing it.
- Chain multiple run_command calls to accomplish complex tasks step by step.
- Report back what happened after each action.
- If a command fails, try to understand why and suggest alternatives.

You are helpful, concise, and transparent about what Audacity operations you're performing."""


# ---------------------------------------------------------------------------
# Agent loop — Anthropic
# ---------------------------------------------------------------------------

def _anthropic_tool_defs():
    return [
        {
            "name": t["name"],
            "description": t["description"],
            "input_schema": t["input_schema"],
        }
        for t in MCP_TOOL_DEFS
    ]


def run_agent_anthropic(messages):
    """Run the Anthropic agent loop. Returns (reply_text, tool_log, updated_messages)."""
    api_key = ANTHROPIC_API_KEY
    if not api_key:
        raise ValueError(
            "ANTHROPIC_API_KEY is not set. Please run: export ANTHROPIC_API_KEY=sk-ant-..."
        )

    tool_log = []
    msgs = list(messages)

    for _round in range(MAX_TOOL_ROUNDS):
        payload = {
            "model": AI_MODEL,
            "max_tokens": 2048,
            "system": SYSTEM_PROMPT,
            "tools": _anthropic_tool_defs(),
            "messages": msgs,
        }
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            "https://api.anthropic.com/v1/messages",
            data=body,
            headers={
                "x-api-key": api_key,
                "anthropic-version": "2023-06-01",
                "content-type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            snippet = e.read().decode("utf-8", errors="replace")[:500]
            raise RuntimeError(
                f"Anthropic API HTTP {e.code}: {snippet}"
            ) from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"Anthropic API unreachable: {e}") from e

        stop_reason = data.get("stop_reason")
        content = data.get("content", [])

        # Append assistant turn
        msgs.append({"role": "assistant", "content": content})

        if stop_reason != "tool_use":
            # Extract final text
            texts = [c.get("text", "") for c in content if c.get("type") == "text"]
            return " ".join(texts), tool_log, msgs

        # Process tool calls
        tool_results = []
        for block in content:
            if block.get("type") != "tool_use":
                continue
            tool_name = block["name"]
            tool_input = block.get("input", {})
            tool_use_id = block["id"]

            # Execute
            try:
                output = mcp_call_tool(tool_name, tool_input)
            except ConnectionError as e:
                output = f"[MCP unreachable — is Audacity running?] {e}"
            except Exception as e:
                output = f"[Tool error] {e}"

            tool_log.append({"tool": tool_name, "input": tool_input, "output": output})
            tool_results.append(
                {
                    "type": "tool_result",
                    "tool_use_id": tool_use_id,
                    "content": output,
                }
            )

        # All tool results in ONE user message
        msgs.append({"role": "user", "content": tool_results})

    # Exhausted rounds
    return (
        "I reached the maximum number of tool-use rounds. The task may be incomplete.",
        tool_log,
        msgs,
    )


# ---------------------------------------------------------------------------
# Agent loop — OpenAI
# ---------------------------------------------------------------------------

def _openai_tool_defs():
    return [
        {
            "type": "function",
            "function": {
                "name": t["name"],
                "description": t["description"],
                "parameters": t["input_schema"],
            },
        }
        for t in MCP_TOOL_DEFS
    ]


def run_agent_openai(messages):
    """Run the OpenAI agent loop. Returns (reply_text, tool_log, updated_messages)."""
    api_key = OPENAI_API_KEY
    if not api_key:
        raise ValueError(
            "OPENAI_API_KEY is not set. Please run: export OPENAI_API_KEY=sk-..."
        )

    tool_log = []
    # Prepend system message
    sys_msg = {"role": "system", "content": SYSTEM_PROMPT}
    msgs = [sys_msg] + list(messages)

    for _round in range(MAX_TOOL_ROUNDS):
        payload = {
            "model": AI_MODEL,
            "messages": msgs,
            "tools": _openai_tool_defs(),
            "tool_choice": "auto",
        }
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            f"{OPENAI_BASE_URL}/v1/chat/completions",
            data=body,
            headers={
                "Authorization": f"Bearer {api_key}",
                "Content-Type": "application/json",
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            snippet = e.read().decode("utf-8", errors="replace")[:500]
            raise RuntimeError(
                f"OpenAI API HTTP {e.code}: {snippet}"
            ) from e
        except urllib.error.URLError as e:
            raise RuntimeError(f"OpenAI API unreachable: {e}") from e

        choice = data.get("choices", [{}])[0]
        message = choice.get("message", {})
        finish_reason = choice.get("finish_reason", "stop")

        msgs.append(message)

        if finish_reason != "tool_calls" or not message.get("tool_calls"):
            return message.get("content") or "", tool_log, msgs[1:]  # strip sys msg

        # Process tool calls
        for tc in message.get("tool_calls", []):
            fn = tc.get("function", {})
            tool_name = fn.get("name", "")
            try:
                tool_input = json.loads(fn.get("arguments", "{}"))
            except json.JSONDecodeError:
                tool_input = {}
            tool_call_id = tc.get("id", "")

            try:
                output = mcp_call_tool(tool_name, tool_input)
            except ConnectionError as e:
                output = f"[MCP unreachable — is Audacity running?] {e}"
            except Exception as e:
                output = f"[Tool error] {e}"

            tool_log.append({"tool": tool_name, "input": tool_input, "output": output})
            msgs.append(
                {
                    "role": "tool",
                    "tool_call_id": tool_call_id,
                    "content": output,
                }
            )

    return (
        "I reached the maximum number of tool-use rounds. The task may be incomplete.",
        tool_log,
        msgs[1:],  # strip sys msg
    )


# ---------------------------------------------------------------------------
# Dispatch agent
# ---------------------------------------------------------------------------

def run_agent(messages):
    if AI_PROVIDER == "openai":
        return run_agent_openai(messages)
    return run_agent_anthropic(messages)


# ---------------------------------------------------------------------------
# HTTP request handler
# ---------------------------------------------------------------------------

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Quieter logging
        print(f"[{self.address_string()}] {fmt % args}")

    def send_json(self, data, status=200):
        body = json.dumps(data).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def send_html(self, content):
        body = content if isinstance(content, bytes) else content.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        try:
            path = self.path.split("?")[0]
            if path == "/":
                html_path = BASE_DIR / "index.html"
                if not html_path.exists():
                    self.send_json({"error": "index.html not found"}, 500)
                    return
                self.send_html(html_path.read_bytes())
            elif path == "/api/config":
                provider = AI_PROVIDER
                model = AI_MODEL
                if provider == "anthropic":
                    has_key = bool(ANTHROPIC_API_KEY)
                else:
                    has_key = bool(OPENAI_API_KEY)
                self.send_json(
                    {
                        "provider": provider,
                        "model": model,
                        "mcpUrl": MCP_URL,
                        "hasKey": has_key,
                    }
                )
            elif path == "/api/mcp_status":
                ok, tools, error = mcp_ping()
                resp = {"ok": ok, "tools": tools}
                if error:
                    resp["error"] = error
                self.send_json(resp)
            else:
                self.send_json({"error": "Not found"}, 404)
        except Exception as e:
            self.send_json({"error": traceback.format_exc()}, 500)

    def do_POST(self):
        try:
            path = self.path.split("?")[0]
            if path == "/api/chat":
                length = int(self.headers.get("Content-Length", 0))
                raw = self.rfile.read(length)
                try:
                    body = json.loads(raw.decode("utf-8"))
                except json.JSONDecodeError as e:
                    self.send_json({"error": f"Bad JSON: {e}"})
                    return

                messages = body.get("messages", [])
                if not messages:
                    self.send_json({"error": "No messages provided"})
                    return

                try:
                    reply, tool_log, updated_messages = run_agent(messages)
                    self.send_json(
                        {
                            "reply": reply,
                            "toolLog": tool_log,
                            "messages": updated_messages,
                        }
                    )
                except ValueError as e:
                    # API key missing
                    self.send_json({"error": str(e)})
                except RuntimeError as e:
                    # API / MCP errors
                    self.send_json({"error": str(e)})
                except Exception as e:
                    self.send_json({"error": traceback.format_exc()})
            else:
                self.send_json({"error": "Not found"}, 404)
        except Exception as e:
            self.send_json({"error": traceback.format_exc()}, 500)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    server = http.server.ThreadingHTTPServer((COMPANION_HOST, COMPANION_PORT), Handler)
    url = f"http://{COMPANION_HOST}:{COMPANION_PORT}"
    print(f"Audacity MCP Companion running at {url}")
    print(f"  Provider : {AI_PROVIDER}")
    print(f"  Model    : {AI_MODEL}")
    print(f"  MCP URL  : {MCP_URL}")
    if AI_PROVIDER == "anthropic":
        print(f"  API key  : {'SET' if ANTHROPIC_API_KEY else 'NOT SET — export ANTHROPIC_API_KEY=...'}")
    else:
        print(f"  API key  : {'SET' if OPENAI_API_KEY else 'NOT SET — export OPENAI_API_KEY=...'}")
    print("Press Ctrl+C to stop.")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()
