#!/usr/bin/env python3
"""Otis mod-mcp-server smoke test (SESSION5). Prereq: Otis running with a fresh/empty project (open build/RelWithDebInfo/Audacity.app). Run: python3 scripts/mcp-smoke-test.py — expects 18/18 PASS."""
import json
import os
import shutil
import urllib.request

URL = "http://127.0.0.1:4830/mcp"
TESTDIR = "/tmp/otis smoke test"
PASS, FAIL = [], []


def rpc(method, params, rid=1, timeout=330):
    body = json.dumps({"jsonrpc": "2.0", "id": rid, "method": method,
                       "params": params}).encode()
    req = urllib.request.Request(URL, data=body,
                                 headers={"content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def run(cmd):
    return rpc("tools/call", {"name": "run_command",
                              "arguments": {"command": cmd}})


def tool_text(resp):
    if "result" not in resp:
        return json.dumps(resp.get("error", resp))
    return resp["result"]["content"][0]["text"]


def is_err(resp):
    if "result" not in resp:
        return True
    return resp["result"].get("isError", False)


def check(name, cond, detail=""):
    (PASS if cond else FAIL).append(name)
    print(("PASS" if cond else "FAIL"), name, ("— " + detail) if detail else "")


def track_count():
    r = run("GetInfo: Type=Tracks Format=JSON")
    txt = tool_text(r)
    # response text has JSON array followed by status line(s)
    start, end = txt.find("["), txt.rfind("]")
    return len(json.loads(txt[start:end + 1]))


shutil.rmtree(TESTDIR, ignore_errors=True)
os.makedirs(TESTDIR, exist_ok=True)

# 0. baseline
n0 = track_count()
print("baseline tracks:", n0)

# 1. create a track first (Tone shows a modal error without one), then tone
r = run("NewMonoTrack:")
check("new-mono-track", not is_err(r), tool_text(r)[:80])
r = run("Select: Start=0 End=10")
check("select-range2", not is_err(r), tool_text(r)[:80])
r = run("SelectTracks: Track=0 TrackCount=1")
check("select-track", not is_err(r), tool_text(r)[:80])
r = run("Tone: Frequency=440 Amplitude=0.8")
check("tone-generate", not is_err(r), tool_text(r)[:80])
n1 = track_count()
check("tone-created-track", n1 >= max(n0, 1), f"tracks {n0} -> {n1}")

# 2. Export2 with EMPTY selection to a quoted path WITH SPACES
r = run("SelectNone:")
check("select-none", not is_err(r), tool_text(r)[:80])
out1 = f"{TESTDIR}/out one.wav"
r = run(f'Export2: Filename="{out1}" NumChannels=1')
check("export-empty-selection-quoted-path", not is_err(r), tool_text(r)[:120])
if os.path.isfile(out1):
    with open(out1, "rb") as f:
        magic = f.read(12)
    size = os.path.getsize(out1)
    check("export-is-RIFF-WAVE-not-AIFF",
          magic[:4] == b"RIFF" and magic[8:12] == b"WAVE",
          f"magic={magic!r} size={size}")
    check("export-has-audio-data", size > 100000, f"size={size}")
else:
    check("export-file-exists", False, out1)

# 3. UNQUOTED spaced path must be a VISIBLE error (not silent truncation)
r = run(f"Export2: Filename={TESTDIR}/bad path.wav")
check("unquoted-space-visible-error", is_err(r), tool_text(r)[:120])

# 4. server must NOT be wedged after the error (no modal dialog)
r = run("GetInfo: Type=Tracks Format=JSON")
check("no-wedge-after-error", not is_err(r))

# 5. Import2 with double-quoted spaced path
r = run(f'Import2: Filename="{out1}"')
check("import-double-quoted", not is_err(r), tool_text(r)[:100])
n2 = track_count()
check("import-added-track", n2 == n1 + 1, f"tracks {n1} -> {n2}")

# 6. Import2 with SINGLE-quoted spaced path (UNIX split mode)
r = run(f"Import2: Filename='{out1}'")
check("import-single-quoted", not is_err(r), tool_text(r)[:100])
n3 = track_count()
check("import-single-quote-added-track", n3 == n2 + 1, f"tracks {n2} -> {n3}")

# 7. bad command name is a visible error
r = run("TotallyNonexistentCommand42:")
check("unknown-command-visible-error", is_err(r), tool_text(r)[:100])

# 8. non-object arguments -> clean JSON-RPC error, not bare 500
try:
    r = rpc("tools/call", {"name": "get_info", "arguments": "oops"})
    check("non-object-args-clean-error", "error" in r,
          json.dumps(r)[:120])
except Exception as e:
    check("non-object-args-clean-error", False, f"HTTP-level failure: {e}")

# 9. string id with quote is echoed as valid JSON
try:
    r = rpc("ping", {}, rid='we"ird')
    check("string-id-escaping", r.get("id") == 'we"ird', json.dumps(r)[:120])
except Exception as e:
    check("string-id-escaping", False, str(e))

print()
print(f"RESULT: {len(PASS)} passed, {len(FAIL)} failed")
if FAIL:
    print("FAILED:", FAIL)
