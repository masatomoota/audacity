#!/usr/bin/env python3
"""
stem_mcp_server.py — a tiny MCP (stdio) server exposing UVR-style stem
separation to the Codex agent, powered by `audio-separator`.

The Codex companion registers this as an MCP server, so the agent gets a
`separate_stems` tool.  Typical flow the agent runs:

    Audacity Export2 (project -> /tmp/song.wav)
      -> separate_stems(input_path="/tmp/song.wav")        # this server
      -> Audacity Import2 (each returned stem -> new track)

Separation runs the `audio-separator` CLI from a dedicated venv
(~/.audacity-mcp-companion/sep-venv), so the heavy ML deps stay isolated and the
work happens off Audacity's UI thread (no freeze, no rebuild).  Stdlib only.
"""

import glob
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

SUPPORT = Path(os.environ.get(
    "AUDACITY_COMPANION_HOME", str(Path.home() / ".audacity-mcp-companion")))
VENV = Path(os.environ.get("SEP_VENV", str(SUPPORT / "sep-venv")))
AUDIO_SEP = VENV / "bin" / "audio-separator"
MODELS_DIR = SUPPORT / "models"
OUT_DIR = SUPPORT / "stems"
# MDX-Net (ONNX) Vocals/Instrumental — high quality, no torch needed. Override
# per call or via SEP_DEFAULT_MODEL.
DEFAULT_MODEL = os.environ.get("SEP_DEFAULT_MODEL", "UVR-MDX-NET-Inst_HQ_3.onnx")

PROTOCOL_VERSION = "2025-03-26"

RECOMMENDED = [
    {"model": "UVR-MDX-NET-Inst_HQ_3.onnx", "stems": "Vocals + Instrumental",
     "note": "MDX-Net, high quality, fast (ONNX). Default."},
    {"model": "UVR_MDXNET_KARA_2.onnx", "stems": "Lead + Backing vocals",
     "note": "Karaoke / backing-vocal split."},
    {"model": "model_bs_roformer_ep_317_sdr_12.9755.ckpt",
     "stems": "Vocals + Instrumental", "note": "BS-Roformer, SOTA, slower."},
    {"model": "htdemucs.yaml", "stems": "Vocals/Drums/Bass/Other",
     "note": "Demucs 4-stem (uses torch)."},
    {"model": "htdemucs_6s.yaml",
     "stems": "Vocals/Drums/Bass/Guitar/Piano/Other", "note": "Demucs 6-stem."},
]

TOOLS = [
    {
        "name": "separate_stems",
        "description": (
            "Separate an audio file into stems with UVR/audio-separator and "
            "return the produced stem file paths. From Audacity: export the "
            "project to a WAV (run_command Export2), call this with that path, "
            "then Import2 each returned stem as a new track. Default model "
            "splits Vocals/Instrumental; pass model='htdemucs.yaml' for 4 stems "
            "(vocals/drums/bass/other). Separation can take a few minutes."),
        "inputSchema": {
            "type": "object",
            "properties": {
                "input_path": {"type": "string", "description":
                    "Absolute path to the input audio file (e.g. a WAV exported "
                    "from Audacity via Export2)."},
                "model": {"type": "string", "description":
                    "Model filename. Default UVR-MDX-NET-Inst_HQ_3.onnx. Call "
                    "list_stem_models for options."},
                "output_dir": {"type": "string", "description":
                    "Optional output directory (defaults to "
                    "~/.audacity-mcp-companion/stems)."},
            },
            "required": ["input_path"],
        },
    },
    {
        "name": "list_stem_models",
        "description": "List recommended stem-separation models and the stems "
                       "each produces.",
        "inputSchema": {"type": "object", "properties": {}},
    },
]


def _text(s, is_error=False):
    return {"content": [{"type": "text", "text": s}], "isError": is_error}


def _validate_input_audio(path):
    """Defensive check: Audacity's Export2 returns OK even when the time
    selection is empty (Start==End), producing a header-only file. Catch that
    here and tell the agent exactly what to do, instead of letting
    audio-separator fail with a cryptic error a few seconds later. Accept both
    WAV (RIFF) and AIFF (FORM...AIFF) since Audacity's Export2 actually writes
    AIFF regardless of the .wav extension."""
    try:
        size = os.path.getsize(path)
    except OSError as e:
        return f"could not stat input file {path}: {e}"
    if size < 4096:
        return (
            f"input file {path} is only {size} bytes — Otis exported a header "
            f"with no audio. This usually means the time selection was empty. "
            f"Before Export2, run 'SelectAll:' so the whole project is exported.")
    try:
        with open(path, "rb") as f:
            head = f.read(12)
    except OSError as e:
        return f"could not read input file {path}: {e}"
    is_wav  = head[0:4] == b"RIFF" and head[8:12] == b"WAVE"
    is_aiff = head[0:4] == b"FORM" and head[8:12] in (b"AIFF", b"AIFC")
    if not (is_wav or is_aiff):
        return (
            f"input file {path} is not a recognized WAV/AIFF audio container "
            f"(magic={head[:4]!r}). Re-export from Otis with Export2 after "
            f"'SelectAll:'.")
    return None


def _run_with_pgroup_kill(cmd, timeout):
    """Like subprocess.run(..., capture_output=True, text=True, timeout=timeout)
    but on timeout, kills the whole process group (audio-separator/torch can
    spawn worker children that survive a plain process.kill()) before
    collecting output. Mirrors subprocess.run's return shape (a CompletedProcess)
    and re-raises TimeoutExpired like subprocess.run does."""
    p = subprocess.Popen(
        cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
        start_new_session=True)
    try:
        out, err = p.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(os.getpgid(p.pid), signal.SIGKILL)
        except Exception:
            pass
        out, err = p.communicate()
        raise subprocess.TimeoutExpired(cmd, timeout, output=out, stderr=err)
    return subprocess.CompletedProcess(cmd, p.returncode, out, err)


def do_separate(args):
    inp = args.get("input_path", "")
    if not inp or not os.path.isfile(inp):
        return _text(f"input_path not found: {inp}", True)
    err = _validate_input_audio(inp)
    if err:
        return _text(err, True)
    if not AUDIO_SEP.exists():
        return _text(
            f"audio-separator is not installed at {AUDIO_SEP}. Create the venv "
            f"with:\n  python3.11 -m venv {VENV}\n  {VENV}/bin/pip install "
            f"'audio-separator[cpu]'", True)
    model = args.get("model") or DEFAULT_MODEL
    outdir = args.get("output_dir") or str(OUT_DIR)
    os.makedirs(outdir, exist_ok=True)
    os.makedirs(MODELS_DIR, exist_ok=True)
    start = time.time()
    cmd = [str(AUDIO_SEP), inp,
           "--model_filename", model,
           "--output_dir", outdir,
           "--model_file_dir", str(MODELS_DIR),
           "--output_format", "WAV"]
    try:
        p = _run_with_pgroup_kill(cmd, timeout=3600)
    except subprocess.TimeoutExpired:
        return _text("separation timed out (over 60 min).", True)
    except Exception as e:
        return _text(f"separation failed to start: {e}", True)

    stem = Path(inp).stem
    produced = sorted(
        f for f in glob.glob(os.path.join(outdir, "*.wav"))
        if os.path.getmtime(f) >= start - 1 and stem in os.path.basename(f))
    if not produced:
        produced = sorted(
            f for f in glob.glob(os.path.join(outdir, "*.wav"))
            if os.path.getmtime(f) >= start - 1)
    if not produced:
        tail = (p.stderr or p.stdout or "")[-1000:]
        return _text(f"audio-separator produced no output (exit {p.returncode})."
                     f"\n{tail}", True)
    lines = [f"Separated '{os.path.basename(inp)}' with model '{model}'. "
             f"{len(produced)} stem(s) — Import2 each into Audacity:"]
    lines += produced
    out = _text("\n".join(lines))
    out["structuredContent"] = {"model": model, "stems": produced}
    return out


def do_list_models(_args):
    txt = "Recommended stem-separation models:\n" + "\n".join(
        f"- {m['model']}  →  {m['stems']}  ({m['note']})" for m in RECOMMENDED)
    return _text(txt)


def handle(method, params):
    if method == "initialize":
        return {"protocolVersion": PROTOCOL_VERSION,
                "capabilities": {"tools": {"listChanged": False}},
                "serverInfo": {"name": "stem-separator", "version": "0.1.0"}}
    if method == "ping":
        return {}
    if method == "tools/list":
        return {"tools": TOOLS}
    if method == "tools/call":
        name = params.get("name")
        a = params.get("arguments") or {}
        if name == "separate_stems":
            return do_separate(a)
        if name == "list_stem_models":
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
        if rid is None:           # notification (e.g. notifications/initialized)
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
