#!/usr/bin/env python3
"""Generate the Otis brand assets (waveform-O mark, wordmark, lockups, web icons).

The mark = an "O" ring with a 13-bar symmetric audio waveform inside it,
in Otis scarlet (#D8382A). This is the source of truth; raster icons (PNG/ICO)
are rasterised from these SVGs by build_assets.sh.

Run:  python3 generate_assets.py
"""
import os

OUT = os.path.dirname(os.path.abspath(__file__))

SCARLET = "#D8382A"   # primary brand / mark
INK     = "#1A1714"   # near-black tile background
CREAM   = "#F5EFE6"   # light foreground on dark

# Bar height as a fraction of the peak, by distance from the centre bar (0..6).
RATIOS = [1.0, 0.904, 0.784, 0.648, 0.512, 0.392, 0.28]
NBARS = 13
MID = (NBARS - 1) // 2  # 6


def mark_elements(S, color, cx=None, cy=None):
    """Return the SVG fragment for the waveform-O mark, sized to an S x S box."""
    cx = S / 2 if cx is None else cx
    cy = S / 2 if cy is None else cy
    ring_outer = 0.390 * S
    stroke = 0.0898 * S
    ring_center_r = ring_outer - stroke / 2
    inner_r = ring_outer - stroke
    bar_w = 0.0352 * S
    bar_gap = 0.00977 * S
    spacing = bar_w + bar_gap
    total_w = NBARS * bar_w + (NBARS - 1) * bar_gap
    start = cx - total_w / 2
    peak_h = 0.488 * S
    rx = bar_w * 0.5
    els = [f'<circle cx="{cx:.2f}" cy="{cy:.2f}" r="{ring_center_r:.2f}" '
           f'fill="none" stroke="{color}" stroke-width="{stroke:.2f}"/>']
    for i in range(NBARS):
        d = abs(i - MID)
        h = peak_h * RATIOS[d]
        x = start + i * spacing
        y = cy - h / 2
        els.append(f'<rect x="{x:.2f}" y="{y:.2f}" width="{bar_w:.2f}" '
                   f'height="{h:.2f}" rx="{rx:.2f}" fill="{color}"/>')
    return "\n  ".join(els)


def svg_mark(S, color, bg=None, rounded=False):
    head = (f'<svg width="{S}" height="{S}" viewBox="0 0 {S} {S}" '
            f'xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Otis">\n  ')
    body = ""
    if bg:
        if rounded:
            body += f'<rect width="{S}" height="{S}" rx="{0.225*S:.1f}" fill="{bg}"/>\n  '
        else:
            body += f'<rect width="{S}" height="{S}" fill="{bg}"/>\n  '
    body += mark_elements(S, color)
    return head + body + "\n</svg>\n"


WORDMARK_FONT = ('Inter, "Helvetica Neue", Helvetica, "Segoe UI", '
                 'Roboto, Arial, sans-serif')


def svg_lockup(text_color, mark_color=SCARLET):
    """Symbol + 'Otis' wordmark, horizontal lockup."""
    W, H = 900, 340
    msize = 300
    mx, my = 16, 20
    cx = mx + msize / 2
    cy = my + msize / 2
    mark = mark_elements(msize, mark_color, cx=cx, cy=cy)
    text_x = mx + msize + 36
    baseline = cy + msize * 0.185
    fs = msize * 0.60
    return (
        f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
        f'xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Otis">\n  '
        f'{mark}\n  '
        f'<text x="{text_x:.1f}" y="{baseline:.1f}" '
        f'font-family=\'{WORDMARK_FONT}\' font-size="{fs:.1f}" '
        f'font-weight="500" letter-spacing="1" fill="{text_color}">Otis</text>\n'
        f'</svg>\n'
    )


def svg_wordmark(text_color):
    W, H = 560, 300
    fs = 200
    return (
        f'<svg width="{W}" height="{H}" viewBox="0 0 {W} {H}" '
        f'xmlns="http://www.w3.org/2000/svg" role="img" aria-label="Otis">\n  '
        f'<text x="20" y="225" font-family=\'{WORDMARK_FONT}\' font-size="{fs}" '
        f'font-weight="500" letter-spacing="1" fill="{text_color}">Otis</text>\n'
        f'</svg>\n'
    )


WEBMANIFEST = '''{
  "name": "Otis",
  "short_name": "Otis",
  "description": "LLM-controlled audio editor",
  "icons": [
    { "src": "icon-192.png", "sizes": "192x192", "type": "image/png", "purpose": "any maskable" },
    { "src": "icon-512.png", "sizes": "512x512", "type": "image/png", "purpose": "any maskable" }
  ],
  "theme_color": "#D8382A",
  "background_color": "#1A1714",
  "display": "standalone"
}
'''


def write(name, content):
    path = os.path.join(OUT, name)
    with open(path, "w") as f:
        f.write(content)
    print("wrote", name)


def main():
    write("otis-mark.svg",          svg_mark(512, SCARLET))
    write("favicon.svg",            svg_mark(512, SCARLET))
    write("otis-icon-tile.svg",     svg_mark(512, SCARLET, bg=INK))
    write("otis-icon-rounded.svg",  svg_mark(1024, SCARLET, bg=INK, rounded=True))
    write("otis-lockup-light.svg",  svg_lockup(INK))
    write("otis-lockup-dark.svg",   svg_lockup(CREAM))
    write("otis-wordmark-light.svg", svg_wordmark(INK))
    write("otis-wordmark-dark.svg",  svg_wordmark(CREAM))
    write("site.webmanifest",       WEBMANIFEST)


if __name__ == "__main__":
    main()
