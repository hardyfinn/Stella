#!/usr/bin/env python3
# Draws res/Stella.svg plus the knob, switch and button pictures.
# VCV Rack can't draw SVG <text>, so every label is turned into outline paths
# with fontTools. Positions are in mm and must match src/Stella.cpp.
#   pip install fonttools
#   python3 res/make_panel.py
import os
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
import haer_logo

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_BOLD = os.environ.get("PANEL_FONT_BOLD", "/usr/share/fonts/opentype/inter/Inter-Bold.otf")
FONT_MED = os.environ.get("PANEL_FONT_MED", "/usr/share/fonts/opentype/inter/Inter-Medium.otf")
FONT_BLACK = os.environ.get("PANEL_FONT_BLACK", "/usr/share/fonts/opentype/inter/Inter-Black.otf")

W, H = 182.88, 128.5  # 36 HP
BG = "#151B30"
PLATE = "#0D1122"
INK = "#EFE6D2"
DIM = "#9AA3BD"
LINE = "#262E4A"
VC = ["#FFB347", "#5FD3E6", "#FF7FB0", "#B6E05A"]

fonts = {}
def text(s, x, y, size, font=FONT_BOLD, fill=INK, anchor="middle", spacing=0.04):
    """SVG path for string s; (x, y) is the baseline point in mm."""
    if font not in fonts:
        f = TTFont(font)
        fonts[font] = (f.getGlyphSet(), f.getBestCmap(), f["head"].unitsPerEm)
    gs, cmap, upm = fonts[font]
    scale = size / upm
    track = spacing * upm
    names = [cmap[ord(c)] for c in s]
    width = sum(gs[n].width + track for n in names) - track
    x0 = x - width * scale / 2 if anchor == "middle" else (x - width * scale if anchor == "end" else x)
    pen = SVGPathPen(gs)
    cx = 0.0
    for n in names:
        gs[n].draw(TransformPen(pen, (scale, 0, 0, -scale, x0 + cx * scale, y)))
        cx += gs[n].width + track
    return f'<path fill="{fill}" d="{pen.getCommands()}"/>'

# ---- LAYOUT (mm) -------------------------------------------------------------
DISP = (5.0, 12.5, 78.0)              # x, y, size of the map screen
GY = 106.0                            # global row
AXIS_X, LOAD_X, DRIFT_X, DCV_X, GRAV_X, MIX_X = 10.0, 29.0, 41.5, 52.5, 65.5, 78.0
COLS = [111.0, 129.0, 147.0, 165.0]
ROWS = [("TRIG", 19.0, "j"), ("X", 31.0, "k"), ("X CV", 42.5, "j"), ("Y", 54.0, "k"), ("Y CV", 65.5, "j"),
        ("SPREAD", 77.0, "k"), ("CHANCE", 89.0, "k"), ("CHANCE CV", 100.5, "j"), ("OUT", 112.5, "o")]

p = []
add = p.append
add(f'<rect width="{W}" height="{H}" fill="{BG}"/>')
add(f'<rect width="{W}" height="2" fill="{PLATE}"/><rect y="{H-2}" width="{W}" height="2" fill="{PLATE}"/>')
add(text("STELLA", 5.0, 9.2, 6.0, FONT_BLACK, anchor="start", spacing=0.06))
add(text("SAMPLE MAP · 4 VOICES", 32.0, 9.0, 2.9, FONT_BOLD, DIM, "start"))
x, y, s = DISP
add(f'<rect x="{x-0.8}" y="{y-0.8}" width="{s+1.6}" height="{s+1.6}" rx="1.6" fill="#05070F" stroke="#3A4466" stroke-width="0.4"/>')

# global row
ly = GY - 7.5
add(text("AXIS", AXIS_X, ly, 3.2))
add(text("AUTO", AXIS_X + 4.0, GY - 1.6, 2.9, FONT_BOLD, DIM, "start"))
add(text("FIXED", AXIS_X + 4.0, GY + 3.6, 2.9, FONT_BOLD, DIM, "start"))
add(text("LOAD", LOAD_X, ly, 3.2))
add(text("DRIFT", DRIFT_X, ly, 3.2))
add(text("CV", DCV_X, ly, 2.9, fill=DIM))
add(text("GRAVITY", GRAV_X, ly, 3.2))
add(text("MIX", MIX_X, ly, 2.9))
add(f'<rect x="{MIX_X-4.8}" y="{GY-4.8}" width="9.6" height="9.6" rx="1.2" fill="{INK}"/>')

# voice grid
for i, cx in enumerate(COLS):
    add(f'<circle cx="{cx}" cy="9.6" r="3.1" fill="{VC[i]}"/>')
    add(text(str(i + 1), cx, 10.95, 3.8, FONT_BLACK, BG))
add(f'<rect x="{COLS[0]-8}" y="106.6" width="{COLS[-1]-COLS[0]+16}" height="11.8" rx="1.5" fill="{PLATE}"/>')
for name, y, kind in ROWS:
    add(f'<line x1="102.5" y1="{y}" x2="{COLS[-1]+8}" y2="{y}" stroke="{LINE}" stroke-width="0.25"/>')
    if kind == "k":
        add(text(name, 101.5, y + 1.15, 3.2, anchor="end"))
    else:
        add(text(name, 101.5, y + 1.05, 2.9, fill=INK if kind == "o" else DIM, anchor="end"))
    if kind == "o":
        for cx in COLS:
            add(f'<rect x="{cx-4.8}" y="{y-4.8}" width="9.6" height="9.6" rx="1.2" fill="{INK}"/>')

svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}mm" height="{H}mm" viewBox="0 0 {W} {H}">\n' + "\n".join(p) + "\n</svg>\n"
out = os.path.join(HERE, "Stella.svg")
open(out, "w").write(svg)
# Haer maker logo, bottom-left rail
haer_logo.add_to_file(out, x=17.6, y=126.0)

# ---- knobs, switch, button ---------------------------------------------------
def knob(path, col, d=9.2):
    r = d / 2
    open(path, "w").write(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{d}mm" height="{d}mm" viewBox="0 0 {d} {d}">'
        f'<circle cx="{r}" cy="{r}" r="{r-0.25}" fill="#2B3352" stroke="#4A5580" stroke-width="0.5"/>'
        f'<circle cx="{r}" cy="{r}" r="{r*0.78}" fill="#39436A"/>'
        f'<line x1="{r}" y1="{r*0.22}" x2="{r}" y2="{r*0.95}" stroke="{col}" stroke-width="0.85" stroke-linecap="round"/>'
        '</svg>\n')
knob(os.path.join(HERE, "Knob0.svg"), INK)
for i, c in enumerate(VC):
    knob(os.path.join(HERE, f"Knob{i+1}.svg"), c)

for frame, up in ((0, True), (1, False)):
    hy = 0.45 if up else 4.15
    open(os.path.join(HERE, f"Axis_{frame}.svg"), "w").write(
        '<svg xmlns="http://www.w3.org/2000/svg" width="3.4mm" height="8mm" viewBox="0 0 3.4 8">'
        f'<rect x="0.2" y="0.2" width="3" height="7.6" rx="0.8" fill="#2B3352" stroke="#4A5580" stroke-width="0.4"/>'
        f'<rect x="0.6" y="{hy}" width="2.2" height="3.4" rx="0.5" fill="{INK}"/></svg>\n')
for frame, col in ((0, INK), (1, VC[0])):
    open(os.path.join(HERE, f"Load_{frame}.svg"), "w").write(
        '<svg xmlns="http://www.w3.org/2000/svg" width="7mm" height="7mm" viewBox="0 0 7 7">'
        f'<circle cx="3.5" cy="3.5" r="3.25" fill="#39436A" stroke="#4A5580" stroke-width="0.5"/>'
        f'<circle cx="3.5" cy="3.5" r="2.2" fill="{col}"/></svg>\n')
print("wrote", out)
