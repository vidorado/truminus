"""
Generate main/font_icons.ttf — a merged subset of FontAwesome 6 Free Solid
and FontAwesome 6 Brands containing only the glyphs used by the TruMinus UI.

Sources (FA 6.7.2, included in repo):
  scripts/fonts/fa6-free-solid-900.otf   — solid icons
  scripts/fonts/fa6-brands-400.ttf       — brand icons (bluetooth)
  scripts/fonts/fa6-regular-400.ttf      — regular icons (currently unused)

Usage:
    python3 scripts/gen_icon_font.py

Requires fonttools:
    pip install fonttools brotli   # brotli only needed for woff2 output
"""

import sys
import tempfile
from pathlib import Path

try:
    from fontTools import subset as ss
    from fontTools.ttLib import TTFont
    from fontTools.merge import Merger
except ImportError:
    sys.exit("fonttools not found — run: pip install fonttools")

ROOT       = Path(__file__).parent.parent
SOLID_SRC  = ROOT / "scripts" / "fonts" / "fa6-free-solid-900.ttf"  # TTF (not OTF) for merge compat
BRANDS_SRC = ROOT / "scripts" / "fonts" / "fa6-brands-400.ttf"
DST        = ROOT / "main" / "font_icons.ttf"

# Glyphs from FA6 Free Solid — keep in sync with #define FA_* in p4display.cpp
# and p4settings.cpp.
SOLID_GLYPHS = {
    # p4display.cpp
    0xF002: "magnifying-glass  FA_SEARCH    (scan buttons)",
    0xE3AF: "house-chimney     FA_HOUSE_CHIM",
    0xF013: "cog               FA_COG",
    0xF043: "tint/water-drop   FA_TINT",
    0xF053: "chevron-left      FA_CHEVRON_L",
    0xF054: "chevron-right     FA_CHEVRON_R",
    0xF0D7: "caret-up          FA_CARET_U   (setpoint / fan-level)",
    0xF0D8: "caret-down        FA_CARET_D   (setpoint / fan-level)",
    0xF074: "shuffle/random    FA_RANDOM",
    0xF0E7: "bolt/lightning    FA_BOLT",
    0xF1EB: "wifi              FA_WIFI",
    0xF2C9: "thermometer-half  FA_THERM_HALF",
    0xF7E4: "fire-flame-curved FA_FIRE",
    0xF2DC: "snowflake         FA_SNOWFLAKE (A/C cool + snoweco component)",
    # p4settings.cpp — settings menu
    0xF0E0: "envelope          FA_ENVELOPE  (MQTT)",
    0xF185: "sun               FA_SUN       (Solar/BLE)",
    0xF108: "display/desktop   FA_DISPLAY   (Display settings)",
    0xF0AC: "globe             FA_GLOBE     (Language)",
    0xF0C2: "cloud             FA_CLOUD     (WSS reverse-tunnel status)",
    0xF019: "download          FA_DOWNLOAD  (firmware updates / self-OTA)",
    0xF071: "triangle-exclam.  FA_WARN      (status-bar alert prefix: No-LIN / A/C fault)",
    # p4display.cpp — INVERSOR port dynamic indicators
    0xE55F: "plug-circle-bolt  FA_PLUG_BOLT (AC mains connected)",
    0xF177: "arrow-left-long   FA_ARROW_L   (BAT discharging)",
    0xF178: "arrow-right-long  FA_ARROW_R   (BAT charging)",
    # p4settings.cpp — password toggle / navigation
    0xF06E: "eye               FA_EYE",
    0xF070: "eye-slash         FA_EYE_SLASH",
    0xF2F6: "right-to-bracket  FA_SIGN_IN",
    # LVGL built-in symbols used by lv_keyboard (LV_SYMBOL_*)
    # LV_SYMBOL_NEW_LINE (U+F8A2) is not in FA6 Free; it is aliased below to U+F2F6
    # so the Enter key renders as the right-to-bracket arrow (user request).
    0xF00C: "check             LV_SYMBOL_OK        (keyboard confirm / tick)",
    0xF00D: "xmark             LV_SYMBOL_CLOSE     (keyboard close / X)",
    0xF55A: "delete-left       LV_SYMBOL_BACKSPACE (keyboard backspace)",
    0xF11C: "keyboard          LV_SYMBOL_KEYBOARD  (mode-switch button in all kb maps)",
}

# Glyphs from FA6 Brands — bluetooth is brand-only, not in Solid.
BRANDS_GLYPHS = {
    0xF293: "bluetooth         FA_BLUETOOTH (BLE status)",
}

ALL_GLYPHS = {**SOLID_GLYPHS, **BRANDS_GLYPHS}
# Virtual alias verified separately (not in source fonts, added via cmap patch).
ALIAS_GLYPHS = {0xF8A2: "enter arrow (aliased from U+F2F6)  LV_SYMBOL_NEW_LINE"}
# Custom composite glyphs built from existing outlines (not in any source font).
CUSTOM_GLYPHS = {0xE900: "snowflake+ECO   FA_SNOWLEAF (A/C eco, snowflake + ECO text from font_regular)"}


def add_snoweco_glyph(dst: Path):
    """Build a custom 'snowflake + ECO' glyph at U+E900.

    Upper ~58% of the cell: snowflake (U+F2DC) from the icon font.
    Lower ~38%: letters E, C, O in bold, outlines taken from
    main/font_bold.ttf so they read clearly at small sizes.
    Both bands are shifted ~0.07 em down from the previous layout so the
    glyph sits centred in the button rather than appearing top-heavy.
    """
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    from fontTools.pens.transformPen import TransformPen

    font = TTFont(str(dst))
    cmap = font.getBestCmap()
    snow_name = cmap[0xF2DC]
    glyf = font["glyf"]
    gs   = font.getGlyphSet()
    em   = font["head"].unitsPerEm  # 512 for FA6

    reg_src  = Path(dst).parent.parent / "main" / "font_bold.ttf"
    reg_font = TTFont(str(reg_src))
    reg_cmap = reg_font.getBestCmap()
    reg_gs   = reg_font.getGlyphSet()
    reg_em   = reg_font["head"].unitsPerEm
    # Uniform baseline metrics so E, C, O all scale to the same visual height.
    # Using the per-glyph bbox would make O/C appear smaller than E because
    # their curves produce larger sh (overshoot / undershoot) → smaller fit().
    os2_tbl  = reg_font.get("OS/2")
    cap_h    = (os2_tbl.sCapHeight if (os2_tbl and os2_tbl.sCapHeight)
                else int(0.72 * reg_em))
    reg_hmtx = reg_font["hmtx"].metrics

    def bbox_fa(name):
        g = glyf[name]
        g.recalcBounds(glyf)
        return g.xMin, g.yMin, g.xMax, g.yMax

    def fit(b, t):
        sx0, sy0, sx1, sy1 = b
        tx0, ty0, tx1, ty1 = t
        sw, sh = sx1 - sx0, sy1 - sy0
        s = min((tx1 - tx0) / sw, (ty1 - ty0) / sh)
        dx = tx0 + ((tx1 - tx0) - sw * s) / 2 - sx0 * s
        dy = ty0 + ((ty1 - ty0) - sh * s) / 2 - sy0 * s
        return (s, 0, 0, s, dx, dy)

    pen = TTGlyphPen(gs)

    # Snowflake: upper band — slightly larger than before to give it more presence.
    gs[snow_name].draw(TransformPen(pen, fit(bbox_fa(snow_name),
                                             (0, int(0.27 * em), em, int(0.96 * em)))))

    # ECO letters (bold): lower band, zero inter-letter gap so the letters
    # sit as close together as the slot width allows (tighter kerning).
    eco_y0, eco_y1 = int(-0.15 * em), int(0.24 * em)
    gap    = 0
    slot_w = em // 3
    # All three letters must share the SAME source rectangle so fit() gives
    # them an identical scale factor.  Using each letter's own adv_w makes
    # 'O' (widest) smaller than 'E'/'C'.  Using the widest advance as a
    # common reference width equalises them.
    letter_names = [reg_cmap.get(ord(ch)) for ch in "ECO"]
    ref_w = max((reg_hmtx.get(n, (reg_em, 0))[0] for n in letter_names if n),
                default=reg_em)
    for i, gname in enumerate(letter_names):
        if not gname:
            continue
        x0 = i * (slot_w + gap)
        x1 = x0 + slot_w
        reg_gs[gname].draw(TransformPen(pen, fit((0, 0, ref_w, cap_h),
                                                 (x0, eco_y0, x1, eco_y1))))

    new_glyph = pen.glyph()
    new_glyph.recalcBounds(glyf)

    name = "snoweco"
    glyf[name] = new_glyph
    font["hmtx"][name] = (em, 0)
    order = font.getGlyphOrder()
    if name not in order:
        order.append(name)
        font.setGlyphOrder(order)
    font["maxp"].numGlyphs = len(font.getGlyphOrder())
    for sub in font["cmap"].tables:
        if hasattr(sub, "cmap"):
            sub.cmap[0xE900] = name
    font.save(str(dst))


def subset_font(src: Path, codepoints: list, label: str) -> str:
    opts = ss.Options()
    opts.layout_features = []
    opts.name_IDs        = [1, 2, 4, 5]
    opts.recalc_bounds   = True

    font      = ss.load_font(str(src), opts)
    subsetter = ss.Subsetter(opts)
    subsetter.populate(unicodes=codepoints)
    subsetter.subset(font)

    tmp = tempfile.mktemp(suffix=f"_{label}.ttf")
    ss.save_font(font, tmp, opts)
    return tmp


def main():
    for src in (SOLID_SRC, BRANDS_SRC):
        if not src.exists():
            sys.exit(f"Source font not found: {src}")

    solid_tmp  = subset_font(SOLID_SRC,  list(SOLID_GLYPHS.keys()),  "solid")
    brands_tmp = subset_font(BRANDS_SRC, list(BRANDS_GLYPHS.keys()), "brands")

    DST.parent.mkdir(parents=True, exist_ok=True)
    merger = Merger()
    merged = merger.merge([solid_tmp, brands_tmp])
    merged.save(str(DST))

    # Alias U+F8A2 (LV_SYMBOL_NEW_LINE) → same glyph as U+F2F6 (right-to-bracket).
    # FA6 has no glyph at F8A2; this makes the keyboard Enter key render as the
    # right-to-bracket arrow while LVGL's internal NEW_LINE logic still works.
    out = TTFont(str(DST))
    sign_in_glyph = out.getBestCmap().get(0xF2F6)
    if sign_in_glyph:
        for subtable in out['cmap'].tables:
            if hasattr(subtable, 'cmap'):
                subtable.cmap[0xF8A2] = sign_in_glyph
        out.save(str(DST))

    # Build the custom snowflake+ECO glyph (U+E900).
    add_snoweco_glyph(DST)

    # Verify
    out  = TTFont(str(DST))
    cmap = out.getBestCmap()
    print(f"Written: {DST}  ({DST.stat().st_size} bytes)")
    print()
    ok = True
    for cp, desc in sorted(ALL_GLYPHS.items()):
        found = cp in cmap
        src   = "brands" if cp in BRANDS_GLYPHS else "solid"
        print(f"  U+{cp:04X} [{src:6s}]  {'✓' if found else '✗ MISSING'}  {desc}")
        if not found:
            ok = False
    for cp, desc in sorted(ALIAS_GLYPHS.items()):
        found = cp in cmap
        print(f"  U+{cp:04X} [alias ]  {'✓' if found else '✗ MISSING'}  {desc}")
        if not found:
            ok = False
    for cp, desc in sorted(CUSTOM_GLYPHS.items()):
        found = cp in cmap
        print(f"  U+{cp:04X} [custom]  {'✓' if found else '✗ MISSING'}  {desc}")
        if not found:
            ok = False
    if not ok:
        sys.exit("\nSome glyphs are missing from the source fonts.")
    print("\nAll glyphs present.")


if __name__ == "__main__":
    main()
