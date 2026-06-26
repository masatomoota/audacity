#!/usr/bin/env python3
"""
transcribe_mcp_server.py — a tiny MCP (stdio) server exposing Whisper speech-to-
text to the Codex agent.

The Codex companion registers this as an MCP server, so the agent gets a
`transcribe_audio` tool. Typical flow from Audacity:

    Audacity Export2 (project/selection -> /tmp/clip.wav)
      -> transcribe_audio(input_path="/tmp/clip.wav")        # this server
      -> agent reports the transcript (optionally builds a label track)

Transcription runs LOCALLY via mlx-whisper (Apple-Silicon GPU, no API key, audio
never leaves the machine) from the isolated audio venv
(~/.audacity-mcp-companion/sep-venv). If OPENAI_API_KEY is set and
TRANSCRIBE_ENGINE=openai, it uses the OpenAI transcription API instead
(gpt-4o-transcribe / whisper-1). Stdlib only for this process; the heavy work is
the mlx_whisper subprocess. Mirrors stem_mcp_server.py.
"""

import json
import os
import subprocess
import sys
import urllib.request
import uuid
from pathlib import Path

SUPPORT = Path(os.environ.get(
    "AUDACITY_COMPANION_HOME", str(Path.home() / ".audacity-mcp-companion")))
VENV = Path(os.environ.get("SEP_VENV", str(SUPPORT / "sep-venv")))
MLX_WHISPER = VENV / "bin" / "mlx_whisper"
OUT_DIR = SUPPORT / "transcripts"
# Apple-Silicon GPU, fast + accurate. Override per call or via WHISPER_MODEL.
DEFAULT_MODEL = os.environ.get("WHISPER_MODEL", "mlx-community/whisper-large-v3-turbo")

PROTOCOL_VERSION = "2025-03-26"

RECOMMENDED = [
    {"model": "mlx-community/whisper-large-v3-turbo",
     "note": "既定。高精度かつ高速（Apple GPU）。多言語・約1.5GB。"},
    {"model": "mlx-community/whisper-large-v3",
     "note": "最高精度・やや遅い。"},
    {"model": "mlx-community/whisper-small",
     "note": "軽量・高速・精度はそこそこ（約500MB）。"},
    {"model": "mlx-community/whisper-base",
     "note": "最軽量。短い明瞭な音声向け。"},
]

TOOLS = [
    {
        "name": "transcribe_audio",
        "description": (
            "Transcribe an audio file to text with Whisper (local mlx-whisper; "
            "no API key, audio stays on the machine). From Audacity: export the "
            "project or selection to a WAV (run_command Export2), then call this "
            "with that path. Returns the full text plus timestamped segments "
            "({start,end,text}); the agent can report the text or build an "
            "Audacity label track from the segments. Auto-detects language unless "
            "'language' is given (e.g. 'ja','en')."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "input_path": {"type": "string", "description":
                    "Absolute path to the audio file (e.g. a WAV exported from "
                    "Audacity via Export2)."},
                "language": {"type": "string", "description":
                    "Optional ISO language code (ja, en, ...). Omit to auto-detect."},
                "model": {"type": "string", "description":
                    "Optional model. Default mlx-community/whisper-large-v3-turbo. "
                    "See list_transcribe_models."},
            },
            "required": ["input_path"],
        },
    },
    {
        "name": "list_transcribe_models",
        "description": "List recommended Whisper models for transcription.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def _text(s, is_error=False):
    return {"content": [{"type": "text", "text": s}], "isError": is_error}


def _segments_summary(segments, limit=40):
    out = []
    for seg in segments[:limit]:
        st = seg.get("start", 0.0)
        en = seg.get("end", 0.0)
        out.append(f"[{st:7.2f}-{en:7.2f}] {seg.get('text','').strip()}")
    if len(segments) > limit:
        out.append(f"... (+{len(segments) - limit} more segments)")
    return "\n".join(out)


def _transcribe_openai(inp, language, model):
    """Optional cloud path: OpenAI audio.transcriptions (multipart upload)."""
    key = os.environ.get("OPENAI_API_KEY", "")
    if not key:
        return _text("TRANSCRIBE_ENGINE=openai but OPENAI_API_KEY is not set.", True)
    base = os.environ.get("OPENAI_BASE_URL", "https://api.openai.com").rstrip("/")
    omodel = model if model and "/" not in model else \
        os.environ.get("OPENAI_TRANSCRIBE_MODEL", "gpt-4o-transcribe")
    boundary = "----audacity" + uuid.uuid4().hex
    with open(inp, "rb") as f:
        audio = f.read()
    parts = []

    def field(name, value):
        parts.append(f"--{boundary}\r\nContent-Disposition: form-data; "
                     f"name=\"{name}\"\r\n\r\n{value}\r\n".encode())
    field("model", omodel)
    field("response_format", "json")
    if language:
        field("language", language)
    fn = os.path.basename(inp)
    parts.append((f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; "
                  f"filename=\"{fn}\"\r\nContent-Type: application/octet-stream\r\n\r\n"
                  ).encode() + audio + b"\r\n")
    parts.append(f"--{boundary}--\r\n".encode())
    body = b"".join(parts)
    req = urllib.request.Request(
        f"{base}/v1/audio/transcriptions", data=body,
        headers={"Authorization": f"Bearer {key}",
                 "Content-Type": f"multipart/form-data; boundary={boundary}"},
        method="POST")
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return _text(f"OpenAI transcription failed: {e}", True)
    txt = data.get("text", "")
    out = _text(f"文字起こし（OpenAI {omodel}）:\n\n{txt}")
    out["structuredContent"] = {"engine": "openai", "model": omodel, "text": txt}
    return out


def _validate_input_audio(path):
    # Same defensive check as stem_mcp_server (Export2 + empty selection
    # produces a header-only 410-byte AIFF). Accepts WAV (RIFF) and AIFF.
    try:
        size = os.path.getsize(path)
    except OSError as e:
        return f"could not stat input file {path}: {e}"
    if size < 4096:
        return (
            f"input file {path} is only {size} bytes — Otis exported a header "
            f"with no audio. Before Export2, run 'SelectAll:' so the whole "
            f"project is exported.")
    try:
        with open(path, "rb") as f:
            head = f.read(12)
    except OSError as e:
        return f"could not read input file {path}: {e}"
    if not ((head[0:4] == b"RIFF" and head[8:12] == b"WAVE")
            or (head[0:4] == b"FORM" and head[8:12] in (b"AIFF", b"AIFC"))):
        return (
            f"input file {path} is not a recognized WAV/AIFF container "
            f"(magic={head[:4]!r}). Re-export with Export2 after 'SelectAll:'.")
    return None


def do_transcribe(args):
    inp = args.get("input_path", "")
    if not inp or not os.path.isfile(inp):
        return _text(f"input_path not found: {inp}", True)
    err = _validate_input_audio(inp)
    if err:
        return _text(err, True)
    language = (args.get("language") or "").strip()
    model = args.get("model") or DEFAULT_MODEL

    if os.environ.get("TRANSCRIBE_ENGINE", "").lower() == "openai":
        return _transcribe_openai(inp, language, model)

    if not MLX_WHISPER.exists():
        return _text(
            f"mlx-whisper is not installed at {MLX_WHISPER}. Run "
            f"「ステム分離セットアップ.command」or:\n  {VENV}/bin/pip install mlx-whisper",
            True)
    os.makedirs(OUT_DIR, exist_ok=True)
    base = Path(inp).stem + "_" + uuid.uuid4().hex[:8]
    cmd = [str(MLX_WHISPER), inp,
           "--model", model,
           "--output-dir", str(OUT_DIR),
           "--output-name", base,
           "--output-format", "json",
           "--task", "transcribe"]
    if language:
        cmd += ["--language", language]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
    except subprocess.TimeoutExpired:
        return _text("transcription timed out (over 60 min).", True)
    except Exception as e:
        return _text(f"transcription failed to start: {e}", True)

    jpath = OUT_DIR / (base + ".json")
    if not jpath.is_file():
        tail = (p.stderr or p.stdout or "")[-1000:]
        return _text(f"mlx_whisper produced no output (exit {p.returncode}).\n{tail}",
                     True)
    try:
        data = json.loads(jpath.read_text())
    except Exception as e:
        return _text(f"could not parse transcript json: {e}", True)
    text = (data.get("text") or "").strip()
    segs = data.get("segments") or []
    lang = data.get("language") or language or "auto"
    body = (f"文字起こし完了（model {model} / language {lang} / {len(segs)} segments）。\n\n"
            f"=== 全文 ===\n{text}\n\n=== セグメント（start-end 秒） ===\n"
            f"{_segments_summary(segs)}")
    out = _text(body)
    out["structuredContent"] = {
        "engine": "mlx-whisper", "model": model, "language": lang,
        "text": text,
        "segments": [{"start": s.get("start"), "end": s.get("end"),
                      "text": (s.get("text") or "").strip()} for s in segs],
        "json_path": str(jpath),
    }
    return out


def do_list_models(_args):
    txt = "Whisper モデル候補:\n" + "\n".join(
        f"- {m['model']}  ({m['note']})" for m in RECOMMENDED)
    return _text(txt)


def handle(method, params):
    if method == "initialize":
        return {"protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "transcribe", "version": "0.1.0"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        name = params.get("name")
        a = params.get("arguments") or {}
        if name == "transcribe_audio":
            return do_transcribe(a)
        if name == "list_transcribe_models":
            return do_list_models(a)
        return _text(f"unknown tool: {name}", True)
    return None


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except Exception:
            continue
        rid = req.get("id")
        method = req.get("method")
        if rid is None:
            continue
        try:
            result = handle(method, req.get("params") or {})
            if result is None:
                resp = {"jsonrpc": "2.0", "id": rid,
                        "error": {"code": -32601,
                                  "message": f"method not found: {method}"}}
            else:
                resp = {"jsonrpc": "2.0", "id": rid, "result": result}
        except Exception as e:
            resp = {"jsonrpc": "2.0", "id": rid,
                    "error": {"code": -32603, "message": str(e)}}
        sys.stdout.write(json.dumps(resp) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
