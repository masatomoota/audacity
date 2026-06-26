# Handoff — Rebrand to "Otis" + new app icon

**Status:** ✅ **APPLIED 2026-06-26** (macOS legacy app + companion). The app presents as **Otis** everywhere users look. This document remains the implementation brief; see "Implementation status" below for what was actually done, the gaps found in this brief, and what was deferred.
**Audience:** the engineer/LLM who will apply the rename and wire in the new icon.
**Scope:** product-name rename (Audacity → Otis) and app-icon replacement only. The UI accent-color change is a separate brief: see `OTIS_UI_COLOR_HANDOFF.md`.

---

## Implementation status (2026-06-26)

**Approach — display-name, not bundle-dir rename.** The literal `Otis.app` bundle rename (changing `AUDACITY_NAME`) was attempted and **reverted**: the macOS bundle name has *multiple* hardcoded sources that diverge (`AUDACITY_NAME`→exe, `mac/Wrapper.c` hardcodes the exe name, `CMakeLists.txt:454 _APPDIR`, the build-time codesign path, **and the wx/conan framework deploy is keyed to the CMake `TARGET` name "Audacity"**). Unifying on `Otis.app` requires renaming the `TARGET` (ripples through hundreds of `${TARGET}` refs) — high risk, no user-visible gain. Instead:
- The bundle stays `Audacity.app` on disk (Group B identity preserved), but **`CFBundleName` + `CFBundleDisplayName` = "Otis"** and `CFBundleIconFile = Otis.icns` → Finder, Dock, menu bar, and icon all show **Otis**.
- `scripts/make-dmg.sh` stages/distributes it **as `Otis.app`** (dir rename at package time; `CFBundleExecutable=Wrapper` works regardless of dir name).
- `src/AudacityApp.cpp` `SetAppDisplayName("Otis")` (kept `SetAppName`/`SetVendorName` = `AppName` so the config/data dir is preserved).

**Applied (verified at runtime — menu/title/About/recovery all read "Otis", MCP works):**
- Bundle plist: `CFBundleName`/`CFBundleDisplayName`/`CFBundleIconFile`/version/mic strings → Otis; `Otis.icns` copied to `mac/Resources/` and referenced in `src/CMakeLists.txt`.
- Legacy in-app strings → Otis: splash, About menu + `AboutDialog` ProgramName, **window title** (`lib-project-file-io/ProjectFileIO.cpp` `SetProjectTitle`), Preferences, Help/File menus, config/recovery/lang/timer/crash/mixer/toolbar/what's-new/plugin/MIR/benchmark dialogs.
- Companion (`mcp-companion/index.html`): title, all visible UI strings, scarlet `--accent`, Otis SVG favicon.
- AU4 visible strings, Windows `audacity.rc` + `.iss` display names, `mac/Install.txt`, READMEs.

**Gaps in THIS brief that were found & handled (Group A was incomplete):**
- `mac/Wrapper.c:32` hardcodes the exe name — a literal rename breaks launch without patching it.
- `CMakeLists.txt:454 _APPDIR` + build codesign path + the `TARGET`-keyed framework deploy — the real reasons a bundle rename splits into two bundles.
- The completeness sweep excluded `libraries/`, but `lib-project-file-io` (window title) and others there hold first-party user-facing strings.

**Deferred (need a product decision — NOT done):**
- Literal on-disk `Otis.app` bundle (needs the `TARGET` rename pass).
- ~38 `libraries/` engine strings: attribution ("The Audacity Team"), plugin descriptions ("Provides … to Audacity"), project-format-version messages, error dialogs ("Audacity could not write…") — many are arguably about the upstream engine; do a deliberate keep/change pass.
- In-app **update dialogs** (`src/update/*`) point at Audacity's update server — rename is misleading; better to disable for Otis.
- `src/update/Audacity40PromoDialog.cpp` — the "Audacity 4 cloud" upsell; likely remove for Otis.
- Windows installer `[Icons]`/`[Run]` exe-name coupling (`audacity.exe` vs the renamed exe) — needs a dedicated Windows pass (Windows not built/tested here).

---

## 1. Background / why "Otis"

This repo is an Audacity fork that can be **fully driven by an LLM** (the MCP server in `modules/scripting/mod-mcp-server` + the chat companion in `mcp-companion/`). The product is being given its own identity.

The name **Otis** was chosen after checking many candidates for collisions in the audio-software namespace (most short audio words — Cadence, Sonar, Reverb, Aurio, Sonant, Tactus, Quinn, etc. — were already taken). "Otis":

- reads as a warm, human first name → fits the "talk to it and it edits" assistant model;
- carries a soul/analog-music association (Otis Redding) that we lean into for the visual brand;
- has **no collision in creative audio-editing software** (the only same-name software, "Otis AI", is an ad-tech SaaS — different product category; see the trademark note below).

**Trademark note (not blocking, but do this before any public launch):** "Otis AI" (meetotis.com, ad-tech) occupies part of the AI-software lane, and Otis Worldwide (elevators) holds the mark in a different class (machinery). Recommendation: file/clear under **"Otis Audio"** with a goods/services description limited to audio-editing software (Nice classes 9 + 42), and run a professional clearance search first. The `.audio` TLD (we recommend `otis.audio`, which was available) reinforces the differentiation.

## 2. The mark

A scarlet **"O" ring with a 13-bar symmetric audio waveform inside it** — the ring is the O of "Otis", the waveform says "audio", and the symmetry reads like a level/VU meter at rest.

- Primary color: **scarlet `#D8382A`** (緋色). This single-color version is the approved final (a multi-color "sunset" bar variant was explored and rejected for the icon itself — but that gradient idea was adopted for the *UI*, see the color handoff).
- Icon tile background: **ink `#1A1714`**.
- Wordmark "Otis": humanist sans (Inter / Helvetica Neue / Arial fallback stack), weight 500 — the same family as the earlier "tis" lettering.

All assets are regenerated reproducibly from `assets/otis/generate_assets.py` + `assets/otis/build_assets.sh` (requires `rsvg-convert` + ImageMagick). Edit the script, not the SVGs, if the geometry/color ever needs to change.

## 3. Delivered assets (`assets/otis/`)

| File | Purpose |
|---|---|
| `favicon.svg` / `otis-mark.svg` | **Primary** icon — transparent, scarlet mark, scales to any size |
| `favicon.ico` | **Fallback** for legacy browsers — multi-res 16/32/48/64, opaque ink tile |
| `apple-touch-icon.png` | 180×180, opaque ink tile (iOS adds its own corner mask) |
| `icon-192.png`, `icon-512.png` | PWA / Android, `purpose: any maskable` |
| `icon-1024.png` | master raster (rounded ink tile) |
| `Otis.icns` | macOS app icon, built from the rounded master via `iconutil` |
| `otis-lockup-light.svg` / `-dark.svg` | symbol + "Otis" wordmark (for README, About screen, splash) |
| `otis-wordmark-light.svg` / `-dark.svg` | "Otis" wordmark only |
| `site.webmanifest` | PWA manifest (theme `#D8382A`, bg `#1A1714`) |

> The raster icons are cut from the **opaque ink tile** so they stay visible on any tab/home-screen background. `favicon.svg` is transparent so modern browsers adapt it. This light(SVG)/dark(raster) split is intentional and standard.

## 4. Code changes — rename map

Line numbers are approximate (from a snapshot scan); **grep to confirm** before editing. Group A = safe to rename. Group B = DO NOT rename (compatibility / data persistence).

### Group A — rename the user-facing name to "Otis"

C++ (legacy wx) app:
- `CMakeLists.txt:590` — `set( AUDACITY_NAME "Audacity" )` → display name (macOS/Windows)
- `CMakeLists.txt:592` — `set( AUDACITY_NAME "audacity" )` → Linux display name
- `src/CMakeLists.txt:5` — `set( TARGET Audacity )` (build-target/exe name — optional; renaming changes the binary name)
- `src/AudacityApp.cpp:1328` — splash "Audacity is starting up..."
- `src/AudacityApp.cpp:1639` — menu "&About Audacity..."
- `src/AboutDialog.cpp:75-77` — `ProgramName = Verbatim("Audacity")`

AU4 (Qt/QML) app:
- `au4/src/app/app.cpp:84,86` — `appName = "Audacity4Development" / "Audacity4"`
- `au4/src/appshell/view/mainwindowtitleprovider.cpp:99` — window title "Audacity 4"
- `au4/src/project/internal/projectuiactions.cpp:744-745` — "About Audacity" action
- `au4/src/appshell/qml/AboutDialog.qml:32` — **bug: currently says "About MuseScore"** (leftover from upstream) → set to "About Otis"

macOS bundle:
- `cmake-proxies/cmake-modules/MacOSXBundleInfo.plist.in` — `CFBundleName` / version strings (~lines 210,220,236) → "Otis"
- same file `CFBundleIconFile` (~line 212): `Audacity.icns` → `Otis.icns`

Windows:
- `win/audacity.rc:29,35` — `FileDescription` / `ProductName` → "Otis"
- `win/Inno_Setup_Wizard/audacity.iss.in:39,49` — installer `AppVerName` / `DefaultDirName`

Web companion:
- `mcp-companion/index.html:6` — `<title>Audacity コンパニオン · Codex</title>` → "Otis …"
- `mcp-companion/server.py` — docstrings / DEV_INSTRUCTIONS mentioning Audacity (cosmetic)
- `mcp-companion/legacy/index.html` — title/header (only if the legacy UI is still used)

Docs:
- `README.md` (root) — replace with the new draft (`README.draft.md`, see §6)
- `mcp-companion/README.md` — title/branding

Helpful sweep to find anything missed (review each hit; don't blind-replace):
```bash
grep -rniI --exclude-dir=lib-src --exclude-dir=libraries --exclude-dir=.git \
  -e 'Audacity' src au4 mcp-companion win mac cmake-proxies CMakeLists.txt
```

### Group B — DO NOT rename (will break things)

- **`CFBundleIdentifier = org.audacityteam.audacity`** (plist ~line 214) — changing it makes macOS treat it as a new app: lost preferences, re-sign, re-notarize. Keep unless doing a deliberate clean-slate migration.
- **URL scheme `audacity:`** (plist ~line 245-248) — file/protocol associations depend on it. Keep, or add `otis:` *alongside* and migrate later.
- **Project/file-format identifiers** — `.aup3`/`.aup`, MIME `application/x-audacity-project`. Safe to ADD new types; do not repurpose existing ones.
- **Preferences/data dirs** — `~/.config/audacity/`, `~/.audacity-data/`, Windows registry `Software\\Audacity`. Renaming orphans existing user data. If you change it, write a migration that copies the old dir first.
- **Upstream library / C++ class names** — `libaudacity*`, `Audacity*` classes, module identifiers. These are API/ABI and persistence surfaces, not branding. Leave them.

## 5. Wiring in the new icon

**macOS app:**
1. Copy `assets/otis/Otis.icns` → `mac/Resources/Otis.icns`.
2. Update `CFBundleIconFile` in `MacOSXBundleInfo.plist.in` to `Otis.icns`.
3. Update the icns path in `src/CMakeLists.txt` (~lines 1111-1117, currently `../mac/Resources/Audacity.icns`).
4. The 7 document-type icons (`AudacityProject.icns`, `AudacityWAV.icns`, …) can be rebranded later; not required for the app icon.

**Windows app:** replace `win/audacity.ico` with `assets/otis/favicon.ico` (or a higher-res .ico — regenerate with larger sizes if needed) and confirm the reference in `win/audacity.rc`.

**Web companion (`mcp-companion/`):**
1. Copy `favicon.svg`, `favicon.ico`, `apple-touch-icon.png`, `icon-192.png`, `icon-512.png`, `site.webmanifest` into the directory `index.html` is served from.
2. Add to `<head>` of `mcp-companion/index.html`:
```html
<link rel="icon" href="favicon.svg" type="image/svg+xml">
<link rel="icon" href="favicon.ico" sizes="any">
<link rel="apple-touch-icon" href="apple-touch-icon.png">
<link rel="manifest" href="site.webmanifest">
```
   (Confirm `server.py` serves static files from that dir; if it only serves `index.html`, add a static route or inline the SVG.)

**Regenerating assets** (if geometry/color changes): `bash assets/otis/build_assets.sh`.

## 6. README

A logo-bearing draft is at `README.draft.md`. Review, then replace the root `README.md` with it (it intentionally drops the upstream audacityteam.org branding). Keep the GPL license section accurate to this fork's licensing.

## 7. Suggested order

1. Branding strings (Group A) — start with the two `CMakeLists.txt` name vars + AU4 `app.cpp`, build, confirm title/About/splash read "Otis".
2. Icon files + references (§5), rebuild, confirm Dock/taskbar/tab icon.
3. Web companion title + favicons.
4. README swap.
5. (separate) UI accent color — `OTIS_UI_COLOR_HANDOFF.md`.

Defer all Group B items unless a deliberate identity migration is planned.
