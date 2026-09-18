#!/usr/bin/env python3
"""Generate Distill's mark, icon and logo as SVG.

The wordmark is outlined from the Source Serif 4 the reader itself ships, so
the logo carries no font dependency: an SVG that names a font renders as
whatever the viewer happens to have, which for a logo is not a rendering at
all. Re-run this after changing the shapes or the palette; the SVGs are
committed, so nothing at build time depends on it.

    python3 tools/make_branding.py
"""

import os
import shutil
import subprocess
import sys

from fontTools.misc.transform import Transform
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_HEADER = os.path.join(HERE, "src", "app_icon.h")
# The sizes a window manager actually asks for. _NET_WM_ICON holds several and
# the manager picks the best fit, so supplying the common ones exactly beats
# making it rescale one.
ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)
FONT = os.path.join(HERE, "external", "fonts", "SourceSerif4-Bold.ttf")
OUT = os.path.join(HERE, "share", "branding")

# The reader's own palette, so the identity and the pages agree.
INK = "#171614"        # --heading-text
TEAL = "#165d7a"       # --link, the warm theme's accent
PAPER = "#f4f1ea"      # --page-background

CAP_UNITS = 670        # Source Serif 4 cap height, in font units

# The mark: three lines of a page, narrowing, and the drop they come to.
# Authored on a 64 grid; the content bounding box is recorded so the lockup
# can place it without measuring paths at run time.
BARS = [(14, 10, 36, 4), (19, 19, 26, 4), (24, 28, 16, 4)]
DROP = "M32 36 C27 43 21 47 21 53 A11 11 0 0 0 43 53 C43 47 37 43 32 36 Z"
MARK_BBOX = (14, 10, 50, 64)

# The icon drops one line and grows the drop: three 4-unit bars turn to mush
# at 16 px, which is the size an icon has to survive.
ICON_BARS = [(17, 10, 30, 5), (23, 20, 18, 5)]
ICON_DROP = "M32 29 C27 36 21 40 21 46 A11 11 0 0 0 43 46 C43 40 37 36 32 29 Z"


def bars_svg(bars, indent):
    pad = " " * indent
    return "\n".join(
        f'{pad}<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{h / 2}"/>'
        for x, y, w, h in bars)


def wordmark(cap_px, x, baseline):
    """"Distill" as paths, and where the text ends."""
    font = TTFont(FONT)
    glyphs, cmap, hmtx = font.getGlyphSet(), font.getBestCmap(), font["hmtx"]
    scale = cap_px / CAP_UNITS
    paths = []
    for ch in "Distill":
        name = cmap[ord(ch)]
        pen = SVGPathPen(glyphs)
        # Font space is y-up and starts at the baseline; flip and place it.
        glyphs[name].draw(TransformPen(pen, Transform(scale, 0, 0, -scale, x, baseline)))
        commands = pen.getCommands()
        if commands:
            paths.append(commands)
        x += hmtx[name][0] * scale
    return paths, x


def write(name, body):
    path = os.path.join(OUT, name)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(body)
    print(f"wrote {os.path.relpath(path, HERE)}")


def make_mark():
    write("distill-mark.svg", f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64" role="img" aria-label="Distill">
  <title>Distill</title>
  <!-- A page narrowing to what is worth keeping. Inherits currentColor. -->
  <g fill="currentColor">
{bars_svg(BARS, 4)}
    <path d="{DROP}"/>
  </g>
</svg>
''')


def make_icon(name, background, ink, drop, rounded=True):
    plate = f'  <rect width="64" height="64" rx="14" fill="{background}"/>\n' if rounded else ""
    write(name, f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" width="64" height="64" role="img" aria-label="Distill">
  <title>Distill</title>
{plate}  <g fill="{ink}">
{bars_svg(ICON_BARS, 4)}
  </g>
  <path d="{ICON_DROP}" fill="{drop}"/>
</svg>
''')


def make_logo(name, bar_fill, drop_fill, text_fill):
    height = 64
    mx0, my0, mx1, my1 = MARK_BBOX
    mark_h = height - 18                       # breathing room above and below
    scale = mark_h / (my1 - my0)
    left = 12.0
    tx, ty = left - mx0 * scale, 9.0 - my0 * scale
    mark_w = (mx1 - mx0) * scale
    axis = 9.0 + mark_h / 2                    # mark and wordmark share it

    cap = mark_h * 0.52                        # calibrated so neither dominates
    paths, end = wordmark(cap, left + mark_w + mark_h * 0.34, axis + cap / 2)
    width = round(end + left, 1)
    glyphs = "\n".join(f'    <path d="{d}"/>' for d in paths)
    write(name, f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" role="img" aria-label="Distill">
  <title>Distill</title>
  <g transform="translate({tx:.3f} {ty:.3f}) scale({scale:.4f})">
    <g fill="{bar_fill}">
{bars_svg(BARS, 6)}
    </g>
    <path d="{DROP}" fill="{drop_fill}"/>
  </g>
  <g fill="{text_fill}">
{glyphs}
  </g>
</svg>
''')


def make_icon_header():
    """The icon as PNG bytes, for the GTK shell to hand the window manager.

    PNG rather than the SVG it came from: gdk-pixbuf has PNG built in, while
    SVG needs librsvg's loader, which is a runtime dependency this project
    would be taking on for one icon -- and on a machine without it the window
    would simply have no icon, with nothing to say why.

    Committed, so the build needs neither this script nor rsvg-convert.
    """
    if not shutil.which("rsvg-convert"):
        print("rsvg-convert not found; leaving src/app_icon.h as it is", file=sys.stderr)
        return
    source = os.path.join(OUT, "distill-icon.svg")
    blobs = []
    for size in ICON_SIZES:
        png = subprocess.run(
            ["rsvg-convert", "-w", str(size), "-h", str(size), source],
            check=True, capture_output=True).stdout
        blobs.append((size, png))

    lines = ['/* Generated by tools/make_branding.py -- do not edit.',
             ' *',
             ' * The application icon, one PNG per size, so the shell can set it on the',
             ' * window itself. On X11 that is the whole story: the bytes become',
             ' * _NET_WM_ICON and the task bar reads them, with no installed file and no',
             ' * desktop entry involved. A Wayland compositor ignores them and matches the',
             ' * application id against a desktop entry instead, so that case still needs',
             ' * one; see share/branding/README.md.',
             ' */',
             '#ifndef DISTILL_APP_ICON_H',
             '#define DISTILL_APP_ICON_H',
             '',
             '#include <stddef.h>',
             '']
    for size, png in blobs:
        lines.append(f"static const unsigned char distill_icon_{size}_png[] = {{")
        for at in range(0, len(png), 16):
            chunk = ", ".join(f"0x{b:02x}" for b in png[at:at + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("")
    lines.append("static const struct { const unsigned char *data; size_t length; } "
                 "distill_icon_pngs[] = {")
    for size, png in blobs:
        lines.append(f"    {{distill_icon_{size}_png, sizeof distill_icon_{size}_png}},")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    with open(ICON_HEADER, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")
    total = sum(len(png) for _, png in blobs)
    print(f"wrote {os.path.relpath(ICON_HEADER, HERE)} "
          f"({len(blobs)} sizes, {total} bytes of PNG)")


def main():
    if not os.path.isfile(FONT):
        sys.exit(f"missing {FONT}; external/ holds the bundled fonts")
    os.makedirs(OUT, exist_ok=True)
    make_mark()
    # Teal is the primary plate: a paper-coloured icon all but disappears on
    # the light surfaces a desktop usually has, and an icon has to hold an
    # edge at 16 px against a background nobody chose for it.
    make_icon("distill-icon.svg", TEAL, PAPER, PAPER)
    make_icon("distill-icon-light.svg", PAPER, INK, TEAL)
    make_logo("distill-logo.svg", INK, TEAL, INK)
    make_logo("distill-logo-mono.svg", "currentColor", "currentColor", "currentColor")
    make_icon_header()
    return 0


if __name__ == "__main__":
    sys.exit(main())
