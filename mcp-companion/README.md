# Audacity MCP Companion

A local web chat app that lets you drive Audacity in natural language via an LLM.
The assistant runs a server-side agent loop, calling Audacity's built-in MCP server
to execute scripting commands on your behalf.

---

## Prerequisites

1. **Audacity** (the fork with `mod-mcp-server`) must be running, with the MCP server
   enabled and listening at `http://127.0.0.1:4830/mcp`.

2. **Python 3** — uses the standard library only (no pip installs needed).
   macOS ships `/usr/bin/python3`; install Xcode Command Line Tools if missing.

3. An **LLM API key** (Anthropic or OpenAI):

   ```sh
   export ANTHROPIC_API_KEY=sk-ant-...
   # — or —
   export OPENAI_API_KEY=sk-...
   ```

---

## Quick start

```sh
cd mcp-companion
./run.sh
```

`run.sh` checks for Python 3, warns if no API key is set, starts `server.py`,
and opens `http://127.0.0.1:8765` in your default browser.

**Manual start:**

```sh
python3 server.py
# then open http://127.0.0.1:8765
```

---

## Configuration (environment variables)

| Variable | Default | Description |
|---|---|---|
| `AI_PROVIDER` | `anthropic` | `anthropic` or `openai` |
| `AI_MODEL` | `claude-sonnet-4-6` / `gpt-4o` | Model name |
| `ANTHROPIC_API_KEY` | _(none)_ | Anthropic API key |
| `OPENAI_API_KEY` | _(none)_ | OpenAI API key |
| `OPENAI_BASE_URL` | `https://api.openai.com` | Override for Ollama, LM Studio, etc. |
| `MCP_URL` | `http://127.0.0.1:4830/mcp` | Audacity MCP endpoint |
| `COMPANION_PORT` | `8765` | Web UI port |
| `COMPANION_HOST` | `127.0.0.1` | Bind address |

Example — use a local Ollama model:

```sh
export AI_PROVIDER=openai
export AI_MODEL=llama3.2
export OPENAI_BASE_URL=http://localhost:11434
export OPENAI_API_KEY=ollama   # required but ignored by Ollama
python3 server.py
```

---

## Features

- **Chat UI** — user/assistant message bubbles with a scrollable conversation history.
- **Tool call log** — each `run_command` / `get_info` call is shown as a collapsible
  block with input and Audacity's response.
- **Live MCP indicator** — green dot = Audacity reachable; red = offline. Polled every 15 s.
- **Provider/model badge** — shown in the header; pulls from `/api/config`.
- **Conversation continuity** — the full message history is sent with every request so
  the assistant remembers context.
- **Example prompts** — click any chip in the empty state to pre-fill the input.
- **Both Anthropic and OpenAI** agent loop formats implemented.

---

## Troubleshooting

**MCP indicator is red / "MCP offline"**
- Make sure Audacity is running.
- Check that `mod-mcp-server` is enabled in Audacity preferences.
- Verify it listens on port 4830: `curl -s http://127.0.0.1:4830/mcp`
- If it runs on a different port, set `export MCP_URL=http://127.0.0.1:<port>/mcp`

**"No API key detected" banner**
- Export the key before starting the server: `export ANTHROPIC_API_KEY=sk-ant-...`
- The key is resolved server-side and never sent to the browser.

**"Anthropic API HTTP 401"**
- Your API key is invalid or expired. Generate a new one at console.anthropic.com.

**"Anthropic API HTTP 529 / 529 overloaded"**
- Anthropic is rate-limiting. Wait a moment and retry.

**Port 8765 already in use**
- `export COMPANION_PORT=9000 && python3 server.py`

**Server crashes on startup**
- Requires Python 3.7+ (for `ThreadingHTTPServer`). Check: `python3 --version`

---

## Architecture

```
Browser (index.html)
   │  POST /api/chat  {messages:[…]}
   ▼
server.py  (ThreadingHTTPServer on :8765)
   │  run_agent_anthropic / run_agent_openai
   │    → POST https://api.anthropic.com/v1/messages  (or OpenAI)
   │    ← tool_use blocks
   │  mcp_call_tool(name, args)
   │    → POST http://127.0.0.1:4830/mcp  (JSON-RPC 2.0)
   │    ← result text
   │  (loop up to 12 tool rounds)
   │  returns {reply, toolLog, messages}
   ▼
Browser renders bubbles + collapsible tool blocks
```

---

## Files

| File | Purpose |
|---|---|
| `server.py` | Python web server + agent loop + MCP proxy |
| `index.html` | Self-contained chat UI (inline CSS + vanilla JS) |
| `run.sh` | Convenience launcher (checks deps, opens browser) |
| `README.md` | This file |
