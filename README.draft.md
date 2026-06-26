<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/otis/otis-lockup-dark.svg">
    <img src="assets/otis/otis-lockup-light.svg" alt="Otis" width="360">
  </picture>
</p>

<p align="center">
  <b>The audio editor you talk to.</b><br>
  A full-featured, multi-track audio editor — driven end-to-end by an LLM.
</p>

<p align="center">
  <img alt="License" src="https://img.shields.io/badge/license-GPLv3-D8382A">
  <img alt="Platforms" src="https://img.shields.io/badge/macOS%20·%20Windows%20·%20Linux-1A1714">
  <img alt="Built on Audacity" src="https://img.shields.io/badge/built%20on-Audacity-E8A33D">
</p>

---

## What is Otis?

**Otis** is a professional multi-track audio editor that an LLM can **fully control** — record, cut, fade, mix, apply effects, export — from natural-language instructions. Tell it *"trim the silence at the start, fade out the last two seconds, and bounce it to MP3"* and it does the work in a real editor, not a black box.

It is built on the [Audacity](https://www.audacityteam.org) engine, with a Model-Context-Protocol (MCP) control surface and a chat companion bolted on so any MCP-capable assistant can operate the app.

## Features

- **Talk to edit** — an MCP server exposes the editor's commands so an LLM can perform real, sample-accurate edits.
- **Chat companion** — a lightweight UI (`mcp-companion/`) that pairs a conversation with the running editor; one launcher starts both together.
- **Everything Audacity does** — multi-track editing, 32-bit float processing, a deep effects set, and VST / LV2 / AU plugin support.
- **Record** from any real or virtual audio device on the system.
- **Import / export** a wide range of formats (extensible with FFmpeg).
- **Scriptable** via Nyquist and named-pipe scripting, in addition to the MCP layer.
- **Cross-platform** — macOS, Windows, and GNU/Linux.

## Getting started

### Users
Packaged builds (macOS `.dmg`) bundle the editor and the chat companion behind a single launcher. See the releases for the latest build.

### Developers
Otis builds the same way as Audacity — see [`BUILDING.md`](BUILDING.md). The LLM-control pieces live in:

- `modules/scripting/mod-mcp-server/` — the MCP server module inside the editor.
- `mcp-companion/` — the chat companion UI + bridge.

Implementation notes and design history are in the `MCP_*_HANDOFF.md` documents at the repo root.

## License

Otis is open-source software licensed **GPLv3**, inherited from Audacity. Most code files are GPLv2-or-later; the notable exception is `/lib-src` (third-party libraries). Documentation is CC-BY 3.0 unless otherwise noted. See [`LICENSE.txt`](LICENSE.txt).

## Acknowledgements

Otis stands on the shoulders of the [Audacity](https://www.audacityteam.org) project and its contributors. Audacity is a registered trademark of its owners; "Otis" is an independent fork and is not affiliated with or endorsed by the Audacity project.

---

<sub>Brand assets live in <a href="assets/otis/">assets/otis/</a> and regenerate from <code>generate_assets.py</code>. Draft README — review before replacing <code>README.md</code>.</sub>
