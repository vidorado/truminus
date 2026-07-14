"""Geometry for the two OpenAir flap-mode icons (SWING / FIX).

Both the LCD icon font (main/font_icons.ttf, via gen_icon_font.py) and the web
UI (inline <svg> in data/index.html) render these, so the shapes are defined
once here as filled, closed contours in a 512x512 box with a Y-DOWN axis (SVG
convention).  Each icon is a list of contours; each contour is a list of
(x, y) points.  Fills use the non-zero winding rule, so overlapping
same-wound contours union cleanly (no need for hole handling).

  SWING  U+E901  two flap bars (top-horizontal + right-vertical) with a curved
                 arrow oscillating between them — mirrors the app's FLAPS icon.
  FIX    U+E902  three right-pointing arrows (steady airflow) — the app's FIX
                 icon.

Run this file directly to dump the SVG `d` strings for pasting into index.html
and to write a PNG preview (needs Pillow).
"""

import math

BOX = 512  # matches font_icons.ttf unitsPerEm (FA6)


def _arrow_right(yc, x_tail, x_head, x_tip, half_shaft, half_head):
    """A right-pointing arrow polygon: shaft rectangle + triangular head."""
    return [
        (x_tail, yc - half_shaft),
        (x_head, yc - half_shaft),
        (x_head, yc - half_head),
        (x_tip,  yc),
        (x_head, yc + half_head),
        (x_head, yc + half_shaft),
        (x_tail, yc + half_shaft),
    ]


def _arc(cx, cy, r, a_start_deg, a_end_deg, steps):
    """Sample a circular arc (degrees, Y-down) into a point list."""
    pts = []
    for i in range(steps + 1):
        a = math.radians(a_start_deg + (a_end_deg - a_start_deg) * i / steps)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def _arrow(tail, tip, half_shaft, half_head, head_len):
    """Arrow polygon from `tail` to `tip` along an arbitrary direction."""
    tx, ty = tail
    px, py = tip
    dx, dy = px - tx, py - ty
    L = math.hypot(dx, dy)
    ux, uy = dx / L, dy / L          # unit along the arrow
    nx, ny = -uy, ux                 # unit perpendicular
    hbx, hby = px - ux * head_len, py - uy * head_len   # head base centre
    def p(cx, cy, s):
        return (cx + nx * s, cy + ny * s)
    return [
        p(tx, ty,  half_shaft),
        p(hbx, hby,  half_shaft),
        p(hbx, hby,  half_head),
        (px, py),
        p(hbx, hby, -half_head),
        p(hbx, hby, -half_shaft),
        p(tx, ty, -half_shaft),
    ]


def fix_contours():
    """Three right arrows fanning from clustered tails; the middle one is longer."""
    t, h, hl = 15, 44, 74
    return [
        _arrow((172, 175), (336, 150), t, h, hl),   # top
        _arrow((150, 256), (352, 256), t, h, hl),   # middle
        _arrow((172, 337), (336, 362), t, h, hl),   # bottom
    ]


def swing_contours():
    oy = 50  # nudge the whole icon down so it sits centred in the cell
    cx, cy = 252, 168 + oy
    r_out, r_in = 112, 80

    # The two bars share the top-right corner square so they read as one L.
    top_bar   = [(96, 96 + oy), (312, 96 + oy), (312, 120 + oy), (96, 120 + oy)]
    right_bar = [(288, 96 + oy), (312, 96 + oy), (312, 296 + oy), (288, 296 + oy)]

    # Crescent band. Both ends stop short of their arrowhead, leaving a gap.
    band = _arc(cx, cy, r_out, 176, 100, 14) + _arc(cx, cy, r_in, 100, 176, 14)

    up_arrow    = [(156, 118 + oy), (184, 182 + oy), (128, 182 + oy)]   # points up
    right_arrow = [(316, 268 + oy), (256, 240 + oy), (256, 296 + oy)]   # points right

    return [top_bar, right_bar, band, up_arrow, right_arrow]


def _normalize(contours, target_frac=0.80):
    """Uniformly scale + centre contours to fill target_frac of the BOX.

    Keeps aspect ratio so the arrows/bars aren't distorted; centres the icon's
    bounding box in the cell so it reads balanced at small button sizes.
    """
    xs = [x for c in contours for x, _ in c]
    ys = [y for c in contours for _, y in c]
    w, h = max(xs) - min(xs), max(ys) - min(ys)
    s = (BOX * target_frac) / max(w, h)
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    off = BOX / 2
    return [[(off + (x - cx) * s, off + (y - cy) * s) for x, y in c]
            for c in contours]


def _norm(fn):
    return lambda: _normalize(fn())


ICONS = {
    0xE901: ("flapswing", _norm(swing_contours)),
    0xE902: ("flapfix",   _norm(fix_contours)),
}


def svg_path_d(contours):
    """Serialise contours to an SVG path `d` string (Y-down, as defined here)."""
    parts = []
    for c in contours:
        parts.append("M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in c) + " Z")
    return " ".join(parts)


if __name__ == "__main__":
    for cp, (name, fn) in ICONS.items():
        print(f"\n{name} (U+{cp:04X}):")
        print(svg_path_d(fn()))
    try:
        from PIL import Image, ImageDraw
        for cp, (name, fn) in ICONS.items():
            im = Image.new("RGBA", (BOX, BOX), (240, 242, 244, 255))
            d = ImageDraw.Draw(im)
            for c in fn():
                d.polygon(c, fill=(40, 46, 66, 255))
            out = f"/tmp/claude-1000/-home-victor-Documentos-proyectos-Personales-truminus/e89d805f-9e40-487c-87a5-277dd5c5cbc8/scratchpad/{name}_preview.png"
            im.save(out)
            print(f"  preview -> {out}")
    except ImportError:
        print("(Pillow not installed; skipped PNG preview)")
