# Handoff — Otis UI accent color ("sunset sweep" / "center-hot meter")

**Status:** design decided, code NOT yet applied. Implementation brief.
**Audience:** the engineer/LLM applying the theme change.
**Related:** `OTIS_REBRAND_HANDOFF.md` (name + icon). This doc is colors only.

---

## 1. Intent / why this palette

Otis's brand idea is **warm, analog, "soul"** (see the rebrand handoff). During logo exploration we tried coloring the waveform bars and produced two warm ramps that everyone liked — but for the *icon* we deliberately kept a single flat scarlet so it stays legible at 16px. The **multi-color ramps were too good to throw away**, so the decision is to **adopt them in the UI instead** of the icon.

Two related ramps, used for different things:

- **Sunset sweep** — a left→right ramp from **gold → scarlet**. Use for *linear, directional* surfaces: primary buttons, progress bars, the playhead/timeline accent, a horizontal level meter, focus underlines.
- **Center-hot meter** — a *symmetric* ramp where the center is the hottest deep-scarlet and it cools to gold at the edges. Use for *symmetric* visualizations: the waveform/VU display, a centered EQ, anything that mirrors around a midline (it mimics "the peak is burning").

This **replaces the current blue accent** (`#4493f8`) in the chat companion. The blue is generic and off-brand; scarlet/gold is the Otis identity.

## 2. Color tokens (canonical hex)

### Core brand
| Token | Hex | Use |
|---|---|---|
| `--otis-scarlet` | `#D8382A` | primary accent (default), matches the icon |
| `--otis-scarlet-deep` | `#C9171E` | pressed/active, hottest meter center |
| `--otis-gold` | `#E8A33D` | secondary warm accent |
| `--otis-gold-light` | `#F0B24A` | ramp start / highlight |
| `--otis-ink` | `#1A1714` | dark surface / tile bg |
| `--otis-cream` | `#F5EFE6` | light foreground on dark |

Keep a non-brand **success green** for OK/confirm states (the companion's existing `--accent2: #2ea043` is fine) — don't make "success" scarlet, scarlet will read as error/record.

### Sunset sweep — 13-stop ramp (gold → scarlet), left→right
```
#F0B24A  #ECA640  #E89A38  #E48C30  #E07E2C  #DC6E29  #D85E27
#D44E25  #D03E22  #CC3220  #C8281D  #C5201B  #C21A19
```
CSS gradient (use the endpoints for a smooth fill; the 13 stops are for discrete bars):
```css
--otis-sweep: linear-gradient(90deg, #F0B24A 0%, #DC6E29 50%, #C21A19 100%);
```

### Center-hot meter — by distance from center (0 = center, 6 = edge)
```
d0 #CE2018   d1 #D83A28   d2 #DE4F28   d3 #E36A2A
d4 #E8842F   d5 #EC9B3C   d6 #F0B24A
```
CSS gradient for a symmetric bar/meter:
```css
--otis-meter: linear-gradient(90deg,
  #F0B24A 0%, #E8842F 22%, #DE4F28 38%,
  #CE2018 50%,
  #DE4F28 62%, #E8842F 78%, #F0B24A 100%);
```

> Implementation note: for **many small bars** (a real EQ/waveform), assign each bar a *solid* color from the stop list above rather than a CSS `linear-gradient` — discrete solid fills look crisper and avoid the gradient "flash" on re-render. Reserve the `linear-gradient(...)` form for single continuous surfaces (one button, one progress bar).

## 3. Where to apply

### A. Chat companion web UI — `mcp-companion/index.html` (do this first; highest impact, lowest risk)

Current state (`:10`):
```css
--accent:  #4493f8;   /* blue — REPLACE */
--accent2: #2ea043;   /* green success — KEEP */
```
`var(--accent)` is used throughout for buttons, borders, spinners, active/focus states (approx lines 25, 27, 52, 54, 67, 81, 88, 98, 102, 113, 128 — grep `var(--accent)` to get the live list).

Change to:
```css
--accent:        #D8382A;  /* Otis scarlet */
--accent-strong: #C9171E;  /* pressed / active */
--accent-gold:   #E8A33D;
--accent2:       #2ea043;  /* success — unchanged */
--otis-sweep: linear-gradient(90deg, #F0B24A 0%, #DC6E29 50%, #C21A19 100%);
--otis-meter: linear-gradient(90deg, #F0B24A 0%, #E8842F 22%, #DE4F28 38%, #CE2018 50%, #DE4F28 62%, #E8842F 78%, #F0B24A 100%);
```
Then:
- Primary action button / send button → background `var(--otis-sweep)` (or solid `var(--accent)` if you prefer flat); hover/active → `var(--accent-strong)`.
- Focus rings, active tabs, spinner, links → `var(--accent)`.
- Any audio level meter / waveform strip in the companion → `var(--otis-meter)` (symmetric) or per-bar solid stops.
- Double-check contrast: scarlet text on the dark `--otis-ink` passes; scarlet text on white is borderline for body copy — use it for accents/icons, not long text.

### B. AU4 (Qt/QML) editor accent — more involved

The AU4 accent is **not hardcoded in this repo**; it comes from `uiConfiguration()->possibleAccentColors()` (Muse framework, outside the repo). To set Otis scarlet as the default accent:

- `au4/src/appshell/view/firstlaunchsetup/themespagemodel.cpp:159` — sets `ThemeStyleKey::ACCENT_COLOR` via `setCurrentThemeStyleValue(...)`. Set/default this to `#D8382A`.
- `au4/src/appshell/view/preferences/appearancepreferencesmodel.cpp` — `accentColors()` mirror; add `#D8382A` to the offered palette so it's selectable.
- `au4/src/appshell/qml/shared/AccentColorsList.qml` / `Preferences/internal/AccentColorsSection.qml` — the swatch list UI; ensure the scarlet swatch appears.
- For the **waveform/clip rendering** colors specifically (the part that should use the meter ramp), find the track/clip painter in the AU4 view layer and the legacy `src/` wave drawing (`AColor`, wave clip view). This is the higher-effort piece — treat as a follow-up after the accent default lands.

### C. Legacy wx C++ theme — lowest priority

Colors flow through the theme system (`src/prefs/ThemePrefs.*`, `src/ThemedWrappers.h`, `AColor`). If the legacy UI is still shipped, introduce scarlet as the selection/cursor accent there; otherwise skip — the AU4 UI is the future.

## 4. Acceptance check

- Companion: no blue `#4493f8` left (`grep 4493f8 mcp-companion/`), primary actions are warm gold→scarlet, success stays green, focus/active states are scarlet.
- Editor: default accent swatch is Otis scarlet; the scarlet swatch is selectable in Preferences.
- A level meter / waveform somewhere renders the meter ramp (hot center) — even one surface proves the token plumbing.

## 5. Source of truth

These exact ramps come from the logo-exploration widgets and are reproduced in `assets/otis/generate_assets.py` comments / this doc. If the palette is ever retuned, update this file and the companion CSS together so the tokens don't drift.
