# mod-mcp-server

**mod-mcp-server** is a Phase-0 Audacity 3.x loadable module that exposes Audacity's scripting interface over HTTP using the [Model Context Protocol](https://modelcontextprotocol.io/) (MCP) / JSON-RPC 2.0.  It binds strictly to `127.0.0.1:4830` and serves a single endpoint `POST /mcp`; command execution is delegated to Audacity's existing `ScriptCommandRelay` (the same mechanism used by `mod-script-pipe`), so all commands are safely marshalled to the wx main thread via `AppCommandEvent` + `wxSemaphore` — no project state is accessed directly from the HTTP worker thread.

**Phase-0 tools exposed:** `run_command` (proxies any Audacity command string) and `get_info` (wraps the built-in `GetInfo` command).

**Vendored dependencies:** `httplib.h` (cpp-httplib) and `json.hpp` (nlohmann/json) are already vendored under `lib/cpp-httplib/` and `lib/nlohmann-json/` inside this module directory, and `CMakeLists.txt` adds both to the include path and builds them in — no manual setup is required. Ensure `mod-mcp-server` is listed in the `MODULES` list in `modules/scripting/CMakeLists.txt`.
