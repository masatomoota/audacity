#!/usr/bin/env python3
"""Otis mod-mcp-server smoke test (SESSION5/6). Prereq: Otis running with a fresh/empty project (open build/RelWithDebInfo/Audacity.app). Run: python3 scripts/mcp-smoke-test.py — expects 35/35 PASS when starting from zero tracks (the 4-check modal-suppression section is skipped if the project already has tracks, giving 31)."""
import json
import os
import shutil
import time
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


def call(tool_name, arguments):
    return rpc("tools/call", {"name": tool_name, "arguments": arguments})


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


def wait_project_ready(timeout=60):
    """The MCP server comes up a few seconds before the first project window;
    until then GetInfo returns an empty response. Poll until it succeeds."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return track_count()
        except Exception:
            time.sleep(2)
    raise SystemExit("project window never became ready — is Otis running?")


shutil.rmtree(TESTDIR, ignore_errors=True)
os.makedirs(TESTDIR, exist_ok=True)

# 0. baseline (waits for the project window on a fresh launch)
n0 = wait_project_ready()
print("baseline tracks:", n0)

# 0b. modal-suppression tests — only meaningful with ZERO tracks, so this
# section MUST run before any track is created.  Core-side modal-dialog
# suppression is being implemented by a parallel agent; these tests only
# assert on behavior, not on which agent's change makes them pass.
if n0 == 0:
    # The key assertion is that these fail cleanly (isError) and, crucially,
    # do NOT wedge Otis behind a modal dialog / infinite progress spinner —
    # verified by the follow-up GetInfo returning promptly. Message text is
    # localized (the running app may be Japanese), so we do not assert on
    # specific English wording; we only require the failure be reported.
    r = run("Tone: Frequency=440")
    check("tone-no-track-visible-error", is_err(r), tool_text(r)[:120])
    r = run("GetInfo: Type=Tracks Format=JSON")
    check("no-wedge-after-tone-no-track", not is_err(r), tool_text(r)[:80])

    r = run('Import2: Filename="/nonexistent_otis_test.wav"')
    check("import-missing-file-visible-error", is_err(r), tool_text(r)[:160])
    r = run("GetInfo: Type=Tracks Format=JSON")
    check("no-wedge-after-import-missing-file", not is_err(r), tool_text(r)[:80])
else:
    print("SKIP modal-suppression tests — project already has", n0, "track(s)")

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

# 10. tools/list returns the 10 dedicated + generic tools
r = rpc("tools/list", {})
tool_names = {t.get("name") for t in r.get("result", {}).get("tools", [])}
check("tools-list-has-10-tools", len(tool_names) == 10,
      f"count={len(tool_names)} names={sorted(tool_names)}")

# 11. generate_tone works even from zero tracks, and adds a track
n_before_tone = track_count()
r = call("generate_tone", {})
check("generate-tone-ok", not is_err(r), tool_text(r)[:120])
n_after_tone = track_count()
check("generate-tone-added-track", n_after_tone == n_before_tone + 1,
      f"tracks {n_before_tone} -> {n_after_tone}")

# 12. set_track renames + pans the last track; verify via list_tracks
last_track_idx = n_after_tone - 1
r = call("set_track", {"track": last_track_idx, "name": "Vocals", "pan": 50})
check("set-track-ok", not is_err(r), tool_text(r)[:120])
r = call("list_tracks", {})
check("set-track-name-visible", "Vocals" in tool_text(r), tool_text(r)[:200])

# 13. export_audio to a spaced path -> file exists with RIFF/WAVE magic
out2 = f"{TESTDIR}/tool export.wav"
r = call("export_audio", {"path": out2, "num_channels": 1})
check("export-audio-ok", not is_err(r), tool_text(r)[:120])
if os.path.isfile(out2):
    with open(out2, "rb") as f:
        magic2 = f.read(12)
    check("export-audio-is-RIFF-WAVE",
          magic2[:4] == b"RIFF" and magic2[8:12] == b"WAVE", f"magic={magic2!r}")
else:
    check("export-audio-file-exists", False, out2)

# 14. import_audio re-imports that file -> track count +1
n_before_import = track_count()
r = call("import_audio", {"path": out2})
check("import-audio-ok", not is_err(r), tool_text(r)[:120])
n_after_import = track_count()
check("import-audio-added-track", n_after_import == n_before_import + 1,
      f"tracks {n_before_import} -> {n_after_import}")

# 15. apply_effect on the whole selection
r = call("select_audio", {"mode": "all"})
check("select-audio-all-ok", not is_err(r), tool_text(r)[:120])
r = call("apply_effect", {"name": "Amplify", "params": {"Ratio": 0.5}})
check("apply-effect-ok", not is_err(r), tool_text(r)[:120])

# 16. remove_track removes the last track -> track count -1
n_before_remove = track_count()
r = call("remove_track", {"track": n_before_remove - 1})
check("remove-track-ok", not is_err(r), tool_text(r)[:120])
n_after_remove = track_count()
check("remove-track-removed-track", n_after_remove == n_before_remove - 1,
      f"tracks {n_before_remove} -> {n_after_remove}")

print()
print(f"RESULT: {len(PASS)} passed, {len(FAIL)} failed")
if FAIL:
    print("FAILED:", FAIL)
