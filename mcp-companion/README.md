# Otis (Codex app-server edition)

A local web chat that drives Otis (the Audacity-based editor) in natural
language. The "brain" is the **Codex app server** (`codex app-server`) rather
than a direct API-key call, so you get two things for free:

- **Web login** — sign in with your **ChatGPT account** in the browser. No API
  key to paste or store.
- **Memory** — every conversation is a Codex *thread*, persisted to disk as a
  rollout `.jsonl`. Past chats are listed in the sidebar and can be resumed, and
  context survives restarts.

Codex runs the agent loop and reaches Otis through the running
`mod-mcp-server` (`http://127.0.0.1:4830/mcp`), wired up in an **isolated
`CODEX_HOME`** so only Otis's `run_command` / `get_info` tools are in scope —
none of your global Codex MCP servers leak in.

> The previous API-key companion (direct Anthropic/OpenAI agent loop) is kept
> under [`legacy/`](legacy/) for reference.

---

## Prerequisites

1. **Otis** (the Audacity-based editor, with `mod-mcp-server` enabled) running
   and listening at `http://127.0.0.1:4830/mcp`.
2. **Codex CLI** (`codex`) installed and on `PATH`. First-time login:
   ```sh
   codex login          # opens a browser; sign in with ChatGPT
   ```
   The companion will reuse this login automatically (see below), or you can log
   in from the chat UI.
3. **Python 3.7+** — standard library only, no pip installs.

---

## Quick start

```sh
cd mcp-companion
./run.sh
```

`run.sh` checks Python + the Codex CLI, reminds you if Otis isn't reachable,
starts `server.py`, and opens `http://127.0.0.1:8765`.

On first launch the server starts `codex app-server`, and — if you're already
logged into Codex globally — **inherits that ChatGPT login** (copies
`~/.codex/auth.json` into the companion's isolated home) so you don't have to log
in again. If you're not logged in, click **"Log in with ChatGPT"** in the UI; it
opens the OAuth page in your browser and the chat unlocks once you finish.

---

## Configuration (environment variables)

| Variable | Default | Description |
|---|---|---|
| `MCP_URL` | `http://127.0.0.1:4830/mcp` | Audacity MCP endpoint |
| `COMPANION_PORT` | `8765` | Web UI port |
| `COMPANION_HOST` | `127.0.0.1` | Bind address |
| `CODEX_BIN` | `codex` | Path to the Codex CLI |
| `CODEX_MODEL` | _(account default)_ | Override the model (e.g. a faster one) |
| `CODEX_HOME_COMPANION` | `mcp-companion/.codex` | Isolated Codex home (auth + threads + config) |
| `CODEX_WORKDIR` | `<home>/workdir` | Working dir for Codex threads |
| `GLOBAL_CODEX_HOME` | `~/.codex` | Where to inherit an existing ChatGPT login from |
| `CODEX_SANDBOX` | `danger-full-access` | Codex sandbox mode (see note below) |

A `.env` file in this directory is auto-loaded (it does not override already-set
vars). `.env` and `.codex/` are git-ignored.

> **Sandbox note.** The generated `config.toml` uses
> `sandbox_mode = "danger-full-access"`. Codex's restricted sandboxes
> (`read-only` / `workspace-write`) block the agent's MCP-over-HTTP call to
> Otis's localhost server (the tool comes back empty / "rejected"). Full
> access is acceptable here: it's a localhost, user-driven audio tool, the Codex
> home is isolated to this companion, `approval_policy = "never"`, and the
> developer instructions forbid shell/file use. Override with `CODEX_SANDBOX` if
> you prefer.

---

## Features

- **ChatGPT web login** — auth handled by Codex; nothing stored by the companion
  beyond the isolated `.codex/` home.
- **Persistent memory** — conversations are Codex threads on disk; the sidebar
  lists past chats, **+ New chat** starts a fresh one, and clicking a past chat
  resumes it with full context.
- **Streaming** — assistant text streams token-by-token over Server-Sent Events;
  `run_command` / `get_info` tool calls show live as collapsible blocks.
- **Audio perception** — the assistant can measure audio via `GetAudioStats`,
  `GetLoudness`, `GetSpectrum`, `DetectSilence`, `DetectOnsets` and reason from
  the numbers.
- **Live status** — header chips show Codex login (email/plan) and whether
  Otis's MCP is connected.

---

## Architecture

```
Browser (index.html)
   │  POST /api/chat {message, threadId?}   ── Server-Sent Events ──▶ deltas / tool calls / done
   ▼
server.py  (ThreadingHTTPServer on :8765)
   │  codex_bridge.py  ── JSON-RPC 2.0 over stdio ──▶
   ▼
codex app-server   (subprocess; isolated CODEX_HOME)
   │  • ChatGPT login (account/login/start, account/read)
   │  • threads = memory  (thread/start, thread/list, thread/resume, thread/read)
   │  • turns  (turn/start) stream item/agentMessage/delta, item/completed, …
   │  MCP (streamable HTTP)
   ▼
Audacity mod-mcp-server   (http://127.0.0.1:4830/mcp → run_command / get_info)
```

HTTP API: `GET /api/status`, `GET /api/threads`, `GET /api/thread?id=…`,
`POST /api/chat` (SSE), `POST /api/login`, `GET /api/login/wait`,
`POST /api/logout`, `POST /api/interrupt`.

---

## Troubleshooting

**Otis chip is red / not connected**
- Make sure Otis (with `mod-mcp-server`) is running.
- Verify the port: `curl -s -X POST http://127.0.0.1:4830/mcp -d '{"jsonrpc":"2.0","id":1,"method":"ping"}'`
- Different port? `export MCP_URL=http://127.0.0.1:<port>/mcp`

**"Log in with ChatGPT" keeps showing**
- Finish the browser OAuth flow; the chat unlocks on `account/login/completed`.
- Or pre-login once with `codex login` and restart (the companion inherits it).

**A turn reports a tool as "rejected" / empty results**
- Almost always the sandbox mode. Keep `CODEX_SANDBOX=danger-full-access`
  (the default) — restricted sandboxes block the localhost MCP call.

**`codex` not found**
- Install the Codex CLI and ensure it's on `PATH`, or set `CODEX_BIN`.

**Port 8765 already in use**
- `export COMPANION_PORT=9000 && ./run.sh`

---

## Files

| File | Purpose |
|---|---|
| `server.py` | Web server (SSE) bridging the browser to the Codex app server |
| `codex_bridge.py` | JSON-RPC client for `codex app-server` (requests, streaming, login, approvals) |
| `index.html` | Self-contained chat UI (login, thread sidebar, streaming) |
| `run.sh` | Convenience launcher (checks deps, opens browser) |
| `legacy/` | The previous API-key companion, kept for reference |
| `README.md` | This file |
