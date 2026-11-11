"""Draw the six design screens into docs/screens.svg for the README.

Text is converted to outlines, because GitHub does not load web fonts in SVG images.
Usage: uv run --with fonttools python tools/mkscreens.py <dir with Montserrat-*.ttf>
"""
import sys
from pathlib import Path

from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont

src = Path(sys.argv[1])
FONTS = {w: TTFont(src / f"Montserrat-{n}.ttf") for w, n in ((500, "Medium"), (600, "SemiBold"), (700, "Bold"))}

WHITE, GREY, DIM, GREEN, DARKGREEN = "#fff", "#93949e", "#6f7080", "#0fde41", "#0b5c26"
TRACK, SEG, BG = "#1c2030", "#2a2e40", "#020318"

ONCE = "M3 21q-.825 0-1.412-.587T1 19V5q0-.825.588-1.412T3 3h18q.825 0 1.413.588T23 5v14q0 .825-.587 1.413T21 21zm0-2h18V5H3zm0 0V5zM7 9v7q0 .425.288.713T8 17t.713-.288T9 16V8q0-.425-.288-.712T8 7H6q-.425 0-.712.288T5 8t.288.713T6 9zm8.25 4.825l1.5 2.725q.125.2.325.325t.45.125q.5 0 .75-.437t0-.863L16.25 12l2.05-3.7q.25-.425-.012-.862T17.525 7q-.25 0-.45.125t-.325.325l-1.5 2.725l-1.5-2.725q-.125-.2-.325-.325T12.975 7q-.5 0-.75.438t0 .862L14.25 12l-2.05 3.7q-.25.425.013.863t.762.437q.25 0 .45-.125t.325-.325z"
ALWAYS = "M5.5 17.5q-2.3 0-3.9-1.6T0 12t1.6-3.9t3.9-1.6q.925 0 1.775.325T8.8 7.75l1.7 1.55L9 10.65l-1.55-1.4q-.4-.35-.9-.55T5.5 8.5q-1.45 0-2.475 1.025T2 12t1.025 2.475T5.5 15.5q.55 0 1.05-.2t.9-.55l7.75-7q.675-.6 1.525-.925T18.5 6.5q2.3 0 3.9 1.6T24 12t-1.6 3.9t-3.9 1.6q-.925 0-1.775-.325T15.2 16.25l-1.7-1.55l1.5-1.35l1.55 1.4q.4.35.9.55t1.05.2q1.45 0 2.475-1.025T22 12t-1.025-2.475T18.5 8.5q-.55 0-1.05.2t-.9.55l-7.75 7q-.675.6-1.525.925T5.5 17.5"
CARD = '<rect x="2" y="5" width="20" height="14" rx="2"/><path d="M9 15a4 4 0 0 0 0-6M12.5 17a7 7 0 0 0 0-10M16 18.5a10 10 0 0 0 0-13"/>'


def width(s, size, weight):
    font = FONTS[weight]
    glyphs, cmap = font.getGlyphSet(), font.getBestCmap()
    return sum(glyphs[cmap[ord(c)]].width for c in s) * size / font["head"].unitsPerEm


def outline(s, x, y, size, weight, fill):
    """One run of text as a path, starting at x on baseline y."""
    font = FONTS[weight]
    glyphs, cmap = font.getGlyphSet(), font.getBestCmap()
    k = size / font["head"].unitsPerEm
    d = []
    for c in s:
        glyph = glyphs[cmap[ord(c)]]
        pen = SVGPathPen(glyphs, ntos=lambda v: f"{v:.1f}")
        glyph.draw(TransformPen(pen, (k, 0, 0, -k, x, y)))
        d.append(pen.getCommands())
        x += glyph.width * k
    return f'<path fill="{fill}" d="{"".join(d)}"/>'


def line(y, *runs, x=240, anchor="middle", gap=8):
    """Runs of (text, size, weight, fill) on one baseline, anchored at x."""
    total = sum(width(s, size, w) for s, size, w, _ in runs) + gap * (len(runs) - 1)
    pos = x - {"middle": total / 2, "end": total, "start": 0}[anchor]
    out = []
    for s, size, w, fill in runs:
        out.append(outline(s, pos, y, size, w, fill))
        pos += width(s, size, w) + gap
    return "".join(out), pos - gap


def text(y, s, size, weight, fill, **kw):
    return line(y, (s, size, weight, fill), **kw)[0]


def ring(stroke, dash, length, rotate, offset=0, cap=False):
    return (
        f'<circle cx="240" cy="240" r="222" fill="none" stroke="{stroke}" stroke-width="20" '
        f'pathLength="{length}" stroke-dasharray="{dash}" stroke-dashoffset="{offset}" '
        f'{"stroke-linecap=\"round\" " if cap else ""}transform="rotate({rotate} 240 240)"/>'
    )


def icon(path, x, y, size, fill):
    return f'<path fill="{fill}" transform="translate({x:.1f},{y}) scale({size / 24})" d="{path}"/>'


def home():
    return "".join([
        ring(TRACK, "270 360", 360, 135, cap=True),
        ring(GREEN, "167.4 360", 360, 135, cap=True),
        f'<circle cx="459.3" cy="205.3" r="8" fill="{BG}" stroke="{WHITE}" stroke-width="4"/>',
        text(132, "Garage", 22, 500, GREY),
        '<rect x="187" y="147" width="106" height="36" rx="18" fill="#0b3d1c"/>',
        text(173, "Smart", 22, 600, GREEN),
        line(283, ("7.4", 104, 700, WHITE), ("kW", 30, 600, GREY))[0],
        text(328, "Ioniq 5 · 62 %", 26, 600, WHITE),
        text(362, "80 % at 14:20", 22, 500, GREY),
    ])


def mode():
    big, end = line(264, ("Smart", 64, 700, GREEN), x=200)
    small, end2 = line(318, ("Smart", 30, 600, GREY), x=219)
    return "".join([
        ring(SEG, "46 10 46 10 46 10 46 10 46 360", 360, 135, cap=True),
        ring(GREEN, "46 360", 360, 135, offset=-112, cap=True),
        f'<path fill="{DIM}" d="M240 120l16 20h-32z"/>',
        text(183, "Smart", 30, 600, GREY),
        big, icon(ONCE, end + 16, 208, 64, GREEN),
        small, icon(ALWAYS, end2 + 10, 292, 32, GREY),
        f'<path fill="{DIM}" d="M224 340h32l-16 20z"/>',
    ])


def limit():
    return "".join([
        ring(TRACK, "270 360", 360, 135, cap=True),
        ring(DARKGREEN, "216 360", 360, 135, cap=True),
        ring(GREEN, "167.4 360", 360, 135, cap=True),
        f'<circle cx="459.3" cy="205.3" r="16" fill="{WHITE}" stroke="{BG}" stroke-width="4"/>',
        text(145, "Limit", 22, 500, GREY),
        line(265, ("80", 120, 700, WHITE), ("%", 36, 600, GREY))[0],
        text(310, "62 % now · +13 kWh", 24, 600, WHITE),
        text(351, "Press to set", 20, 500, GREY),
    ])


def flow():
    rows = [("Grid", "0.4 kW"), ("Home", "1.1 kW"), ("Car", "4.6 kW"), ("Battery", "+0.5 kW")]
    return "".join([
        ring(GREEN, "92.5 100", 100, -90),
        ring(GREY, "5.5 100", 100, -90, offset=-93.5),
        line(170, ("5.8", 72, 700, GREEN), ("kW solar", 26, 600, GREY))[0],
        *[text(212 + i * 39, label, 24, 600, GREY, x=90, anchor="start") for i, (label, _) in enumerate(rows)],
        *[text(212 + i * 39, value, 24, 600, WHITE, x=390, anchor="end") for i, (_, value) in enumerate(rows)],
        text(369, "Battery 71 %", 22, 500, GREY),
    ])


def plan():
    return "".join([
        ring(SEG, "0.9 0.1", 24, -90),
        ring(DARKGREEN, "0.9 0.1 0.9 0.1 0.9 0.1 0.9 0.1 0.9 24", 24, -90, offset=-11),
        ring(GREEN, "0.9 0.1 0.9 0.1 0.9 0.1 0.9 24", 24, -90, offset=-1),
        f'<circle cx="120.7" cy="84.5" r="4" fill="{WHITE}"/>',
        text(158, "Ready by", 22, 500, GREY),
        text(250, "07:00", 96, 700, WHITE),
        text(300, "80 % · 01:00 to 05:00", 26, 600, WHITE),
        text(337, "21 ct/kWh · now 34 ct", 22, 500, GREY),
    ])


def rfid():
    return "".join([
        f'<circle cx="240" cy="240" r="222" fill="none" stroke="{GREEN}" stroke-width="20"/>',
        f'<g transform="translate(204,107) scale(3)" fill="none" stroke="{GREEN}" stroke-width="1.8" '
        f'stroke-linecap="round" stroke-linejoin="round">{CARD}</g>',
        text(211, "Vehicle", 22, 500, GREY),
        text(280, "Ioniq 5", 60, 700, WHITE),
        text(327, "Assigned to Garage", 26, 600, GREEN),
        text(367, "Smart · limit 80 %", 22, 500, GREY),
    ])


SCREENS = [
    (home, "Home"), (mode, "Rotate: mode"), (limit, "Press, rotate: limit"),
    (flow, "Swipe: energy flow"), (plan, "Swipe: plan and prices"), (rfid, "RFID tag: vehicle"),
]

out = [
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1080 800" width="1080" height="800">',
    "<title>smart-knob screens: home, mode, limit, energy flow, plan, RFID</title>",
]
for i, (draw, caption) in enumerate(SCREENS):
    x, y = i % 3 * 360, i // 3 * 400
    # each dial is drawn at 2x in a 720 box: knob ring, round screen, then the content
    out.append(
        f'<g transform="translate({x},{y}) scale(.5)">'
        '<circle cx="360" cy="360" r="320" fill="#26272f" stroke="#3a3b45" stroke-width="2"/>'
        f'<circle cx="360" cy="360" r="240" fill="{BG}"/>'
        f'<g transform="translate(120,120)">{draw()}</g></g>'
    )
    out.append(text(y + 372, caption, 16, 600, "#808080", x=x + 180))
out.append("</svg>")
(Path(__file__).parent.parent / "docs" / "screens.svg").write_text("\n".join(out) + "\n")
